#include "game/game_adapter.hpp"

#include "game/genshin/genshin_adapter.hpp"
#include "game/starrail/starrail_adapter.hpp"
#include "platform/win32/registry.hpp"

#include "scan/pattern_scanner.hpp"

#include <cstring>
#include <utility>

namespace hoyoflux::game {

std::unique_ptr<GameAdapter> make_adapter(GameId game) {
    switch (game) {
    case GameId::Genshin:
        return std::make_unique<GenshinAdapter>();
    case GameId::StarRail:
        return std::make_unique<StarRailAdapter>();
    }
    return nullptr;
}

const ResolvedSignature* find_resolved(const std::vector<ResolvedSignature>& resolved,
                                       std::string_view id) {
    for (const auto& entry : resolved) {
        if (entry.resolved && entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

Result<PersistentDisplayState> GameAdapter::snapshot_persistent_display_state()
    const {
    auto state = snapshot_persistent_roots(persistent_state_roots());
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->sets.empty()) {
        return std::unexpected(Error::make(
            ErrorCode::NotSupported,
            "no persistent display settings found for this game (nothing to "
            "protect); is the game installed and has it run once?"));
    }
    return state;
}

Result<void> GameAdapter::restore_persistent_display_state(
    const PersistentDisplayState& state) const {
    return restore_persistent_roots(state);
}

std::vector<std::wstring> GameAdapter::persistent_state_roots() const {
    return {};  // default: nothing known to protect
}

namespace {

// Best-effort decode of the Unity Screenmanager DWORDs for the diagnostic
// view; restore never relies on this.
void decode_screenmanager(PersistentDisplayState& state,
                          const std::wstring& name, const win32::RegistryValue& value) {
    if (value.type != REG_DWORD || value.data.size() != sizeof(uint32_t)) {
        return;
    }
    uint32_t dword = 0;
    std::memcpy(&dword, value.data.data(), sizeof(dword));
    if (name.rfind(L"Screenmanager Resolution Width H", 0) == 0) {
        state.resolution.emplace(dword, state.resolution ? state.resolution->height : 0);
    } else if (name.rfind(L"Screenmanager Resolution Height H", 0) == 0) {
        state.resolution.emplace(state.resolution ? state.resolution->width : 0,
                                 dword);
    } else if (name.rfind(L"Screenmanager Fullscreen mode H", 0) == 0) {
        state.fullscreen_mode = dword;
    }
}

}  // namespace

Result<PersistentDisplayState> snapshot_persistent_roots(
    const std::vector<std::wstring>& roots) {
    PersistentDisplayState state;
    for (const auto& root : roots) {
        auto values = win32::read_registry_values(root);
        if (!values) {
            return std::unexpected(values.error());
        }
        PersistentSettingSet set;
        set.root = root;
        for (auto& value : *values) {
            if (value.name.rfind(kUnityScreenmanagerPrefix, 0) != 0) {
                continue;
            }
            decode_screenmanager(state, value.name, value);
            PersistentSetting setting;
            setting.name = std::move(value.name);
            setting.type = value.type;
            setting.data = std::move(value.data);
            set.settings.push_back(std::move(setting));
        }
        if (!set.settings.empty()) {
            state.sets.push_back(std::move(set));
        }
    }
    return state;
}

Result<void> restore_persistent_roots(const PersistentDisplayState& state) {
    for (const auto& set : state.sets) {
        std::vector<win32::RegistryValue> values;
        values.reserve(set.settings.size());
        for (const auto& setting : set.settings) {
            values.push_back(win32::RegistryValue{setting.name, setting.type,
                                                  setting.data});
        }
        if (auto written = win32::write_registry_values(set.root, values);
            !written) {
            return std::unexpected(written.error());
        }
    }
    return {};
}

Result<void> validate_profile(const Profile& profile, const CapabilityReport& report) {
    // Which capabilities this profile actually exercises. FpsUnlock is
    // always on (runtime.fps has no "off" value); FullscreenMode only
    // matters when the render policy drives the display at all.
    const bool drive_render = profile.render.resolution.has_value();
    const std::pair<Capability, bool> required[] = {
        {Capability::FpsUnlock, true},
        {Capability::CustomResolution, drive_render},
        {Capability::FullscreenMode, drive_render && profile.render.fullscreen.has_value()},
        {Capability::MonitorSelection, profile.render.monitor.has_value()},
        {Capability::MobileUi, profile.ui.mobile_ui},
        {Capability::CustomDpi, profile.ui.dpi_scale.has_value()},
        {Capability::PowerSave,
         profile.runtime.power_save == PowerSavePolicy::Enabled},
        {Capability::PersistentStateGuard,
         drive_render && profile.render.persistence == ResolutionPersistence::Session},
    };

    for (const auto& [capability, needed] : required) {
        if (!needed) {
            continue;
        }
        const CapabilityEntry* entry = report.find(capability);
        const bool ok = entry != nullptr &&
                        entry->status == CapabilityStatus::Supported;
        if (ok) {
            continue;
        }
        std::string message = entry != nullptr && !entry->reason.empty()
                                  ? entry->reason
                                  : std::string(to_string(capability)) +
                                        " is not available for this game";
        return std::unexpected(
            Error::make(ErrorCode::NotSupported, std::move(message)));
    }
    return {};
}

// Shared resolver loop: both games scan their signatures against every
// provided snapshot and keep the first match. A signature is resolved when
// every one of its resolvers produced a value.
Result<std::vector<ResolvedSignature>> resolve_all_signatures(
    const std::vector<scan::Signature>& signatures,
    const std::vector<scan::ModuleSnapshot>& snapshots) {
    std::vector<ResolvedSignature> out;
    out.reserve(signatures.size());
    for (const auto& sig : signatures) {
        ResolvedSignature resolved;
        resolved.id = sig.id;
        resolved.fields.resize(sig.resolvers.size(), 0);
        bool matched = false;
        bool all_resolved = true;
        for (const auto& snapshot : snapshots) {
            const auto* section = snapshot.find_section(sig.section);
            if (section == nullptr) {
                continue;
            }
            auto match = scan::scan_first(sig.pattern, section->bytes);
            if (!match) {
                continue;
            }
            matched = true;
            for (size_t r = 0; r < sig.resolvers.size(); ++r) {
                auto address = scan::resolve_match(sig, *section, *match, r);
                if (!address) {
                    all_resolved = false;
                    continue;
                }
                resolved.fields[r] = *address;
            }
            break;  // first snapshot hosting a match wins
        }
        resolved.resolved = matched && all_resolved;
        out.push_back(std::move(resolved));
    }
    return out;
}

}  // namespace hoyoflux::game
