#include "platform/win32/notification.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace hoyoflux::win32 {
namespace {

constexpr UINT kNotifyMessage = WM_APP + 1;
constexpr UINT kCleanupMessage = WM_APP + 2;
constexpr UINT_PTR kCleanupTimerId = 1;
constexpr UINT kTransientDurationMs = 6000;
constexpr wchar_t kWindowClassName[] = L"HoyoFluxNotificationOwner";

void copy_text(wchar_t* destination, size_t capacity, std::wstring_view text) {
    const size_t count = std::min(capacity - 1, text.size());
    std::wmemcpy(destination, text.data(), count);
    destination[count] = L'\0';
}

struct NotifyRequest {
    std::wstring title;
    std::wstring body;
    NotificationKind kind{NotificationKind::Info};
    std::optional<Result<void>> result;
};

class NotificationService {
public:
    ~NotificationService() { cleanup(); }

    Result<void> notify(std::wstring_view title, std::wstring_view body,
                        NotificationKind kind) {
        std::lock_guard operation_lock(operation_mutex_);
        const HWND window = ensure_window();
        if (window == nullptr) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "notification worker startup failed",
                ERROR_GEN_FAILURE));
        }

        NotifyRequest request{std::wstring(title), std::wstring(body), kind,
                              std::nullopt};
        (void)SendMessageW(window, kNotifyMessage, 0,
                           reinterpret_cast<LPARAM>(&request));
        if (!request.result) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "notification worker did not respond",
                ERROR_GEN_FAILURE));
        }
        return std::move(*request.result);
    }

    void cleanup() {
        std::lock_guard operation_lock(operation_mutex_);

        HWND window = nullptr;
        {
            std::unique_lock lifecycle_lock(lifecycle_mutex_);
            if (!worker_.joinable()) {
                return;
            }
            lifecycle_cv_.wait(lifecycle_lock,
                               [this] { return ready_; });
            window = window_;
        }

        if (window != nullptr) {
            (void)SendMessageW(window, kCleanupMessage, 0, 0);
        }

        std::thread worker;
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            worker = std::move(worker_);
        }
        if (worker.joinable()) {
            worker.join();
        }
    }

private:
    HWND ensure_window() {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        if (!worker_.joinable()) {
            ready_ = false;
            running_ = false;
            window_ = nullptr;
            worker_ = std::thread([this] { worker_main(); });
        }
        lifecycle_cv_.wait(lifecycle_lock, [this] { return ready_; });
        if (running_) {
            return window_;
        }

        std::thread failed_worker = std::move(worker_);
        lifecycle_lock.unlock();
        if (failed_worker.joinable()) {
            failed_worker.join();
        }
        return nullptr;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(
                window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        auto* service = reinterpret_cast<NotificationService*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (service == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message) {
        case kNotifyMessage:
            service->handle_notify(
                window, *reinterpret_cast<NotifyRequest*>(lparam));
            return 0;
        case kCleanupMessage:
            service->remove_icon(window);
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (wparam == kCleanupTimerId) {
                service->remove_icon(window);
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void worker_main() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &NotificationService::window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClassName;
        const ATOM registered = RegisterClassExW(&window_class);
        const bool class_registered = registered != 0;
        if (!class_registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            signal_worker_state(nullptr, false);
            return;
        }

        const HWND window = CreateWindowExW(
            0, kWindowClassName, L"HoyoFlux notification owner", 0, 0, 0,
            0, 0, HWND_MESSAGE, nullptr, instance, this);
        signal_worker_state(window, window != nullptr);
        if (window == nullptr) {
            if (class_registered) {
                UnregisterClassW(kWindowClassName, instance);
            }
            return;
        }

        MSG message{};
        while (true) {
            const BOOL received = GetMessageW(&message, nullptr, 0, 0);
            if (received <= 0) {
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        remove_icon(window);
        if (IsWindow(window)) {
            DestroyWindow(window);
        }
        if (class_registered) {
            UnregisterClassW(kWindowClassName, instance);
        }
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            window_ = nullptr;
            running_ = false;
            ready_ = true;
        }
        lifecycle_cv_.notify_all();
    }

    void signal_worker_state(HWND window, bool running) {
        {
            std::lock_guard lifecycle_lock(lifecycle_mutex_);
            window_ = window;
            running_ = running;
            ready_ = true;
        }
        lifecycle_cv_.notify_all();
    }

    void handle_notify(HWND window, NotifyRequest& request) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        data.uFlags = NIF_INFO | NIF_ICON | NIF_TIP;
        data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        copy_text(data.szTip, std::size(data.szTip), L"HoyoFlux");
        data.dwInfoFlags = request.kind == NotificationKind::Error
                               ? NIIF_ERROR
                               : (request.kind == NotificationKind::Warning
                                      ? NIIF_WARNING
                                      : NIIF_INFO);
        copy_text(data.szInfoTitle, std::size(data.szInfoTitle),
                  request.title);
        copy_text(data.szInfo, std::size(data.szInfo), request.body);

        const DWORD operation = icon_added_ ? NIM_MODIFY : NIM_ADD;
        if (!Shell_NotifyIconW(operation, &data)) {
            request.result = std::unexpected(Error::make(
                ErrorCode::OsError, "Shell_NotifyIconW failed", GetLastError()));
            return;
        }
        icon_added_ = true;

        KillTimer(window, kCleanupTimerId);
        if (SetTimer(window, kCleanupTimerId, kTransientDurationMs, nullptr) ==
            0) {
            const DWORD error = GetLastError();
            remove_icon(window);
            request.result = std::unexpected(Error::make(
                ErrorCode::OsError, "notification timer setup failed", error));
            return;
        }
        request.result = Result<void>{};
    }

    void remove_icon(HWND window) {
        KillTimer(window, kCleanupTimerId);
        if (!icon_added_) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        (void)Shell_NotifyIconW(NIM_DELETE, &data);
        icon_added_ = false;
    }

    std::mutex operation_mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    std::thread worker_;
    HWND window_{nullptr};
    bool ready_{false};
    bool running_{false};
    bool icon_added_{false};  // accessed only by the notification worker
};

NotificationService& notification_service() {
    static NotificationService service;
    return service;
}

}  // namespace

Result<void> notify(std::wstring_view title, std::wstring_view body,
                    NotificationKind kind) {
    return notification_service().notify(title, body, kind);
}

void cleanup_notifications() { notification_service().cleanup(); }

void notify_best_effort(const NotificationFunction& function,
                        std::wstring_view title, std::wstring_view body,
                        NotificationKind kind) {
    if (function) {
        (void)function(title, body, kind);
    }
}

}  // namespace hoyoflux::win32
