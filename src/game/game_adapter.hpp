#pragma once

// GameAdapter: the only module that knows game-specific facts.
//
// It locates the installation, decides which modules/sections to snapshot,
// and resolves the game's signatures. It never writes memory itself; the
// patch engine (A6) turns resolved knowledge into writes.

#include "domain/error.hpp"
#include "domain/game.hpp"
#include "domain/profile.hpp"
#include "scan/module_snapshot.hpp"

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

struct ResolvedSignature {
    std::string_view id;
    uintptr_t address{0};
    bool resolved{false};
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
    // (which must cover module_requirements()). Powers doctor and the patch
    // engine.
    virtual Result<std::vector<ResolvedSignature>> resolve_signatures(
        const std::vector<scan::ModuleSnapshot>& snapshots) const = 0;
};

// Factory: the adapter for a game id.
std::unique_ptr<GameAdapter> make_adapter(GameId game);

}  // namespace hoyoflux::game
