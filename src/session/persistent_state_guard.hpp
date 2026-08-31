#pragma once

// PersistentStateGuard (F3): watches the game's persistent display settings
// while a session runs and snaps them back to the pre-launch snapshot.
//
// Mechanism: RegNotifyChangeKeyValue (event-driven, no polling - plan §9.2)
// per watched root; on notification the current values are compared with the
// snapshot and rewritten only when they actually differ. That compare step
// also breaks the restore-notifies-restore loop (plan §9.4): our own restore
// write wakes the watch once, the comparison then finds no difference, and
// nothing is written again.
//
// The guard NEVER touches the game's runtime state (plan §9.3): the running
// game already holds its display settings in memory; the registry values it
// would persist for its next launch are what we keep desktop-shaped.

#include "domain/error.hpp"
#include "domain/persistent_state.hpp"
#include "platform/win32/unique_handle.hpp"

#include <windows.h>

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace hoyoflux::session {

class PersistentStateGuard {
public:
    PersistentStateGuard() = default;
    ~PersistentStateGuard();
    PersistentStateGuard(const PersistentStateGuard&) = delete;
    PersistentStateGuard& operator=(const PersistentStateGuard&) = delete;

    // Begin watching every root captured in `snapshot`. Fails (leaving
    // nothing running) when a watch cannot be established. An empty
    // snapshot is a no-op.
    Result<void> start(const PersistentDisplayState& snapshot);

    // Stop watching and join the worker. Safe to call twice.
    void stop();

    // Number of restores performed while watching (diagnostics).
    [[nodiscard]] uint32_t restore_count() const { return restore_count_; }

private:
    // HKEY RAII: RegCloseKey, not CloseHandle.
    class OwnedKey {
    public:
        OwnedKey() = default;
        explicit OwnedKey(HKEY key) noexcept : key_(key) {}
        ~OwnedKey() {
            if (key_ != nullptr) {
                RegCloseKey(key_);
            }
        }
        OwnedKey(const OwnedKey&) = delete;
        OwnedKey& operator=(const OwnedKey&) = delete;
        OwnedKey(OwnedKey&& other) noexcept
            : key_(std::exchange(other.key_, nullptr)) {}
        OwnedKey& operator=(OwnedKey&& other) noexcept {
            if (this != &other) {
                if (key_ != nullptr) {
                    RegCloseKey(key_);
                }
                key_ = std::exchange(other.key_, nullptr);
            }
            return *this;
        }
        [[nodiscard]] HKEY get() const noexcept { return key_; }
        explicit operator bool() const noexcept { return key_ != nullptr; }

    private:
        HKEY key_{nullptr};
    };

    struct Watch {
        std::wstring root;
        OwnedKey key;              // open with KEY_NOTIFY
        win32::UniqueHandle event; // signalled by RegNotifyChangeKeyValue
    };

    Result<void> arm(Watch& watch);
    void watch_loop();

    std::vector<Watch> watches_;
    win32::UniqueHandle stop_event_{nullptr};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> restore_count_{0};
    PersistentDisplayState snapshot_;
};

}  // namespace hoyoflux::session
