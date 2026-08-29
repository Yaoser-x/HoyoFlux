#pragma once

// Capability contract (F0).
//
// Every user-facing feature a Profile can request maps to exactly one
// Capability. Each game adapter reports, per capability, whether its build of
// the game can actually honor it. Nothing in the session flow may silently
// ignore a configured feature: `validate_profile` (game layer) rejects a
// launch whose profile requires an unsupported capability *before* the game
// process exists.
//
// The same report powers `hoyoflux doctor`'s compatibility output, so the
// launcher has exactly one source of truth for "what works on this install".

#include "domain/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux {

enum class Capability : uint8_t {
    FpsUnlock,             // runtime.fps via patch
    DynamicFps,            // fps changes while the game runs (hotkeys / IPC)
    CustomResolution,      // render.resolution launch arguments
    FullscreenMode,        // render.fullscreen launch arguments
    MonitorSelection,      // render.monitor
    MobileUi,              // ui.mobile_ui
    CustomDpi,             // ui.dpi_scale
    PowerSave,             // runtime.power_save foreground throttle
    PersistentStateGuard,  // session-scoped resolution without polluting the
                           // game's persistent settings
};

enum class CapabilityStatus : uint8_t {
    Supported,        // implemented for this install; honoring it is real
    Unsupported,      // not implemented in this build - launching with it is
                      // an error, never a silent no-op
    NotRequired,      // the profile does not use this feature
    SignatureMissing, // implemented, but the required signature did not
                      // resolve on this game version (doctor reports this)
};

struct CapabilityEntry {
    Capability capability{};
    CapabilityStatus status{CapabilityStatus::Unsupported};
    std::string reason;  // human-readable, shown by validate/doctor
};

struct CapabilityReport {
    std::vector<CapabilityEntry> entries;

    [[nodiscard]] const CapabilityEntry* find(Capability capability) const {
        for (const auto& entry : entries) {
            if (entry.capability == capability) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] CapabilityStatus status_of(Capability capability) const {
        const auto* entry = find(capability);
        return entry != nullptr ? entry->status : CapabilityStatus::Unsupported;
    }
};

[[nodiscard]] constexpr std::string_view to_string(Capability capability) {
    using enum Capability;
    switch (capability) {
    case FpsUnlock: return "fps-unlock";
    case DynamicFps: return "dynamic-fps";
    case CustomResolution: return "custom-resolution";
    case FullscreenMode: return "fullscreen-mode";
    case MonitorSelection: return "monitor-selection";
    case MobileUi: return "mobile-ui";
    case CustomDpi: return "custom-dpi";
    case PowerSave: return "power-save";
    case PersistentStateGuard: return "persistent-state-guard";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(CapabilityStatus status) {
    using enum CapabilityStatus;
    switch (status) {
    case Supported: return "supported";
    case Unsupported: return "unsupported";
    case NotRequired: return "not-required";
    case SignatureMissing: return "signature-missing";
    }
    return "unknown";
}

}  // namespace hoyoflux
