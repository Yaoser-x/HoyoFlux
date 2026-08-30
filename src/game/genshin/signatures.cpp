#include "game/genshin/signatures.hpp"

#include <string>

namespace hoyoflux::game::genshin {
namespace {

using scan::ResolveSpec;
using scan::ResolveStrategy;
using scan::Signature;

// Convenience builders for the resolve shapes the legacy code uses.
ResolveSpec field_disp(int64_t off) {
    return ResolveSpec{ResolveStrategy::FieldDisp, off, 0};
}
ResolveSpec rip_rel(int64_t off, int64_t skip) {
    return ResolveSpec{ResolveStrategy::RipRelative, off, skip};
}
ResolveSpec raw_i32(int64_t off) {
    return ResolveSpec{ResolveStrategy::RawInt32At, off, 0};
}

constexpr std::string_view kFps55 =
    "66 0F 6E 0D ?? ?? ?? ?? 0F 57 C0 0F 5B C9";
constexpr std::string_view kFps54 =
    "7E 0C E8 ?? ?? ?? ?? 66 0F 6E C8 0F 5B C9";
constexpr std::string_view kFps37To53 =
    "7F 0E E8 ?? ?? ?? ?? 66 0F 6E C8";
constexpr std::string_view kFpsOld =
    "7F 0F 8B 05 ?? ?? ?? ?? 66 0F 6E C8";
constexpr std::string_view kUnityWndclass =
    "C7 44 24 28 00 00 00 80 C7 44 24 20 00 00 00 80 FF 15 ?? ?? ?? ?? "
    "48 89 05 ?? ?? ?? ?? 48 85 C0";
constexpr std::string_view kPayloadOep =
    "48 83 EC 28 FF D1 31 C0 48 83 C4 28 C3";
constexpr std::string_view kVersionSignV1 =
    "0F 10 05 ?? ?? ?? ?? 0F 11 41 ?? 0F 10 05 ?? ?? ?? ?? 0F 11 41 ?? "
    "0F 10 05 ?? ?? ?? ?? 0F 11 41 ?? 0F 10 05 ?? ?? ?? ?? 0F 11 01";
constexpr std::string_view kVersionSignV2 =
    "48 83 FA 40 41 B8 40 00 00 00 4C 0F 42 C2 48 8D 15 ?? ?? ?? ?? 48 89 F1 E8";

constexpr std::string_view kHookGameSet =
    "48 89 F1 E8 ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 48 8B 0D";
constexpr std::string_view kVerifyV1 =
    "E8 ?? ?? ?? ?? EB 0D 48 89 F1 BA 02 00 00 00 E8 ?? ?? ?? ?? 48 89 F1 31 D2";
constexpr std::string_view kVerifyV2 =
    "E8 ?? ?? ?? ?? EB 0D 48 89 F1 BA 02 00 00 00 E8 ?? ?? ?? ?? 48 8B 0D";
constexpr std::string_view kMobileUiV1 =
    "48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 0F ?? ?? ?? ?? ?? "
    "BA 02 00 00 00 41 B0 01 E8 ?? ?? ?? ?? 48 89 F9 BA 03 00 00 00 45 31 C0 E8";
constexpr std::string_view kMobileUiV2 =
    "48 8B 05 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 0F ?? ?? ?? ?? ?? "
    "BA 02 00 00 00 E8 ?? ?? ?? ?? 48 89 F9 BA 03 00 00 00 E8";
constexpr std::string_view kMobileUiInput =
    "48 8B 05 ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B B8 ?? ?? ?? ?? 48 85 FF "
    "0F 84 ?? ?? ?? ?? 83 BF ?? ?? ?? ?? 03";
constexpr std::string_view kUnhookTime =
    "E8 ?? ?? ?? ?? 48 89 D9 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 85 "
    "?? ?? ?? ?? 48 8B 0D";
constexpr std::string_view kDpi =
    "0F 14 F8 E8 ?? ?? ?? ?? 0F 14 F0 0F 59 F7";

}  // namespace

const std::vector<SignatureId>& all_ids() {
    static const std::vector<SignatureId> ids = {
        SignatureId::Fps55,       SignatureId::Fps54,
        SignatureId::Fps37To53,   SignatureId::FpsOld,
        SignatureId::UnityWndclass, SignatureId::PayloadOep,
        SignatureId::VersionSignV1, SignatureId::VersionSignV2,
        SignatureId::HookGameSet, SignatureId::VerifyV1,
        SignatureId::VerifyV2,    SignatureId::MobileUiV1,
        SignatureId::MobileUiV2,  SignatureId::MobileUiInput,
        SignatureId::UnhookTime,  SignatureId::Dpi,
    };
    return ids;
}

Result<scan::Signature> signature(SignatureId id) {
    switch (id) {
    case SignatureId::Fps55:
        // legacy: rip = addr + 4  (address of the disp field; no disp add)
        return scan::make_signature("genshin.fps.5.5", kTextSection, kFps55,
                                    {field_disp(4)});
    case SignatureId::Fps54:
        // legacy: rip = addr+3; rip += *(i32)(rip) + 6
        return scan::make_signature("genshin.fps.5.4", kTextSection, kFps54,
                                    {rip_rel(3, 6)});
    case SignatureId::Fps37To53:
        // legacy: rip = addr+3; rip += *(i32)(rip) + 6
        return scan::make_signature("genshin.fps.3.7-5.3", kTextSection, kFps37To53,
                                    {rip_rel(3, 6)});
    case SignatureId::FpsOld:
        // legacy: rip = addr + 4  (address of the disp field)
        return scan::make_signature("genshin.fps.old", kTextSection, kFpsOld,
                                    {field_disp(4)});
    case SignatureId::UnityWndclass:
        // legacy: rip = addr+0x19; rip += *(i32)(rip) + 4
        return scan::make_signature("genshin.unitywndclass", kTextSection,
                                    kUnityWndclass, {rip_rel(0x19, 4)});
    case SignatureId::PayloadOep:
        return scan::make_signature("genshin.payloadoep", kTextSection,
                                    kPayloadOep, {ResolveSpec{ResolveStrategy::Direct, 0, 0}});
    case SignatureId::VersionSignV1:
        // display-only; resolves to a version string address
        return scan::make_signature("genshin.versign.v1", kTextSection,
                                    kVersionSignV1, {rip_rel(0x24, 4)});
    case SignatureId::VersionSignV2:
        return scan::make_signature("genshin.versign.v2", kTextSection,
                                    kVersionSignV2, {rip_rel(0x11, 4)});

    case SignatureId::HookGameSet:
        // legacy: rip = addr+10; rip += *(i32)(rip) + 4
        return scan::make_signature("genshin.hookgameset", kIl2CppSection,
                                    kHookGameSet, {rip_rel(10, 4)});
    case SignatureId::VerifyV1:
        // legacy: rip = addr+1; rip += *(i32)(rip) + 4
        return scan::make_signature("genshin.verify.v1", kIl2CppSection, kVerifyV1,
                                    {rip_rel(1, 4)});
    case SignatureId::VerifyV2:
        return scan::make_signature("genshin.verify.v2", kIl2CppSection, kVerifyV2,
                                    {rip_rel(1, 4)});
    case SignatureId::MobileUiV1:
        // legacy 4 fields, in order: Grph_class(addr+3+disp+4),
        // Grph_UIcl_VA(*(i32)(addr+0xA)), Func_gui_set(addr+0x20+disp+4),
        // Func_input_set(addr+0x30+disp+4)
        return scan::make_signature(
            "genshin.mobileui.v1", kIl2CppSection, kMobileUiV1,
            {rip_rel(3, 4), raw_i32(0xA), rip_rel(0x20, 4), rip_rel(0x30, 4)});
    case SignatureId::MobileUiV2:
        // same 4 fields, offsets 3 / 0xA / 0x1D / 0x2A
        return scan::make_signature(
            "genshin.mobileui.v2", kIl2CppSection, kMobileUiV2,
            {rip_rel(3, 4), raw_i32(0xA), rip_rel(0x1D, 4), rip_rel(0x2A, 4)});
    case SignatureId::MobileUiInput:
        // legacy: Grph_inputcl_VA = *(int32*)(addr+0x10)
        return scan::make_signature("genshin.mobileui.input", kIl2CppSection,
                                    kMobileUiInput, {raw_i32(0x10)});
    case SignatureId::UnhookTime:
        // Lifecycle call site plus its original callee. Mobile UI redirects
        // the call displacement to a near one-shot stub running on the game
        // thread; the stub calls the original callee first.
        return scan::make_signature("genshin.unhooktime", kIl2CppSection,
                                    kUnhookTime,
                                    {field_disp(9), rip_rel(9, 4)});
    case SignatureId::Dpi:
        // legacy: rip = addr+4; rip += *(i32)(rip) + 4
        return scan::make_signature("genshin.dpi", kIl2CppSection, kDpi,
                                    {rip_rel(4, 4)});
    }
    return std::unexpected(Error::make(ErrorCode::InvalidArgument,
                                       "unknown genshin signature id"));
}

}  // namespace hoyoflux::game::genshin
