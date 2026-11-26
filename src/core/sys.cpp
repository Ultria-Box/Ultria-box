#include "sys.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace util {

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

std::string expand_env(const std::string& in) {
    std::wstring w = widen(in);
    DWORD n = ExpandEnvironmentStringsW(w.c_str(), nullptr, 0);
    if (!n) return in;
    std::wstring out(n, L'\0');
    ExpandEnvironmentStringsW(w.c_str(), out.data(), n);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return narrow(out);
}

std::string env(const std::string& name) {
    std::wstring wname = widen(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    DWORD got = GetEnvironmentVariableW(wname.c_str(), out.data(), n);
    out.resize(got);
    return narrow(out);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string filename_of(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string dirname_of(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? "" : path.substr(0, p);
}

std::string temp_dir() {
    wchar_t buf[MAX_PATH + 1] = {};
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (!n) return "C:\\Windows\\Temp";
    std::wstring w(buf, n);
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    return narrow(w);
}

std::string exe_path() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return narrow(std::wstring(buf, n));
}

bool path_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool is_dir(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart > (int64_t)512 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)li.QuadPart);
    DWORD total = 0;
    while (total < out.size()) {
        DWORD chunk = 0;
        if (!ReadFile(h, out.data() + total, (DWORD)std::min<size_t>(out.size() - total, 1u << 20), &chunk, nullptr) || chunk == 0)
            break;
        total += chunk;
    }
    CloseHandle(h);
    out.resize(total);
    return true;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    mkdirs(dirname_of(path));
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() ||
              (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
    CloseHandle(h);
    return ok;
}

bool append_file(const std::string& path, const std::vector<uint8_t>& data) {
    mkdirs(dirname_of(path));
    HANDLE h = CreateFileW(widen(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() ||
              (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
    CloseHandle(h);
    return ok;
}

bool mkdirs(const std::string& path) {
    if (path.empty() || is_dir(path)) return true;
    std::error_code ec;
    fs::create_directories(widen(path), ec);
    return !ec || is_dir(path);
}

bool copy_file_to(const std::string& from, const std::string& to) {
    mkdirs(dirname_of(to));
    return CopyFileW(widen(from).c_str(), widen(to).c_str(), FALSE) != 0;
}

bool copy_dir_recursive(const std::string& from, const std::string& to,
                        const std::vector<std::string>& include_patterns, size_t max_file_size) {
    std::error_code ec;
    if (!fs::exists(widen(from), ec)) return false;
    bool any = false;
    for (auto it = fs::recursive_directory_iterator(widen(from), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.file_size(ec) > max_file_size) continue;
        std::wstring wpath = entry.path().wstring();
        std::string fname = narrow(entry.path().filename().wstring());
        if (!include_patterns.empty()) {
            bool match = false;
            for (const auto& p : include_patterns) {
                if (icontains(fname, p) || (p.size() > 1 && p[0] == '*' && ends_with(to_lower(fname), to_lower(p.substr(1))))) {
                    match = true;
                    break;
                }
            }
            if (!match) continue;
        }
        fs::path rel = fs::relative(entry.path(), widen(from), ec);
        std::string dest = join(to, narrow(rel.wstring()));
        if (copy_file_to(narrow(wpath), dest)) any = true;
    }
    return any;
}

bool remove_path(const std::string& path) {
    std::error_code ec;
    fs::remove_all(widen(path), ec);
    return !ec;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(join(path, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(join(path, narrow(name)));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::string> list_subdirs(const std::string& path) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(widen(join(path, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            out.push_back(join(path, narrow(name)));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::string> walk_files(const std::string& root, int max_depth,
                                    const std::vector<std::string>& patterns) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(widen(root), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() > max_depth) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        std::string fname = narrow(it->path().filename().wstring());
        if (!patterns.empty()) {
            bool match = false;
            for (const auto& p : patterns) {
                std::string lp = to_lower(p), lf = to_lower(fname);
                if (p[0] == '*') { if (ends_with(lf, lp.substr(1))) { match = true; break; } }
                else if (icontains(lf, lp)) { match = true; break; }
            }
            if (!match) continue;
        }
        out.push_back(narrow(it->path().wstring()));
    }
    return out;
}

std::string to_lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); }); return s; }
std::string to_upper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::toupper(c); }); return s; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

bool icontains(const std::string& hay, const std::string& needle) {
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string now_str() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

bool is_elevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION el{};
        DWORD sz = sizeof(el);
        if (GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz))
            elevated = el.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

} // namespace util
