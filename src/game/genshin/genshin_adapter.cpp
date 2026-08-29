#include "game/genshin/genshin_adapter.hpp"

#include "game/genshin/signatures.hpp"
#include "platform/win32/registry.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace hoyoflux::game {
namespace {

constexpr uintmax_t kOldExeThresholdBytes = 0x800000;  // legacy: < 8 MB == old

// Version priority order used by the legacy fps scan (main.cpp:2168-2197):
// the first pattern that matches decides the fps address semantics.
constexpr std::string_view kFpsIdsByPriority[] = {
    "genshin.fps.5.5", "genshin.fps.5.4", "genshin.fps.3.7-5.3", "genshin.fps.old"};

}  // namespace

GameId GenshinAdapter::id() const { return GameId::Genshin; }

Result<GameInstall> GenshinAdapter::locate_installation(Region region) const {
    auto paths = win32::read_launcher_paths();
    if (!paths) {
        return std::unexpected(paths.error());
    }

    const bool try_cn = region != Region::Global;
    const bool try_global = region != Region::Cn;

    if (try_cn && paths->genshin_cn.has_value()) {
        return GameInstall{GameId::Genshin, true, *paths->genshin_cn};
    }
    if (try_global && paths->genshin_global.has_value()) {
        return GameInstall{GameId::Genshin, false, *paths->genshin_global};
    }
    return std::unexpected(Error::make(
        ErrorCode::ProcessNotFound,
        "Genshin Impact installation not found (CN or Global launcher registry)"));
}

Result<bool> GenshinAdapter::is_old_version(const GameInstall& install) const {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(install.exe_path, ec);
    if (ec) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "cannot stat game executable: " + install.exe_path.string(),
            ec.value()));
    }
    return size < kOldExeThresholdBytes;
}

Result<ModuleRequirements> GenshinAdapter::module_requirements(
    const GameInstall& install, const Profile& /*profile*/) const {
    auto old_version = is_old_version(install);
    if (!old_version) {
        return std::unexpected(old_version.error());
    }

    ModuleRequirements requirements;
    if (*old_version) {
        // Legacy: engine logic in UnityPlayer.dll, il2cpp in UserAssembly.dll
        // (injected by path into the suspended process).
        requirements.modules.push_back(
            {"UnityPlayer.dll", {genshin::kTextSection.data()}, /*inject=*/true});
        requirements.modules.push_back(
            {"UserAssembly.dll", {genshin::kIl2CppSection.data()}, /*inject=*/true});
    } else {
        // Modern builds merged the game logic and il2cpp into the main exe.
        requirements.modules.push_back(
            {"", {genshin::kTextSection.data(), genshin::kIl2CppSection.data()},
             /*inject=*/false});
    }
    return requirements;
}

Result<std::vector<ResolvedSignature>> GenshinAdapter::resolve_signatures(
    const std::vector<scan::ModuleSnapshot>& snapshots) const {
    std::vector<scan::Signature> signatures;
    signatures.reserve(genshin::all_ids().size());
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
        if (!sig) {
            return std::unexpected(sig.error());
        }
        signatures.push_back(std::move(*sig));
    }
    return resolve_all_signatures(signatures, snapshots);
}

Result<PatchPlan> GenshinAdapter::build_patch_plan(const PatchContext& context) const {
    PatchPlan plan;
    plan.runtime.near_address = context.primary_module_base;
    plan.runtime.initial_fps = context.profile.runtime.fps;
    plan.runtime.initial_flags =
        (context.profile.ui.mobile_ui ? kFlagMobileUi : 0) |
        (context.profile.runtime.power_save == PowerSavePolicy::Enabled ? kFlagPowerSave
                                                                        : 0);

    // FPS: the first resolved signature in legacy priority order yields the
    // address of a rip-relative displacement field inside the game's
    // fps-read instruction. Redirecting it at the RemoteState fps slot is
    // the 1.0.0 replacement for the legacy Private_buffer + sync-thread
    // mechanism (main.cpp:1512-1532): the game then reads its fps from a
    // block only we write - no resident launcher, no remote thread.
    const ResolvedSignature* fps = nullptr;
    for (const auto id : kFpsIdsByPriority) {
        fps = find_resolved(context.resolved, id);
        if (fps != nullptr) {
            break;
        }
    }
    if (fps == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "no Genshin fps signature resolved; game version likely unsupported "
            "(run `hoyoflux doctor`)"));
    }
    plan.operations.push_back(PatchOperation::redirect_relative(
        fps->fields[0], PatchTargetSymbol::RemoteStateFps, 0));

    // Custom DPI: replace the GetDPI prologue with
    //   mov eax, dpi*96 ; movd xmm0, eax ; ret  (main.cpp:1551-1557)
    if (context.profile.ui.dpi_scale.has_value()) {
        const ResolvedSignature* dpi = find_resolved(context.resolved, "genshin.dpi");
        if (dpi == nullptr) {
            return std::unexpected(Error::make(
                ErrorCode::SignatureNotFound,
                "custom dpi requested but the genshin.dpi signature did not "
                "resolve (run `hoyoflux doctor`)"));
        }
        const float dpi_pixels = *context.profile.ui.dpi_scale * 96.0f;
        std::array<std::byte, 16> prologue{};
        prologue[0] = std::byte{0xB8};  // mov eax, imm32
        std::memcpy(prologue.data() + 1, &dpi_pixels, sizeof(dpi_pixels));
        prologue[5] = std::byte{0x66};   // movd xmm0, eax
        prologue[6] = std::byte{0x0F};
        prologue[7] = std::byte{0x6E};
        prologue[8] = std::byte{0xC0};
        prologue[9] = std::byte{0xC3};  // ret
        for (size_t i = 10; i < prologue.size(); ++i) {
            prologue[i] = std::byte{0xCC};
        }
        plan.operations.push_back(
            PatchOperation::write_bytes(dpi->fields[0], prologue));
    }

    // Deliberately deferred (in-process only, see the 1.0.0 plan §A6
    // "注入方式目标"): the verify detour, the UI-unhook detour, the
    // il2cpp MobileUI calls and the in-game fps-set hook all execute code
    // inside the game and need a bootstrap thread the external-only model
    // does not provide. They stay unapplied until real-game validation
    // (plan verification point 1) proves one is required, in which case a
    // minimal targeted stub gets added with a recorded reason.
    return plan;
}

}  // namespace hoyoflux::game
