#include "platform/win32/notification.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace hoyoflux::win32 {
namespace {

constexpr UINT kNotifyMessage = WM_APP + 1;
constexpr UINT kDrainMessage = WM_APP + 2;
constexpr UINT kShutdownMessage = WM_APP + 3;
constexpr UINT_PTR kCleanupTimerId = 1;
constexpr UINT kTransientDurationMs = 6000;
constexpr auto kDrainTimeout = std::chrono::seconds(7);
constexpr auto kShutdownWait = std::chrono::milliseconds(250);
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

class NotificationWorker {
public:
    void run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &NotificationWorker::window_proc;
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

    bool wait_ready() {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        lifecycle_cv_.wait(lifecycle_lock, [this] { return ready_; });
        return running_;
    }

    bool running() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        return running_;
    }

    bool post_notify(NotifyRequest& request) {
        const HWND window = window_handle();
        if (window == nullptr) {
            return false;
        }
        (void)SendMessageW(window, kNotifyMessage, 0,
                           reinterpret_cast<LPARAM>(&request));
        return request.result.has_value();
    }

    bool post_drain() {
        const HWND window = window_handle();
        return window != nullptr && PostMessageW(window, kDrainMessage, 0, 0);
    }

    bool post_shutdown() {
        const HWND window = window_handle();
        return window != nullptr &&
               PostMessageW(window, kShutdownMessage, 0, 0);
    }

    bool wait_stopped(std::chrono::milliseconds timeout) {
        std::unique_lock lifecycle_lock(lifecycle_mutex_);
        return lifecycle_cv_.wait_for(
            lifecycle_lock, timeout, [this] { return ready_ && !running_; });
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(
                window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        auto* worker = reinterpret_cast<NotificationWorker*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (worker == nullptr) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message) {
        case kNotifyMessage:
            worker->handle_notify(
                window, *reinterpret_cast<NotifyRequest*>(lparam));
            return 0;
        case kDrainMessage:
            worker->draining_ = true;
            if (!worker->icon_added_) {
                worker->request_stop(window);
            }
            return 0;
        case kShutdownMessage:
            worker->remove_icon(window);
            worker->request_stop(window);
            return 0;
        case WM_TIMER:
            if (wparam == kCleanupTimerId) {
                worker->remove_icon(window);
                if (worker->draining_) {
                    worker->request_stop(window);
                }
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

    HWND window_handle() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        return running_ ? window_ : nullptr;
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

    void request_stop(HWND window) {
        if (stop_requested_) {
            return;
        }
        stop_requested_ = true;
        DestroyWindow(window);
        PostQuitMessage(0);
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

    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    HWND window_{nullptr};
    bool ready_{false};
    bool running_{false};
    bool icon_added_{false};       // accessed only by the notification worker
    bool draining_{false};         // accessed only by the notification worker
    bool stop_requested_{false};   // accessed only by the notification worker
};

class NotificationService {
public:
    ~NotificationService() { shutdown_immediate(); }

    Result<void> notify(std::wstring_view title, std::wstring_view body,
                        NotificationKind kind) {
        std::lock_guard operation_lock(operation_mutex_);
        const auto worker = ensure_worker();
        if (!worker) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "notification worker startup failed",
                ERROR_GEN_FAILURE));
        }

        NotifyRequest request{std::wstring(title), std::wstring(body), kind,
                              std::nullopt};
        if (!worker->post_notify(request) || !request.result) {
            return std::unexpected(Error::make(
                ErrorCode::OsError, "notification worker did not respond",
                ERROR_GEN_FAILURE));
        }
        return std::move(*request.result);
    }

    void drain() {
        std::lock_guard operation_lock(operation_mutex_);
        const auto worker = worker_state_;
        if (!worker) {
            return;
        }

        if (!worker->running()) {
            finish_worker(false);
            return;
        }
        if (!worker->post_drain()) {
            abandon_worker();
            return;
        }
        if (!worker->wait_stopped(kDrainTimeout)) {
            (void)worker->post_shutdown();
            abandon_worker();
            return;
        }
        finish_worker(false);
    }

    void shutdown_immediate() {
        std::lock_guard operation_lock(operation_mutex_);
        const auto worker = worker_state_;
        if (!worker) {
            return;
        }
        if (worker->running()) {
            (void)worker->post_shutdown();
        }
        if (!worker->wait_stopped(kShutdownWait)) {
            abandon_worker();
            return;
        }
        finish_worker(false);
    }

private:
    std::shared_ptr<NotificationWorker> ensure_worker() {
        if (worker_state_) {
            if (worker_state_->running()) {
                return worker_state_;
            }
            finish_worker(false);
        }

        auto worker = std::make_shared<NotificationWorker>();
        worker_state_ = worker;
        worker_thread_ = std::thread([worker] { worker->run(); });
        if (worker->wait_ready()) {
            return worker;
        }
        finish_worker(false);
        return nullptr;
    }

    void finish_worker(bool detach) {
        std::thread thread = std::move(worker_thread_);
        worker_state_.reset();
        if (!thread.joinable()) {
            return;
        }
        if (detach) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    void abandon_worker() { finish_worker(true); }

    std::mutex operation_mutex_;
    std::shared_ptr<NotificationWorker> worker_state_;
    std::thread worker_thread_;
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

void drain_notifications() { notification_service().drain(); }

void cleanup_notifications() { notification_service().shutdown_immediate(); }

void notify_best_effort(const NotificationFunction& function,
                        std::wstring_view title, std::wstring_view body,
                        NotificationKind kind) {
    if (function) {
        (void)function(title, body, kind);
    }
}

}  // namespace hoyoflux::win32
