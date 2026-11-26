#pragma once
// Shared combo parsing (url:login:pass) for the studio CLI/GUI.
// Same accepted formats as the checker UI; kept dependency-free.
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace combo {

struct Entry {
    std::string url, login, pass;
};

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

inline std::vector<std::string> split_keep(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// accepts url:login:pass (and | ; separators), https://host:login:pass, login:pass
inline bool parse_line(const std::string& raw, Entry& out) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return false;
    if (line.find(':') == std::string::npos && line.find('|') == std::string::npos &&
        line.find(';') == std::string::npos)
        return false;

    char sep = ':';
    if (line.find(" | ") != std::string::npos) sep = '|';
    else if (line.find(';') != std::string::npos && line.find(':') == std::string::npos) sep = ';';

    std::string url, login, pass;
    auto scheme = line.find("://");
    if (scheme != std::string::npos) {
        size_t start = scheme + 3;
        size_t cut = line.find(':', start);
        if (cut == std::string::npos) return false;
        url = line.substr(0, cut);
        std::string rest = line.substr(cut + 1);
        auto p1 = rest.find(':');
        if (p1 == std::string::npos) { login = rest; }
        else { login = rest.substr(0, p1); pass = rest.substr(p1 + 1); }
    } else {
        auto parts = split_keep(line, sep);
        if (parts.size() >= 3) {
            url = parts[0];
            login = parts[1];
            pass = parts[2];
            for (size_t i = 3; i < parts.size(); ++i) pass += sep + parts[i];
        } else if (parts.size() == 2) {
            if (parts[0].find('.') != std::string::npos && parts[0].find('@') == std::string::npos) {
                url = parts[0];
                login = parts[1];
            } else {
                login = parts[0];
                pass = parts[1];
            }
        } else {
            return false;
        }
    }
    url = trim(url); login = trim(login); pass = trim(pass);
    if (url.find("://") != std::string::npos) url = url.substr(url.find("://") + 3);
    while (!url.empty() && url.back() == '/') url.pop_back();
    url = lower(url);
    if (url.empty() && login.empty()) return false;
    out = Entry{url, login, pass};
    return true;
}

inline std::string to_line(const Entry& e) {
    return e.url + ":" + e.login + ":" + e.pass;
}

// host extraction for stats (strip path and port)
inline std::string host_of(const std::string& url) {
    std::string h = url;
    auto slash = h.find('/');
    if (slash != std::string::npos) h = h.substr(0, slash);
    auto colon = h.rfind(':');
    if (colon != std::string::npos && colon > 0 && h.find(']') == std::string::npos)
        h = h.substr(0, colon);
    return h;
}

} // namespace combo
