#pragma once

// GameAdapter: the only module that knows game-specific facts.
//
// It locates the installation, decides which modules/sections to snapshot,
// resolves the game's signatures, and turns resolved knowledge + profile
// into a PatchPlan. It never writes memory itself; the patch engine (A6)
// executes the plan.

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/patch_plan.hpp"
#include "domain/profile.hpp"
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

    // Locate the game executable (launcher registry). Region::Auto prefers
    // the CN install, falling back to Global.
    virtual Result<GameInstall> locate_installation(Region region) const = 0;

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

// First resolved signature with this id, or nullptr.
const ResolvedSignature* find_resolved(const std::vector<ResolvedSignature>& resolved,
                                       std::string_view id);

// Shared resolver loop used by both adapters: scans each signature in every
// snapshot, keeps the first match and resolves all of its resolver specs.
Result<std::vector<ResolvedSignature>> resolve_all_signatures(
    const std::vector<scan::Signature>& signatures,
    const std::vector<scan::ModuleSnapshot>& snapshots);

}  // namespace hoyoflux::game
