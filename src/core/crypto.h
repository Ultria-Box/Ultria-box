#pragma once
// Crypto primitives via Windows CNG (bcrypt.dll).
// AES-256-GCM (Chromium v10/v11/v20), AES-CBC (Gecko), 3DES-CBC (Gecko legacy),
// SHA-1/256, HMAC-SHA256, PBKDF2.
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace crypto {

using Bytes = std::vector<uint8_t>;

// ---- one-shot hashes ----
Bytes sha1(const Bytes& data);
Bytes sha256(const Bytes& data);
Bytes sha1_str(const std::string& s);
Bytes sha256_str(const std::string& s);

// ---- HMAC ----
Bytes hmac_sha256(const Bytes& key, const Bytes& data);

// ---- PBKDF2 ----
Bytes pbkdf2_sha1(const Bytes& password, const Bytes& salt, uint32_t iterations, size_t dklen);
Bytes pbkdf2_sha256(const Bytes& password, const Bytes& salt, uint32_t iterations, size_t dklen);

// ---- AES-256-GCM ----
// decrypt: ct = ciphertext (without tag), tag = 16 bytes, nonce 12 bytes.
std::optional<Bytes> aes_gcm_decrypt(const Bytes& key, const Bytes& nonce, const Bytes& ct,
                                     const Bytes& tag, const Bytes& aad = {});
// encrypt: returns {ciphertext, tag}
std::optional<std::pair<Bytes, Bytes>> aes_gcm_encrypt(const Bytes& key, const Bytes& nonce,
                                                       const Bytes& pt, const Bytes& aad = {});

// ---- AES-CBC (no padding handling; caller strips) ----
std::optional<Bytes> aes_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, bool pkcs7);
// ---- 3DES-CBC ----
std::optional<Bytes> des3_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, bool pkcs7);

// ---- random ----
Bytes random_bytes(size_t n);

// ---- convenience: string -> bytes ----
inline Bytes to_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string to_string(const Bytes& b) { return std::string(b.begin(), b.end()); }

} // namespace crypto
