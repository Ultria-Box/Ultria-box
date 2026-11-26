// Standalone entry for the Ultria Checker app (dist/ultria-checker.exe).
#include "gui.h"
#include <windows.h>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    return gui::run(instance, /*decoy=*/false);
}
