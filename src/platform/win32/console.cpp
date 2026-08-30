#include "platform/win32/console.hpp"

#include <windows.h>

#include <cstdio>
#include <iostream>

namespace hoyoflux::win32 {

bool attach_parent_console() {
    if (GetConsoleWindow() == nullptr &&
        !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return false;
    }
    FILE* stream = nullptr;
    (void)freopen_s(&stream, "CONIN$", "r", stdin);
    (void)freopen_s(&stream, "CONOUT$", "w", stdout);
    (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    return true;
}

}  // namespace hoyoflux::win32
