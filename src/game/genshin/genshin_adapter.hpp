#pragma once

#include "game/game_adapter.hpp"

namespace hoyoflux::game {

class GenshinAdapter final : public GameAdapter {
public:
    GameId id() const override;
    Result<GameInstall> locate_installation(Region region) const override;
    Result<bool> is_old_version(const GameInstall& install) const override;
    Result<ModuleRequirements> module_requirements(
        const GameInstall& install, const Profile& profile) const override;
    Result<std::vector<ResolvedSignature>> resolve_signatures(
        const std::vector<scan::ModuleSnapshot>& snapshots) const override;
};

}  // namespace hoyoflux::game
