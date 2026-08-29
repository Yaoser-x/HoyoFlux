#pragma once

// GameAdapter: the only module that knows game-specific facts.
//
// It locates the installation, decides which modules/sections to snapshot,
// resolves the game's signatures, and turns resolved knowledge + profile
// into a PatchPlan. It never writes memory itself; the patch engine (A6)
// executes the plan.

#include "domain/capability.hpp"
#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/launch_request.hpp"
#include "domain/patch_plan.hpp"
#include "domain/persistent_state.hpp"
#include "domain/profile.hpp"
#include "game/launch/launch_plan.hpp"
#include "scan/module_snapshot.hpp"
#include "scan/signature.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux::game {

enum class Region { Cn, Global, Auto };

struct ModuleRequirement {
    std::string module;  // exe/dll file name; empty = the main module
    std::vector<std::string> sections;
    bool inject_if_missing{false};  // legacy injected engine dlls by path
};

struct ModuleRequirements {
    std::vector<ModuleRequirement> modules;
};

// One entry per resolver of the underlying scan::Signature, in order.
// A signature counts as resolved when every resolver produced a value;
// RawInt32At fields may legitimately be 0 (they carry data, not pointers).
struct ResolvedSignature {
    std::string_view id;
    bool resolved{false};
    std::vector<uintptr_t> fields;
};

// Everything build_patch_plan needs, gathered by the session engine.
struct PatchContext {
    const std::vector<ResolvedSignature>& resolved;
    const Profile& profile;
    // Module base used as the allocation anchor (rip-relative redirects must
    // stay within +-2GB of the code they point at).
    uintptr_t primary_module_base{0};
    bool old_version{false};
};

class GameAdapter {
public:
    virtual ~GameAdapter() = default;

    virtual GameId id() const = 0;

    // Honest per-feature status for this install + profile. Features the
    // profile does not use are reported NotRequired; the rest must be
    // Supported or Unsupported (SignatureMissing is only produced once
    // signatures have actually been resolved, e.g. by doctor).
    virtual CapabilityReport capabilities(const GameInstall& install,
                                          const Profile& profile) const = 0;

    // Locate the game executable (launcher registry). Region::Auto prefers
    // the CN install, falling back to Global.
    virtual Result<GameInstall> locate_installation(Region region) const = 0;

    // F1: turn install + validated request (profile render policy + raw
    // passthrough) into the exact process plan the engine executes. This is
    // the only place game launch arguments exist; the session engine never
    // sees a Unity flag.
    virtual Result<GameLaunchPlan> build_launch_plan(
        const GameInstall& install, const LaunchRequest& request) const = 0;

    // F2: the game's own persisted display settings (its config for the NEXT
    // launch), distinct from the physical Windows display mode. Snapshot
    // before the session, restore after it - this is what keeps an iPad
    // resolution session from leaking into official launcher launches.
    virtual Result<PersistentDisplayState> snapshot_persistent_display_state()
        const;
    virtual Result<void> restore_persistent_display_state(
        const PersistentDisplayState& state) const;

    // HKCU subkeys this game persists display settings into. Games override;
    // the default (empty) means "nothing known to protect". Public because
    // doctor / state-dump must report on the candidate roots themselves.
    virtual std::vector<std::wstring> persistent_state_roots() const;

    // Version detection (e.g. Genshin old-vs-new by exe size).
    virtual Result<bool> is_old_version(const GameInstall& install) const = 0;

    // Which modules and sections the launch flow must snapshot (and possibly
    // inject) for this install + profile.
    virtual Result<ModuleRequirements> module_requirements(
        const GameInstall& install, const Profile& profile) const = 0;

    // Resolve every signature this game knows against the provided snapshots
    // (which must cover module_requirements()). Powers doctor and
    // build_patch_plan.
    virtual Result<std::vector<ResolvedSignature>> resolve_signatures(
        const std::vector<scan::ModuleSnapshot>& snapshots) const = 0;

    // Turn resolved signatures + profile into the patch plan the patch
    // engine executes. Missing signatures that the profile needs (fps) are
    // an error; optional ones (dpi) fail only when the profile asks for them.
    virtual Result<PatchPlan> build_patch_plan(const PatchContext& context) const = 0;
};

// Factory: the adapter for a game id.
std::unique_ptr<GameAdapter> make_adapter(GameId game);

// F0 gate: every profile feature the report marks Unsupported must be
// unused by the profile. Returns an error naming the first violated
// capability (its `reason` is the message body); a valid profile passes.
// Called before the game process exists, so an unsupported feature stops
// the launch instead of being silently ignored.
[[nodiscard]] Result<void> validate_profile(const Profile& profile,
                                            const CapabilityReport& report);

// Shared F2 implementation used by the adapters: capture every
// "Screenmanager*" value stored under each existing root (Unity's standard
// persistent display settings); restore replays them verbatim. The watched
// roots come from the adapter, never a hardcoded value list - the
// real-machine A/B experiment (docs/persistent-state-experiment.md)
// validates coverage.
[[nodiscard]] Result<PersistentDisplayState> snapshot_persistent_roots(
    const std::vector<std::wstring>& roots);
[[nodiscard]] Result<void> restore_persistent_roots(
    const PersistentDisplayState& state);

// True when both states carry exactly the same roots and settings (used to
// verify a recovery before the journal may be cleared).
[[nodiscard]] bool persistent_state_equals(const PersistentDisplayState& a,
                                           const PersistentDisplayState& b);

// Unity Screenmanager value-name prefix (stable across Unity versions; the
// hash suffix is not).
inline constexpr std::wstring_view kUnityScreenmanagerPrefix = L"Screenmanager";

// First resolved signature with this id, or nullptr.
const ResolvedSignature* find_resolved(const std::vector<ResolvedSignature>& resolved,
                                       std::string_view id);

// Shared resolver loop used by both adapters: scans each signature in every
// snapshot, keeps the first match and resolves all of its resolver specs.
Result<std::vector<ResolvedSignature>> resolve_all_signatures(
    const std::vector<scan::Signature>& signatures,
    const std::vector<scan::ModuleSnapshot>& snapshots);

}  // namespace hoyoflux::game
