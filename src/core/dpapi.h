#pragma once
// DPAPI wrappers (crypt32.dll). User-scope calls decrypt data tied to the
// logged-on user, which is exactly how Chromium/Electron stores secrets.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dpapi {

using Bytes = std::vector<uint8_t>;

std::optional<Bytes> unprotect(const Bytes& data, const Bytes& entropy = {});
std::optional<Bytes> protect(const Bytes& data, const Bytes& entropy = {});

// Convenience: base64 string -> unprotect -> string
std::optional<std::string> unprotect_b64_to_string(const std::string& b64);

} // namespace dpapi
