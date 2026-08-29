#pragma once

// StarRailMobileUiPatchBuilder (F5, plan section 11.3): per-game Mobile UI
// composition for Star Rail's starrail.uiset.* signatures.
//
// REAL-GAME GATE: kPayloadValidated stays false until the call sequence has
// been proven against the live game (plan B1). While false, build_patch_plan
// refuses to install the stub - MobileUi stays Unsupported in the capability
// report rather than shipping an unverified patch.

#include "game/game_adapter.hpp"

#include <vector>

namespace hoyoflux::game::starrail {

class StarRailMobileUiPatchBuilder {
public:
    static constexpr bool kPayloadValidated = false;

    [[nodiscard]] static Result<void> add_operations(PatchPlan& plan,
                                                     const PatchContext& context);

    [[nodiscard]] static std::vector<std::byte>
    build_stub(const std::vector<ResolvedSignature>& resolved);
};

}  // namespace hoyoflux::game::starrail
