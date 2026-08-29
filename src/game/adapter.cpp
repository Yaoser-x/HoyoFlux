#include "game/game_adapter.hpp"

#include "game/genshin/genshin_adapter.hpp"
#include "game/starrail/starrail_adapter.hpp"

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

}  // namespace hoyoflux::game
