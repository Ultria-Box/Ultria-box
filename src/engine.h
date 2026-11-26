#pragma once
// Collector engine entry point. Kept separate from main.cpp so the engine can
// be compiled into libultria.a for host applications (constructor auto-start).
namespace ultria {
int run_core();
}
