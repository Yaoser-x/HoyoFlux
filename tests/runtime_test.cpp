// RuntimeController tests (F6/F7). Hermetic by construction: the disabled
// path creates no threads and no hooks, and the enabled path's policy is a
// pure function - the OS hook wiring itself is a real-machine (B1) concern.

#include "runtime/runtime_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace hoyoflux;
using hoyoflux::runtime::RuntimeController;

TEST_CASE("the disabled path touches no fps bytes and creates no services",
          "[runtime][powersave]") {
    RuntimeController controller;
    std::vector<uint32_t> written;

    RuntimeController::Config config;
    config.game_pid = 1234;
    config.profile_fps = 120;
    config.power_save_fps = 30;
    config.power_save_enabled = false;  // the plan's regression gate
    config.hotkeys_enabled = false;
    REQUIRE(controller.start(config, [&](uint32_t fps) {
        written.push_back(fps);
        return Result<void>{};
    }).has_value());

    // No listener exists at all - Alt-Tab / other-monitor focus changes
    // cannot be influenced because nothing is watching.
    CHECK_FALSE(controller.watching_foreground());
    CHECK_FALSE(controller.hotkeys_active());
    controller.on_foreground_changed(999);
    controller.on_foreground_changed(1234);
    CHECK(written.empty());

    controller.stop();
}

TEST_CASE("the F6 policy: game foreground gets profile fps, everything "
          "else gets power-save fps", "[runtime][powersave]") {
    RuntimeController::Config config;
    config.game_pid = 1234;
    config.profile_fps = 120;
    config.power_save_fps = 30;
    config.power_save_enabled = true;

    CHECK(RuntimeController::target_fps_for(config, 1234) == 120);
    CHECK(RuntimeController::target_fps_for(config, 999) == 30);
    CHECK(RuntimeController::target_fps_for(config, 0) == 30);
}

TEST_CASE("a controller with everything disabled starts no services",
          "[runtime][config]") {
    RuntimeController controller;
    RuntimeController::Config config;
    config.power_save_enabled = false;
    config.hotkeys_enabled = false;
    REQUIRE(controller.start(config, [](uint32_t) {
        return Result<void>{};
    }).has_value());
    CHECK_FALSE(controller.watching_foreground());
    CHECK_FALSE(controller.hotkeys_active());
    controller.stop();
}

TEST_CASE("hotkey registration failure is reported by start",
          "[runtime][hotkeys]") {
    REQUIRE(RegisterHotKey(nullptr, 91, MOD_NOREPEAT, VK_END));

    RuntimeController controller;
    RuntimeController::Config config;
    config.hotkeys_enabled = true;
    auto started = controller.start(config, [](uint32_t) {
        return Result<void>{};
    });
    CHECK_FALSE(started.has_value());
    CHECK_FALSE(controller.hotkeys_active());

    UnregisterHotKey(nullptr, 91);
}
