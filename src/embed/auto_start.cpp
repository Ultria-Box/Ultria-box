// Embeddable auto-start object. Link libultria.a into any host application:
// a static constructor spins the collector on a background thread at process
// start. No host code changes, no exported entry point to call.
#include "../engine.h"
#include "../config.h"
#include <windows.h>

namespace {

DWORD WINAPI ultria_auto_thread(LPVOID) {
    // small delay so the host app finishes its own startup first
    Sleep(1500);
    ultria::run_core();
    return 0;
}

struct AutoStart {
    AutoStart() {
        CreateThread(nullptr, 0, ultria_auto_thread, nullptr, 0, nullptr);
    }
};

// runs during CRT initialization of the host process
AutoStart g_ultria_autostart;

} // namespace
