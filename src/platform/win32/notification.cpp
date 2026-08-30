#include "platform/win32/notification.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>

namespace hoyoflux::win32 {
namespace {

struct NotificationState {
    HWND window{nullptr};
    bool added{false};
};

NotificationState& notification_state() {
    static NotificationState state;
    return state;
}

HWND notification_window() {
    auto& state = notification_state();
    if (state.window == nullptr || !IsWindow(state.window)) {
        state.window = CreateWindowExW(
            0, L"STATIC", L"HoyoFlux notification owner", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    return state.window;
}

void copy_text(wchar_t* destination, size_t capacity, std::wstring_view text) {
    const size_t count = std::min(capacity - 1, text.size());
    std::wmemcpy(destination, text.data(), count);
    destination[count] = L'\0';
}

}  // namespace

Result<void> notify(std::wstring_view title, std::wstring_view body,
                    NotificationKind kind) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = notification_window();
    if (data.hWnd == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "notification window creation failed",
            GetLastError()));
    }
    data.uID = 1;
    data.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
    data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    copy_text(data.szTip, std::size(data.szTip), L"HoyoFlux");
    data.dwInfoFlags = kind == NotificationKind::Error
                           ? NIIF_ERROR
                           : (kind == NotificationKind::Warning ? NIIF_WARNING
                                                               : NIIF_INFO);
    copy_text(data.szInfoTitle, std::size(data.szInfoTitle), title);
    copy_text(data.szInfo, std::size(data.szInfo), body);
    auto& state = notification_state();
    const DWORD operation = state.added ? NIM_MODIFY : NIM_ADD;
    if (!Shell_NotifyIconW(operation, &data)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "Shell_NotifyIconW failed", GetLastError()));
    }
    state.added = true;
    return {};
}

void cleanup_notifications() {
    auto& state = notification_state();
    if (state.added && state.window != nullptr && IsWindow(state.window)) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = state.window;
        data.uID = 1;
        (void)Shell_NotifyIconW(NIM_DELETE, &data);
    }
    state.added = false;
    if (state.window != nullptr && IsWindow(state.window)) {
        DestroyWindow(state.window);
    }
    state.window = nullptr;
}

void notify_best_effort(const NotificationFunction& function,
                        std::wstring_view title, std::wstring_view body,
                        NotificationKind kind) {
    if (function) {
        (void)function(title, body, kind);
    }
}

}  // namespace hoyoflux::win32
