#pragma once
// Logging that compiles away in release builds.
#include <cstdio>
#include <string>
#include <windows.h>

#ifdef ULTRA_DEBUG
namespace logx {
inline void line(const std::string& s) {
    OutputDebugStringA(("[ultria] " + s + "\n").c_str());
    std::printf("[ultria] %s\n", s.c_str());
}
} // namespace logx
#define ULOG(s) ::logx::line(std::string(s))
#else
#define ULOG(s) ((void)0)
#endif
