#include "runtime/runtime_controller.hpp"

#include <winuser.h>

#include <cstdio>

namespace hoyoflux::runtime {
namespace {

// The WinEvent callback fires on the thread that installed the hook; that
// thread publishes its controller here before pumping messages.
thread_local RuntimeController* t_hook_controller = nullptr;

}  // namespace

RuntimeController::~RuntimeController() { stop(); }

Result<void> RuntimeController::start(const Config& config, FpsWriter writer) {
    if (!writer) {
        return std::unexpected(
            Error::make(ErrorCode::InvalidArgument, "no fps writer"));
    }
    config_ = config;
    writer_ = std::move(writer);
    current_fps_ = config_.profile_fps;
    fps_control_on_ = true;
    stopping_ = false;

    // F6: a disabled power save must not even create the listener.
    if (config_.power_save_enabled) {
        foreground_active_ = true;
        foreground_worker_ = std::thread([this] { foreground_thread(); });
    }
    if (config_.hotkeys_enabled) {
        hotkeys_active_ = true;
        hotkey_worker_ = std::thread([this] { hotkey_thread(); });
    }
    return {};
}

void RuntimeController::stop() {
    stopping_ = true;
    if (foreground_worker_.joinable()) {
        const uint32_t id = foreground_thread_id_;
        if (id != 0) {
            PostThreadMessage(id, WM_QUIT, 0, 0);
        }
        foreground_worker_.join();
    }
    foreground_active_ = false;
    if (hotkey_worker_.joinable()) {
        const uint32_t id = hotkey_thread_id_;
        if (id != 0) {
            PostThreadMessage(id, WM_QUIT, 0, 0);
        }
        hotkey_worker_.join();
    }
    hotkeys_active_ = false;
}

void RuntimeController::on_foreground_changed(uint32_t foreground_pid) {
    if (!foreground_active_ || !writer_) {
        return;
    }
    // The whole power-save policy: 4 bytes through the fps channel.
    const uint32_t target = target_fps_for(config_, foreground_pid);
    current_fps_ = target;
    (void)writer_(target);
}

void RuntimeController::foreground_thread() {
    foreground_thread_id_ = GetCurrentThreadId();
    t_hook_controller = this;

    // EVENT_SYSTEM_FOREGROUND is delivered synchronously to this thread's
    // message pump - no polling anywhere.
    foreground_hook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        [](HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child,
           DWORD, DWORD) {
            if (id_object != OBJID_WINDOW || id_child != 0 || hwnd == nullptr) {
                return;
            }
            if (t_hook_controller == nullptr) {
                return;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            t_hook_controller->on_foreground_changed(pid);
        },
        GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    if (foreground_hook_ == nullptr) {
        foreground_active_ = false;
        t_hook_controller = nullptr;
        return;
    }

    // The pump that delivers the hook callbacks; WM_QUIT ends it.
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    }
    UnhookWinEvent(foreground_hook_);
    foreground_hook_ = nullptr;
    t_hook_controller = nullptr;
}

void RuntimeController::hotkey_thread() {
    hotkey_thread_id_ = GetCurrentThreadId();

    // RegisterHotKey (plan section 15) - no GetAsyncKeyState polling.
    // END: toggle fps control. Ctrl+Up / Ctrl+Down: step the fps.
    if (!RegisterHotKey(nullptr, 1, MOD_NOREPEAT, VK_END) ||
        !RegisterHotKey(nullptr, 2, MOD_CONTROL | MOD_NOREPEAT, VK_UP) ||
        !RegisterHotKey(nullptr, 3, MOD_CONTROL | MOD_NOREPEAT, VK_DOWN)) {
        hotkeys_active_ = false;
        return;
    }

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message != WM_HOTKEY) {
            continue;
        }
        if (!fps_control_on_ && message.wParam != 1) {
            continue;  // control toggled off: adjustments do nothing
        }
        switch (message.wParam) {
        case 1:  // END: toggle
            fps_control_on_ = !fps_control_on_;
            if (fps_control_on_ && writer_) {
                current_fps_ = config_.profile_fps;
                (void)writer_(config_.profile_fps);
            }
            break;
        case 2: {  // Ctrl+Up
            const uint32_t next = current_fps_ + config_.hotkey_fps_step;
            if (next <= 1000) {
                current_fps_ = next;
                (void)writer_(next);
            }
            break;
        }
        case 3: {  // Ctrl+Down: clamped at 10, never below
            const uint32_t step = config_.hotkey_fps_step;
            const uint32_t current = current_fps_;
            const uint32_t next = current > step + 9 ? current - step : 10;
            current_fps_ = next;
            (void)writer_(next);
            break;
        }
        default: break;
        }
    }

    UnregisterHotKey(nullptr, 1);
    UnregisterHotKey(nullptr, 2);
    UnregisterHotKey(nullptr, 3);
}

}  // namespace hoyoflux::runtime
