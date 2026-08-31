#pragma once

// Game persistent display state (F2).
//
// The state a game persists for its NEXT launch (registry values under its
// own HKCU keys) is a different object from the physical Windows display
// mode HoyoFlux snapshots today. Conflating the two is what let the legacy
// tool's iPad resolution leak into official launcher sessions.
//
// The model is deliberately snapshot-shaped: HoyoFlux captures whatever
// display-related values the game stores and restores them verbatim. It does
// NOT hardcode a field list - the real-machine A/B diff experiment
// (docs/persistent-state-experiment.md) is what validates that the watched
// storage roots see every value the game actually changes.

#include "domain/profile.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hoyoflux {

// One persisted value exactly as stored (e.g. a REG_DWORD under the game's
// Screenmanager set). Type/data are the raw registry representation.
struct PersistentSetting {
    std::wstring name;
    uint32_t type{0};
    std::vector<std::byte> data;
};

// All captured settings of one storage root (an HKCU subkey path).
struct PersistentSettingSet {
    std::wstring root;  // e.g. L"Software\\miHoYo\\Genshin Impact"
    std::vector<PersistentSetting> settings;
};

struct PersistentDisplayState {
    std::vector<PersistentSettingSet> sets;

    // Best-effort decoded view for diagnostics/doctor; never used for
    // restoring (restore always replays `sets` verbatim).
    std::optional<Resolution> resolution;
    std::optional<uint32_t> fullscreen_mode;  // Unity Screenmanager enum
};

}  // namespace hoyoflux
