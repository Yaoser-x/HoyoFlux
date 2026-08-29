#include "session/persistent_state_guard.hpp"

#include "game/game_adapter.hpp"
#include "platform/win32/registry.hpp"

#include <algorithm>

namespace hoyoflux::session {
namespace {

using hoyoflux::PersistentDisplayState;
using hoyoflux::PersistentSetting;
using hoyoflux::PersistentSettingSet;

// The Screenmanager names a root currently stores, compared type+data
// against the snapshot. Absent-from-current names also count as changes.
bool root_matches_snapshot(const std::vector<PersistentSetting>& snapshot,
                           const std::vector<hoyoflux::win32::RegistryValue>& current) {
    if (snapshot.size() != current.size()) {
        return false;
    }
    for (const auto& setting : snapshot) {
        const auto found = std::find_if(current.begin(), current.end(),
                                        [&](const auto& value) {
                                            return value.name == setting.name;
                                        });
        if (found == current.end() || found->type != setting.type ||
            found->data != setting.data) {
            return false;
        }
    }
    return true;
}

}  // namespace

PersistentStateGuard::~PersistentStateGuard() { stop(); }

Result<void> PersistentStateGuard::arm(Watch& watch) {
    const LSTATUS status = RegNotifyChangeKeyValue(
        watch.key.get(), FALSE, REG_NOTIFY_CHANGE_LAST_SET, watch.event.get(),
        /*asynchronous=*/TRUE);
    if (status != ERROR_SUCCESS) {
        return std::unexpected(Error::make(
            ErrorCode::RegistryReadFailed,
            "RegNotifyChangeKeyValue failed for " + std::string(watch.root.begin(), watch.root.end()),
            status));
    }
    return {};
}

Result<void> PersistentStateGuard::start(const PersistentDisplayState& snapshot) {
    if (snapshot.sets.empty()) {
        return {};  // nothing captured -> nothing to protect
    }
    if (running_) {
        return std::unexpected(
            Error::make(ErrorCode::InvalidArgument, "guard already running"));
    }
    snapshot_ = snapshot;

    if (!stop_event_) {
        stop_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!stop_event_) {
            return std::unexpected(Error::make(ErrorCode::OsError,
                                               "CreateEventW(stop) failed",
                                               GetLastError()));
        }
    }
    for (const auto& set : snapshot_.sets) {
        Watch watch;
        watch.root = set.root;
        HKEY raw = nullptr;
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, set.root.c_str(), 0,
                                       KEY_NOTIFY, &raw);
        if (status != ERROR_SUCCESS) {
            stop();
            return std::unexpected(Error::make(
                ErrorCode::RegistryReadFailed,
                "RegOpenKeyExW(KEY_NOTIFY) failed for " +
                    std::string(set.root.begin(), set.root.end()),
                status));
        }
        watch.key = OwnedKey(raw);
        watch.event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!watch.event) {
            stop();
            return std::unexpected(Error::make(ErrorCode::OsError,
                                               "CreateEventW(watch) failed",
                                               GetLastError()));
        }
        if (auto armed = arm(watch); !armed) {
            stop();
            return std::unexpected(armed.error());
        }
        watches_.push_back(std::move(watch));
    }

    running_ = true;
    worker_ = std::thread([this] { watch_loop(); });
    return {};
}

void PersistentStateGuard::stop() {
    if (worker_.joinable()) {
        running_ = false;
        SetEvent(stop_event_.get());
        worker_.join();
    }
    watches_.clear();
}

void PersistentStateGuard::watch_loop() {
    std::vector<HANDLE> handles;
    handles.push_back(stop_event_.get());
    for (const auto& watch : watches_) {
        handles.push_back(watch.event.get());
    }

    while (running_) {
        const DWORD woke = WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
        if (woke == WAIT_FAILED || woke == WAIT_OBJECT_0) {
            return;  // stop event or wait failure
        }
        if (woke < WAIT_OBJECT_0 || woke > WAIT_OBJECT_0 + handles.size() - 1) {
            return;
        }
        const size_t index = static_cast<size_t>(woke - WAIT_OBJECT_0) - 1;
        if (index >= watches_.size()) {
            return;
        }
        Watch& watch = watches_[index];

        // Re-arm first so changes made while we read are not lost; a
        // redundant wake is harmless (comparison decides).
        (void)arm(watch);

        auto current = win32::read_registry_values(watch.root);
        if (!current) {
            continue;  // transient read failure: keep watching
        }
        const auto set = std::find_if(snapshot_.sets.begin(),
                                      snapshot_.sets.end(),
                                      [&](const auto& s) {
                                          return s.root == watch.root;
                                      });
        if (set == snapshot_.sets.end()) {
            continue;
        }
        if (root_matches_snapshot(set->settings, *current)) {
            continue;  // our own restore's notification: nothing to do
        }
        // Snap the values back. This does not disturb the running game -
        // it only reshapes what the game would see on its NEXT launch.
        std::vector<win32::RegistryValue> values;
        values.reserve(set->settings.size());
        for (const auto& setting : set->settings) {
            values.push_back(win32::RegistryValue{setting.name, setting.type,
                                                  setting.data});
        }
        if (win32::write_registry_values(watch.root, values).has_value()) {
            ++restore_count_;
        }
    }
}

}  // namespace hoyoflux::session
