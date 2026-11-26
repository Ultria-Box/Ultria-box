#pragma once
// Filesystem / environment / string helpers (UTF-8 flavored on the API surface).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace util {

// ---- conversions ----
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

// ---- paths / environment ----
std::string expand_env(const std::string& in);          // %LOCALAPPDATA%\... -> C:\Users\...
std::string env(const std::string& name);               // "" if unset
std::string join(const std::string& a, const std::string& b);
std::string filename_of(const std::string& path);
std::string dirname_of(const std::string& path);
std::string temp_dir();
std::string exe_path();

// ---- fs primitives ----
bool path_exists(const std::string& path);
bool is_dir(const std::string& path);
bool read_file(const std::string& path, std::vector<uint8_t>& out);
bool write_file(const std::string& path, const std::vector<uint8_t>& data);
bool append_file(const std::string& path, const std::vector<uint8_t>& data);
bool mkdirs(const std::string& path);
bool copy_file_to(const std::string& from, const std::string& to);
bool copy_dir_recursive(const std::string& from, const std::string& to,
                        const std::vector<std::string>& include_patterns = {},
                        size_t max_file_size = 100ull * 1024 * 1024);
bool remove_path(const std::string& path);

// list files in dir (non recursive); returns full paths
std::vector<std::string> list_dir(const std::string& path);
// list subdirectories (non recursive)
std::vector<std::string> list_subdirs(const std::string& path);
// walk recursively collecting files matching any pattern (glob-ish: substring or suffix with '*')
std::vector<std::string> walk_files(const std::string& root, int max_depth = 6,
                                    const std::vector<std::string>& patterns = {});

// ---- string utils ----
std::string to_lower(std::string s);
std::string to_upper(std::string s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool icontains(const std::string& hay, const std::string& needle);
std::vector<std::string> split(const std::string& s, char sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
std::string now_str();

// realtime protection / analysis hint: current process token elevation
bool is_elevated();

} // namespace util
