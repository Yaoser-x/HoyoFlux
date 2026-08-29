#pragma once

// GenshinMobileUiPatchBuilder (F5, plan section 11.3): composes the in-game
// Mobile UI bootstrap from the resolved mobile-UI signatures. Per-game by
// design - no cross-game branching.
//
// The generated stub runs inside the suspended game (InvokeBootstrap) after
// the engine modules exist: it calls the game's gui_set / input_set functions
// with the resolved object + class pointers, which switches the runtime UI
// mode to mobile.
//
// REAL-GAME GATE: kPayloadValidated stays false until the call sequence has
// been proven against the live game (plan B1). While false, build_patch_plan
// refuses to install the stub - the capability contract reports MobileUi as
// Unsupported instead of shipping an unverified patch.

#include "game/game_adapter.hpp"

#include <vector>

namespace hoyoflux::game::genshin {

class GenshinMobileUiPatchBuilder {
public:
    // True once the stub payload has been validated on a real game.
    static constexpr bool kPayloadValidated = false;

    // Appends InstallCodeStub + InvokeBootstrap to `plan`. Every required
    // signature must be resolved; a missing one is an error, never a silent
    // skip.
    [[nodiscard]] static Result<void> add_operations(PatchPlan& plan,
                                                     const PatchContext& context);

    // The bootstrap machine code for the resolved signatures (exposed for
    // tests and for the real-game validation tooling).
    [[nodiscard]] static std::vector<std::byte>
    build_stub(const std::vector<ResolvedSignature>& resolved);
};

}  // namespace hoyoflux::game::genshin
