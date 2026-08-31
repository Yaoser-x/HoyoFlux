#pragma once

// RuntimeController (F6/F7): the resident component that manages everything
// that happens WHILE the game runs - dynamic fps, power save, hotkeys. It
// owns no patch knowledge: the session engine hands it a writer callback
// bound to whatever fps channel the plan established (RemoteState slot or a
// direct variable write).
//
// F6 semantics (the regression the plan mandates):
//   power_save disabled -> NO foreground hook is registered and the fps
//   channel is never touched. Alt-tab / multi-monitor focus changes cannot
//   be influenced by HoyoFlux, because no listener exists.
//
//   power_save enabled  -> an event-driven foreground hook (EVENT_SYSTEM_
//   FOREGROUND, no polling) writes power_save_fps when the game loses the
//   foreground and profile_fps when it regains it. Exactly 4 bytes.

#include "domain/error.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <thread>

namespace hoyoflux::runtime {

class RuntimeController {
public:
    struct Config {
        uint32_t game_pid{0};
        uint32_t profile_fps{120};
        uint32_t power_save_fps{30};
        bool power_save_enabled{false};
        bool hotkeys_enabled{false};
        uint32_t hotkey_fps_step{10};
    };

    // Writes one fps value through the session's fps channel.
    using FpsWriter = std::function<Result<void>(uint32_t fps)>;

    RuntimeController() = default;
    ~RuntimeController();
    RuntimeController(const RuntimeController&) = delete;
    RuntimeController& operator=(const RuntimeController&) = delete;

    // Starts only the services the config asks for. A config with everything
    // disabled creates no threads at all (F6: disabled == nonexistent).
    Result<void> start(const Config& config, FpsWriter writer);
    void stop();

    [[nodiscard]] bool watching_foreground() const {
        return foreground_active_;
    }
    [[nodiscard]] bool hotkeys_active() const { return hotkeys_active_; }

    // The focus reaction, exposed for tests: true when `foreground_pid` is
    // the game.
    void on_foreground_changed(uint32_t foreground_pid);

    // The pure F6 policy: which fps belongs to `foreground_pid`. Tested
    // directly; the OS hook wiring around it is a real-machine concern.
    [[nodiscard]] static uint32_t target_fps_for(const Config& config,
                                                 uint32_t foreground_pid) {
        return foreground_pid == config.game_pid ? config.profile_fps
                                                 : config.power_save_fps;
    }

private:
    void foreground_thread(std::promise<Result<void>> initialized);
    void hotkey_thread(std::promise<Result<void>> initialized);

    Config config_;
    FpsWriter writer_;

    // thread ids captured at thread start (for WM_QUIT wake-up)
    std::atomic<uint32_t> foreground_thread_id_{0};
    std::atomic<uint32_t> hotkey_thread_id_{0};

    // foreground service
    HWINEVENTHOOK foreground_hook_{nullptr};  // WinEvent hook handle
    std::thread foreground_worker_;
    std::atomic<bool> foreground_active_{false};

    // hotkey service
    std::thread hotkey_worker_;
    std::atomic<bool> hotkeys_active_{false};

    // hotkey state
    std::atomic<bool> fps_control_on_{true};
    std::atomic<uint32_t> current_fps_{0};

    std::atomic<bool> stopping_{false};
};

}  // namespace hoyoflux::runtime
