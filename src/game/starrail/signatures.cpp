#include "game/starrail/signatures.hpp"

#include <string>

namespace hoyoflux::game::starrail {
namespace {

using scan::ResolveSpec;
using scan::ResolveStrategy;

ResolveSpec rip_rel(int64_t off, int64_t skip) {
    return ResolveSpec{ResolveStrategy::RipRelative, off, skip};
}
ResolveSpec field_disp(int64_t off) {
    return ResolveSpec{ResolveStrategy::FieldDisp, off, 0};
}

constexpr std::string_view kFps =
    "66 0F 6E 05 ?? ?? ?? ?? F2 0F 10 3D ?? ?? ?? ?? 0F 5B C0";
constexpr std::string_view kFpsMovFlip =
    "CC 89 0D ?? ?? ?? ?? E9 ?? ?? ?? ?? CC CC CC CC CC";
constexpr std::string_view kUISetV1 =
    "80 B9 ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 03 00 00 00 "
    "48 83 C4 20 5E C3";
constexpr std::string_view kUISetV2 =
    "80 B9 ?? ?? ?? ?? 00 74 ?? C7 05 ?? ?? ?? ?? 03 00 00 00 48 83 C4 20 5E C3";
constexpr std::string_view kUISetV3 =
    "75 05 E8 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 03 00 00 00 48 83 C4 28 C3";

}  // namespace

const std::vector<SignatureId>& all_ids() {
    static const std::vector<SignatureId> ids = {
        SignatureId::Fps, SignatureId::FpsMovFlip, SignatureId::UISetV1,
        SignatureId::UISetV2, SignatureId::UISetV3,
    };
    return ids;
}

Result<scan::Signature> signature(SignatureId id) {
    switch (id) {
    case SignatureId::Fps:
        // legacy: rip = addr+4; rip += *(i32)(rip) + 4
        return scan::make_signature("starrail.fps", kTextSection, kFps,
                                    {rip_rel(4, 4)});
    case SignatureId::FpsMovFlip:
        // resolver 0: the rip target (== the Fps holder when the flip
        // applies); resolver 1: the patch site, match+1 - the 0x89 opcode
        // byte that becomes 0x8B (main.cpp:2225-2231).
        return scan::make_signature("starrail.fpsmovflip", kTextSection,
                                    kFpsMovFlip, {rip_rel(3, 4), field_disp(1)});
    case SignatureId::UISetV1:
        // legacy: tar=addr+15; rip = tar + *(i32)(tar) + 8  (skip over the
        // "C7 05 disp imm32" instruction's imm32)
        return scan::make_signature("starrail.uiset.v1", kIl2CppSection, kUISetV1,
                                    {rip_rel(15, 8)});
    case SignatureId::UISetV2:
        return scan::make_signature("starrail.uiset.v2", kIl2CppSection, kUISetV2,
                                    {rip_rel(11, 8)});
    case SignatureId::UISetV3:
        return scan::make_signature("starrail.uiset.v3", kIl2CppSection, kUISetV3,
                                    {rip_rel(9, 8)});
    }
    return std::unexpected(Error::make(ErrorCode::InvalidArgument,
                                       "unknown starrail signature id"));
}

}  // namespace hoyoflux::game::starrail
