#include "crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

namespace crypto {

namespace {

struct AlgProvider {
    BCRYPT_ALG_HANDLE h = nullptr;
    explicit AlgProvider(LPCWSTR alg, ULONG flags = 0) {
        if (BCryptOpenAlgorithmProvider(&h, alg, nullptr, flags) < 0)
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }
    ~AlgProvider() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
    AlgProvider(const AlgProvider&) = delete;
    AlgProvider& operator=(const AlgProvider&) = delete;
};

Bytes hash_generic(LPCWSTR alg, const Bytes& data) {
    AlgProvider prov(alg);
    DWORD obj_len = 0, cb = 0, hash_len = 0;
    BCryptGetProperty(prov.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &cb, 0);
    BCryptGetProperty(prov.h, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len), &cb, 0);

    std::vector<uint8_t> obj(obj_len), digest(hash_len);
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptCreateHash(prov.h, &hh, obj.data(), (ULONG)obj.size(), nullptr, 0, 0) < 0)
        return {};
    BCryptHashData(hh, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    BCryptFinishHash(hh, digest.data(), (ULONG)digest.size(), 0);
    BCryptDestroyHash(hh);
    return digest;
}

Bytes pbkdf2_generic(LPCWSTR hmac_alg, const Bytes& password, const Bytes& salt,
                     uint32_t iterations, size_t dklen) {
    AlgProvider prov(hmac_alg, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Bytes out(dklen);
    if (BCryptDeriveKeyPBKDF2(prov.h,
            (PUCHAR)password.data(), (ULONG)password.size(),
            (PUCHAR)salt.data(), (ULONG)salt.size(),
            iterations, out.data(), (ULONG)out.size(), 0) < 0)
        return {};
    return out;
}

// Symmetric decrypt helper for CBC-style modes.
std::optional<Bytes> cbc_decrypt_generic(LPCWSTR alg, const Bytes& key, const Bytes& iv,
                                         const Bytes& ct, DWORD flags) {
    try {
        AlgProvider prov(alg);
        if (BCryptSetProperty(prov.h, BCRYPT_CHAINING_MODE,
                (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) < 0)
            return std::nullopt;

        DWORD obj_len = 0, cb = 0;
        BCryptGetProperty(prov.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &cb, 0);

        std::vector<uint8_t> obj(obj_len);
        BCRYPT_KEY_HANDLE kh = nullptr;
        if (BCryptGenerateSymmetricKey(prov.h, &kh, obj.data(), (ULONG)obj.size(),
                (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0)
            return std::nullopt;

        Bytes out(ct.size() + 16);
        ULONG done = 0;
        std::vector<uint8_t> iv_copy = iv;
        NTSTATUS st = BCryptDecrypt(kh, (PUCHAR)ct.data(), (ULONG)ct.size(),
                                    nullptr, iv_copy.data(), (ULONG)iv_copy.size(),
                                    out.data(), (ULONG)out.size(), &done, flags);
        BCryptDestroyKey(kh);
        if (st < 0) return std::nullopt;
        out.resize(done);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

Bytes sha1(const Bytes& data)   { return hash_generic(BCRYPT_SHA1_ALGORITHM, data); }
Bytes sha256(const Bytes& data) { return hash_generic(BCRYPT_SHA256_ALGORITHM, data); }
Bytes sha1_str(const std::string& s)   { return sha1(to_bytes(s)); }
Bytes sha256_str(const std::string& s) { return sha256(to_bytes(s)); }

Bytes hmac_sha256(const Bytes& key, const Bytes& data) {
    try {
        AlgProvider prov(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        DWORD obj_len = 0, cb = 0, hash_len = 0;
        BCryptGetProperty(prov.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &cb, 0);
        BCryptGetProperty(prov.h, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len), &cb, 0);
        std::vector<uint8_t> obj(obj_len), digest(hash_len);
        BCRYPT_HASH_HANDLE hh = nullptr;
        if (BCryptCreateHash(prov.h, &hh, obj.data(), (ULONG)obj.size(),
                (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0)
            return {};
        BCryptHashData(hh, (PUCHAR)data.data(), (ULONG)data.size(), 0);
        BCryptFinishHash(hh, digest.data(), (ULONG)digest.size(), 0);
        BCryptDestroyHash(hh);
        return digest;
    } catch (...) {
        return {};
    }
}

Bytes pbkdf2_sha1(const Bytes& p, const Bytes& s, uint32_t it, size_t n)   { return pbkdf2_generic(BCRYPT_SHA1_ALGORITHM, p, s, it, n); }
Bytes pbkdf2_sha256(const Bytes& p, const Bytes& s, uint32_t it, size_t n) { return pbkdf2_generic(BCRYPT_SHA256_ALGORITHM, p, s, it, n); }

std::optional<Bytes> aes_gcm_decrypt(const Bytes& key, const Bytes& nonce, const Bytes& ct,
                                     const Bytes& tag, const Bytes& aad) {
    try {
        AlgProvider prov(BCRYPT_AES_ALGORITHM);
        if (BCryptSetProperty(prov.h, BCRYPT_CHAINING_MODE,
                (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0)
            return std::nullopt;

        DWORD obj_len = 0, cb = 0;
        BCryptGetProperty(prov.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &cb, 0);
        std::vector<uint8_t> obj(obj_len);
        BCRYPT_KEY_HANDLE kh = nullptr;
        if (BCryptGenerateSymmetricKey(prov.h, &kh, obj.data(), (ULONG)obj.size(),
                (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0)
            return std::nullopt;

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO aci;
        BCRYPT_INIT_AUTH_MODE_INFO(aci);
        std::vector<uint8_t> nonce_copy = nonce, tag_copy = tag;
        aci.pbNonce = nonce_copy.data();
        aci.cbNonce = (ULONG)nonce_copy.size();
        aci.pbTag = tag_copy.data();
        aci.cbTag = (ULONG)tag_copy.size();
        aci.pbAuthData = aad.empty() ? nullptr : (PUCHAR)aad.data();
        aci.cbAuthData = (ULONG)aad.size();

        Bytes out(ct.size());
        ULONG done = 0;
        NTSTATUS st = BCryptDecrypt(kh, (PUCHAR)ct.data(), (ULONG)ct.size(),
                                    &aci, nullptr, 0, out.data(), (ULONG)out.size(), &done, 0);
        BCryptDestroyKey(kh);
        if (st < 0) return std::nullopt;
        out.resize(done);
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::pair<Bytes, Bytes>> aes_gcm_encrypt(const Bytes& key, const Bytes& nonce,
                                                       const Bytes& pt, const Bytes& aad) {
    try {
        AlgProvider prov(BCRYPT_AES_ALGORITHM);
        if (BCryptSetProperty(prov.h, BCRYPT_CHAINING_MODE,
                (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0)
            return std::nullopt;

        DWORD obj_len = 0, cb = 0;
        BCryptGetProperty(prov.h, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_len, sizeof(obj_len), &cb, 0);
        std::vector<uint8_t> obj(obj_len);
        BCRYPT_KEY_HANDLE kh = nullptr;
        if (BCryptGenerateSymmetricKey(prov.h, &kh, obj.data(), (ULONG)obj.size(),
                (PUCHAR)key.data(), (ULONG)key.size(), 0) < 0)
            return std::nullopt;

        Bytes tag(16), out(pt.size());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO aci;
        BCRYPT_INIT_AUTH_MODE_INFO(aci);
        std::vector<uint8_t> nonce_copy = nonce;
        aci.pbNonce = nonce_copy.data();
        aci.cbNonce = (ULONG)nonce_copy.size();
        aci.pbTag = tag.data();
        aci.cbTag = (ULONG)tag.size();
        aci.pbAuthData = aad.empty() ? nullptr : (PUCHAR)aad.data();
        aci.cbAuthData = (ULONG)aad.size();

        ULONG done = 0;
        NTSTATUS st = BCryptEncrypt(kh, (PUCHAR)pt.data(), (ULONG)pt.size(),
                                    &aci, nullptr, 0, out.data(), (ULONG)out.size(), &done, 0);
        BCryptDestroyKey(kh);
        if (st < 0) return std::nullopt;
        out.resize(done);
        return std::make_pair(out, tag);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Bytes> aes_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, bool pkcs7) {
    auto out = cbc_decrypt_generic(BCRYPT_AES_ALGORITHM, key, iv, ct, pkcs7 ? BCRYPT_BLOCK_PADDING : 0);
    if (!out || !pkcs7 || out->empty()) return out;
    // CNG strips padding only when the provider supports it; ensure manual strip as fallback.
    uint8_t pad = out->back();
    if (pad > 0 && pad <= 16 && pad <= out->size()) out->resize(out->size() - pad);
    return out;
}

std::optional<Bytes> des3_cbc_decrypt(const Bytes& key, const Bytes& iv, const Bytes& ct, bool pkcs7) {
    return cbc_decrypt_generic(BCRYPT_3DES_ALGORITHM, key, iv, ct, pkcs7 ? BCRYPT_BLOCK_PADDING : 0);
}

Bytes random_bytes(size_t n) {
    Bytes out(n);
    if (BCryptGenRandom(nullptr, out.data(), (ULONG)out.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        out.clear();
    return out;
}

} // namespace crypto
