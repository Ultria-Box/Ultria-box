// Seal test material for the Wine fixture (run this under wine).
// Generates a genuine Chromium-style key hierarchy:
//   - 32-byte AES-256 key, protected with DPAPI (user scope) and tagged DPAPI
//   - "v10" AES-GCM blobs for a sample password and a sample card number
// and writes seal_out.txt (3 base64 lines: keyblob, password blob, card blob)
// next to the working directory. tools/wine_seal_inject.py then plants the
// values into the browser profile so the collector has real sealed data to
// decrypt end-to-end.
#include "core/crypto.h"
#include "core/dpapi.h"
#include "core/b64.h"

#include <cstdio>
#include <string>
#include <vector>

int main() {
    auto key = crypto::random_bytes(32);
    auto sealed = dpapi::protect(key);
    if (!sealed) {
        std::printf("dpapi protect failed\n");
        return 1;
    }
    std::vector<uint8_t> keyblob;
    const char* tag = "DPAPI";
    keyblob.insert(keyblob.end(), tag, tag + 5);
    keyblob.insert(keyblob.end(), sealed->begin(), sealed->end());

    auto seal = [&](const std::string& plain) -> std::string {
        auto nonce = crypto::random_bytes(12);
        auto enc = crypto::aes_gcm_encrypt(key, nonce, crypto::to_bytes(plain));
        if (!enc) return {};
        std::string r = "v10";
        r.append((const char*)nonce.data(), nonce.size());
        r.append((const char*)enc->first.data(), enc->first.size());
        r.append((const char*)enc->second.data(), 16);
        return r;
    };

    std::string pass_enc = seal("SuperSecret123!");
    std::string card_enc = seal("4539148803436467");
    if (pass_enc.empty() || card_enc.empty()) {
        std::printf("aes-gcm seal failed\n");
        return 1;
    }

    FILE* f = std::fopen("seal_out.txt", "w");
    if (!f) {
        std::printf("cannot write seal_out.txt\n");
        return 1;
    }
    std::fprintf(f, "%s\n", util::b64_encode(keyblob).c_str());
    std::fprintf(f, "%s\n",
                 util::b64_encode(std::vector<uint8_t>(pass_enc.begin(), pass_enc.end())).c_str());
    std::fprintf(f, "%s\n",
                 util::b64_encode(std::vector<uint8_t>(card_enc.begin(), card_enc.end())).c_str());
    std::fclose(f);
    std::printf("seal ok: key=%zu pass=%zu card=%zu\n", keyblob.size(), pass_enc.size(),
                card_enc.size());
    return 0;
}
