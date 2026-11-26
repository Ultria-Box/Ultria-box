#pragma once
// Compile-time string obfuscation.
// Plaintext literals never appear in the binary; each string is XOR-encrypted
// with a key derived from a unique counter, decrypted on the stack at use time.

#include <cstdint>
#include <string>

namespace obf {

template <uint32_t K, size_t N>
struct XS {
    char b[N];

    constexpr XS(const char (&s)[N]) : b{} {
        for (size_t i = 0; i < N; ++i)
            b[i] = s[i] ^ static_cast<char>((K >> ((i % 4) * 8)) & 0xFF);
    }

    std::string dec() const {
        std::string out(N ? N - 1 : 0, '\0');
        for (size_t i = 0; i + 1 < N; ++i)
            out[i] = b[i] ^ static_cast<char>((K >> ((i % 4) * 8)) & 0xFF);
        return out;
    }

    const char* c_str_into(char* buf) const {
        for (size_t i = 0; i + 1 < N; ++i)
            buf[i] = b[i] ^ static_cast<char>((K >> ((i % 4) * 8)) & 0xFF);
        buf[N - 1] = '\0';
        return buf;
    }
};

template <uint32_t K, size_t N>
constexpr auto make(const char (&s)[N]) { return XS<K, N>(s); }

} // namespace obf

// S: literal > OBF("kernel32.dll")
// ULTRA_BUILD_SEED is rotated by tools/builder.py on every build so all
// encrypted strings differ between builds.
#ifndef ULTRA_BUILD_SEED
#define ULTRA_BUILD_SEED 0x1D5B9A3Fu
#endif
#define OBF_CTR2(n) ((n) * 2654435761u + 0x9E3779B9u + (uint32_t)ULTRA_BUILD_SEED)
#define OBF_CTR(n) OBF_CTR2(n)
#define OBF(s) (obf::make<OBF_CTR(__COUNTER__)>(s).dec())

// Wide string variant: returns std::wstring, same xor scheme.
namespace obf {
template <uint32_t K, size_t N>
struct XW {
    wchar_t b[N];

    constexpr XW(const wchar_t (&s)[N]) : b{} {
        for (size_t i = 0; i < N; ++i)
            b[i] = static_cast<wchar_t>(s[i] ^ static_cast<wchar_t>((K >> ((i % 2) * 16)) & 0xFFFF));
    }

    std::wstring dec() const {
        std::wstring out(N ? N - 1 : 0, L'\0');
        for (size_t i = 0; i + 1 < N; ++i)
            out[i] = static_cast<wchar_t>(b[i] ^ static_cast<wchar_t>((K >> ((i % 2) * 16)) & 0xFFFF));
        return out;
    }
};
} // namespace obf

#define OBFW(s) (obf::XW<OBF_CTR(__COUNTER__), (sizeof(s) / sizeof(wchar_t))>(s).dec())
