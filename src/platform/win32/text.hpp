#pragma once

// Small Win32 text boundary used by diagnostics and serialized state. Keep
// all UTF-16 -> UTF-8 narrowing on the Windows API path; byte-wise wchar_t
// narrowing is not valid for non-ASCII registry paths or value names.

#include <windows.h>

#include <limits>
#include <string>
#include <string_view>

namespace hoyoflux::win32 {

inline std::string utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return "<invalid UTF-16>";
    }

    const int length = static_cast<int>(wide.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         wide.data(), length, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        return "<invalid UTF-16>";
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                            result.data(), size, nullptr, nullptr) <= 0) {
        return "<invalid UTF-16>";
    }
    return result;
}

}  // namespace hoyoflux::win32
