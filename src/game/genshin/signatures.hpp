#pragma once

// Genshin Impact signatures (CN + Global), data-ified from the legacy
// main.cpp signature block (reference lines ~2080-2405).
//
// Every entry records the section to scan, the exact hex pattern, and the
// resolve arithmetic the legacy code applied (quoted in comments). Game
// updates only touch this file (and genshin_adapter.cpp, A5).

#include "domain/error.hpp"
#include "scan/signature.hpp"

#include <string_view>
#include <vector>

namespace hoyoflux::game::genshin {

constexpr std::string_view kTextSection = ".text";
constexpr std::string_view kIl2CppSection = "il2cpp";

enum class SignatureId {
    // ---- .text ----
    Fps55,        // Genshin 5.5
    Fps54,        // Genshin 5.4
    Fps37To53,    // Genshin 3.7 - 5.3
    FpsOld,       // older versions
    UnityWndclass,// PowerSave / window-class holder (P_UnityWndclass)
    PayloadOep,   // payload entry (Direct); FF E1 fallback handled in adapter
    VersionSignV1,// display-only version string (skipped by doctor, not patched)
    VersionSignV2,// display-only version string
    // ---- il2cpp (old client: UserAssembly.dll) ----
    HookGameSet,  // in-game 30/60 fps-set hook (IsHookGameSet)
    VerifyV1,     // code-verify function (detoured)
    VerifyV2,     // code-verify function variant
    MobileUiV1,   // 4 resolvers: Grph_class / Grph_UIcl_VA / Func_gui_set / Func_input_set
    MobileUiV2,   // 4 resolvers (older layout)
    MobileUiInput,// Grph_inputcl_VA raw field
    UnhookTime,   // UI-unhook timing function
    Dpi,          // GetDPI function (prologue replaced when CustomDPIScale set)
};

// Build the requested signature. Errors are impossible for the fixed tables
// but returned for future edits.
Result<scan::Signature> signature(SignatureId id);

// All signatures, in declaration order (used by doctor / version matching).
const std::vector<SignatureId>& all_ids();

}  // namespace hoyoflux::game::genshin
