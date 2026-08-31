#pragma once

// Honkai: Star Rail signatures, data-ified from the legacy main.cpp
// (reference: HKSR fps pattern ~2212, mov-flip ~2220, UISet variants
// ~1858-1868). Game updates only touch this file (and starrail_adapter.cpp).

#include "domain/error.hpp"
#include "scan/signature.hpp"

#include <string_view>
#include <vector>

namespace hoyoflux::game::starrail {

constexpr std::string_view kTextSection = ".text";
constexpr std::string_view kIl2CppSection = "il2cpp";

enum class SignatureId {
    Fps,        // FPS variable holder ("ver 1.0 - last")
    FpsMovFlip, // 1-byte mov-opcode flip; conditional on resolving to the Fps target
    UISetV1,    // mobile UI set function (GameAssembly.dll il2cpp)
    UISetV2,
    UISetV3,
};

Result<scan::Signature> signature(SignatureId id);

const std::vector<SignatureId>& all_ids();

}  // namespace hoyoflux::game::starrail
