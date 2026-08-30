#pragma once

// GenshinMobileUiPatchBuilder (F5, plan section 11.3): composes the in-game
// Mobile UI function-entry stub from the resolved mobile-UI signatures. Per-game by
// design - no cross-game branching.
//
// The generated stub is reached from the upstream lifecycle function entry.
// It self-unhooks the saved 16 bytes, resumes the original function, resolves
// UI/input objects and calls both setters on that game thread. Genshin never
// uses InvokeBootstrap/CreateRemoteThread for Mobile UI.
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

    // Appends one InstallFunctionEntryDetour. mobileui.v1/v2, mobileui.input and
    // unhooktime must all resolve; a missing field is an explicit error.
    [[nodiscard]] static Result<void> add_operations(PatchPlan& plan,
                                                     const PatchContext& context);

    // The function-entry machine code for the resolved signatures (exposed for
    // tests and for the real-game validation tooling).
    [[nodiscard]] static std::vector<std::byte>
    build_stub(const std::vector<ResolvedSignature>& resolved);
};

}  // namespace hoyoflux::game::genshin
