#include "platform/win32/console.hpp"

#include <windows.h>

#include <cstdio>
#include <iostream>

namespace hoyoflux::win32 {

namespace {

void bind_console_streams() {
    FILE* stream = nullptr;
    const errno_t stdin_result = freopen_s(&stream, "CONIN$", "r", stdin);
    const errno_t stdout_result = freopen_s(&stream, "CONOUT$", "w", stdout);
    const errno_t stderr_result = freopen_s(&stream, "CONOUT$", "w", stderr);
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    (void)stdin_result;
    (void)stdout_result;
    (void)stderr_result;
}

}  // namespace

bool attach_console(DWORD process_id) {
    if (GetConsoleWindow() == nullptr && !AttachConsole(process_id)) {
        return false;
    }
    bind_console_streams();
    return true;
}

bool attach_parent_console() {
    return attach_console(ATTACH_PARENT_PROCESS);
}

}  // namespace hoyoflux::win32
