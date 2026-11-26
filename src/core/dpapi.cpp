#include "dpapi.h"
#include "b64.h"
#include <windows.h>
#include <dpapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif

namespace dpapi {

std::optional<Bytes> unprotect(const Bytes& data, const Bytes& entropy) {
    if (data.empty()) return std::nullopt;
    DATA_BLOB in{}, ent{}, out{};
    in.pbData = const_cast<BYTE*>(data.data());
    in.cbData = (DWORD)data.size();
    if (!entropy.empty()) {
        ent.pbData = const_cast<BYTE*>(entropy.data());
        ent.cbData = (DWORD)entropy.size();
    }
    if (!CryptUnprotectData(&in, nullptr, entropy.empty() ? nullptr : &ent, nullptr, nullptr, 0, &out))
        return std::nullopt;
    Bytes result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::optional<Bytes> protect(const Bytes& data, const Bytes& entropy) {
    if (data.empty()) return std::nullopt;
    DATA_BLOB in{}, ent{}, out{};
    in.pbData = const_cast<BYTE*>(data.data());
    in.cbData = (DWORD)data.size();
    if (!entropy.empty()) {
        ent.pbData = const_cast<BYTE*>(entropy.data());
        ent.cbData = (DWORD)entropy.size();
    }
    if (!CryptProtectData(&in, L"", entropy.empty() ? nullptr : &ent, nullptr, nullptr, 0, &out))
        return std::nullopt;
    Bytes result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::optional<std::string> unprotect_b64_to_string(const std::string& b64) {
    auto raw = util::b64_decode(b64);
    if (raw.empty()) return std::nullopt;
    auto dec = unprotect(raw);
    if (!dec) return std::nullopt;
    return std::string(dec->begin(), dec->end());
}

} // namespace dpapi
