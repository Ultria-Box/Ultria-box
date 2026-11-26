#pragma once
// Minimal JSON helpers built on nlohmann/json, for Local State / logins.json etc.
#include "../../third_party/json.hpp"
#include <fstream>
#include <string>

namespace util {

using json = nlohmann::json;

inline bool read_json_file(const std::string& path, json& out) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        out = json::parse(f);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace util
