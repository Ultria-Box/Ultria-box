#pragma once
// Anti-VM / anti-debug / anti-sandbox checks. Build flags in config.h decide
// whether a positive detection aborts execution (default: yes).
#include <string>

namespace evade {

struct Environment {
    bool vm = false;
    bool debugger = false;
    bool sandbox = false;
    std::string reason; // first positive signal, for debugging builds
};

Environment inspect();

// individual probes
bool vm_mac_oui();
bool vm_registry();
bool vm_processes();
bool vm_files();
bool vm_cpuid();
bool debugger_present();
bool sandbox_uptime_low();
bool sandbox_low_resources();

} // namespace evade
