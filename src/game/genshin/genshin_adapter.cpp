#include "game/genshin/genshin_adapter.hpp"

#include "game/genshin/mobile_ui.hpp"
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

CapabilityReport GenshinAdapter::capabilities(const GameInstall& /*install*/,
                                              const Profile& profile) const {
    // One entry per user-visible feature. Statuses here are build facts, not
    // aspirations: anything marked Unsupported makes validate_profile stop
    // the launch instead of running a silent no-op. Deferred items carry the
    // phase that will implement them (1.0.0 plan F-phases).
    CapabilityReport report;
    auto add = [&](Capability capability, CapabilityStatus status,
                   std::string reason) {
        report.entries.push_back({capability, status, std::move(reason)});
    };

    const bool drive_render = profile.render.resolution.has_value();
    const auto& fullscreen = profile.render.fullscreen;

    add(Capability::FpsUnlock, CapabilityStatus::Supported,
        "fps redirect patch (first resolved genshin.fps.* signature)");

    // F7: hotkey service (RegisterHotKey) + the RemoteState fps channel.
    add(Capability::DynamicFps,
        profile.runtime.hotkeys ? CapabilityStatus::Supported
                                : CapabilityStatus::NotRequired,
        "END toggles fps control, Ctrl+Up/Down steps it (real-machine gate "
        "pending)");

    add(Capability::CustomResolution,
        drive_render ? CapabilityStatus::Supported : CapabilityStatus::NotRequired,
        "-screen-width/-screen-height launch arguments (real-machine gate "
        "pending: plan F1)");

    // Borderless is not expressible through Unity launch arguments for
    // Genshin; only windowed (0) and the game's fullscreen (1) are honest
    // mappings. Borderless becomes available again via the persistent-state
    // path (plan F2/F3) or by setting it in-game once.
    CapabilityStatus fullscreen_status = CapabilityStatus::Unsupported;
    std::string fullscreen_reason;
    if (!drive_render || !fullscreen.has_value()) {
        fullscreen_status = CapabilityStatus::NotRequired;
    } else if (*fullscreen == FullscreenMode::Windowed ||
               *fullscreen == FullscreenMode::Exclusive) {
        fullscreen_status = CapabilityStatus::Supported;
        fullscreen_reason = "-screen-fullscreen launch argument";
    } else {
        fullscreen_reason =
            "borderless fullscreen cannot be set via Genshin launch "
            "arguments; use \"windowed\"/\"exclusive\" or set it in-game";
    }
    add(Capability::FullscreenMode, fullscreen_status, std::move(fullscreen_reason));

    // Plan F1 §3.5: no monitor-selection mechanism has been verified on a
    // real machine, so requesting one must stop the launch.
    add(Capability::MonitorSelection,
        profile.render.monitor.has_value() ? CapabilityStatus::Unsupported
                                           : CapabilityStatus::NotRequired,
        "monitor selection has no verified mechanism for Genshin in this "
        "build; remove \"monitor\" from the profile");

    // F5: bootstrap mechanism exists (stub install + remote invocation);
    // the payload stays gated off until validated on the live game (B1).
    add(Capability::MobileUi, CapabilityStatus::Unsupported,
        "Mobile UI is not implemented for Genshin in this build: the "
        "bootstrap mechanism exists but its payload is unvalidated (plan B1)");

    add(Capability::CustomDpi,
        profile.ui.dpi_scale.has_value() ? CapabilityStatus::Supported
                                         : CapabilityStatus::NotRequired,
        "GetDPI prologue replacement patch (genshin.dpi)");

    // F6: event-driven foreground hook, exactly one 4-byte write per focus
    // change through the RemoteState fps channel. Disabled means no listener
    // exists at all.
    add(Capability::PowerSave,
        profile.runtime.power_save == PowerSavePolicy::Enabled
            ? CapabilityStatus::Supported
            : CapabilityStatus::NotRequired,
        "EVENT_SYSTEM_FOREGROUND throttle to power_save_fps (real-machine "
        "gate pending)");

    // F2/F3: snapshot + event-driven guard + final restore. Marked
    // Supported because the mechanism is real in this build; the four
    // real-machine gates (Tests A-D) remain the release condition.
    const bool guard_needed =
        drive_render && profile.render.persistence == ResolutionPersistence::Session;
    add(Capability::PersistentStateGuard,
        guard_needed ? CapabilityStatus::Supported : CapabilityStatus::NotRequired,
        "snapshot + RegNotifyChangeKeyValue guard + final restore of the "
        "game's Screenmanager values (real-machine gate pending)");

    return report;
}

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

Result<GameLaunchPlan> GenshinAdapter::build_launch_plan(
    const GameInstall& install, const LaunchRequest& request) const {
    auto render_args = build_render_arguments(request.profile.render);
    if (!render_args) {
        return std::unexpected(render_args.error());
    }
    // Managed render fields first, verbatim user passthrough after - the
    // game's own argv parser sees profile settings win by construction, and
    // conflicts were rejected outright in merge_passthrough.
    auto arguments = merge_passthrough(*render_args, request.game_args,
                                       to_string(GameId::Genshin));
    if (!arguments) {
        return std::unexpected(arguments.error());
    }

    GameLaunchPlan plan;
    plan.executable = install.exe_path;
    plan.working_directory = install.exe_path.parent_path();
    plan.arguments = std::move(*arguments);
    plan.priority = request.profile.runtime.priority;
    return plan;
}

std::vector<std::wstring> GenshinAdapter::persistent_state_roots() const {
    // Unity Screenmanager settings live under the game's own HKCU key. The
    // CN and Global installs use different key names; both are watched (only
    // existing ones produce a snapshot). The real-machine A/B experiment
    // (docs/persistent-state-experiment.md) validates this root list.
    return {L"Software\\miHoYo\\原神", L"Software\\miHoYo\\Genshin Impact"};
}

Result<bool> GenshinAdapter::is_old_version(const GameInstall& install) const {    std::error_code ec;
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

    // Mobile UI (F5): the bootstrap mechanism exists (stub install + remote
    // invocation, see genshin::GenshinMobileUiPatchBuilder) but the payload has not
    // been validated against the live game yet (plan B1). Until then the
    // gate below refuses to patch - the capability contract reports MobileUi
    // as Unsupported, so validate_profile normally stops the launch first.
    if (context.profile.ui.mobile_ui) {
        if (!genshin::GenshinMobileUiPatchBuilder::kPayloadValidated) {
            return std::unexpected(Error::make(
                ErrorCode::NotSupported,
                "Mobile UI stub payload has not been validated against the "
                "live game yet (real-game gate, plan B1); refusing to patch "
                "blindly"));
        }
        if (auto added = genshin::GenshinMobileUiPatchBuilder::add_operations(plan, context);
            !added) {
            return std::unexpected(added.error());
        }
    }

    // Deliberately deferred (in-process only, see the 1.0.0 plan §A6
    // "注入方式目标"): the verify detour, the UI-unhook detour and the
    // in-game fps-set hook execute code inside the game and stay unapplied
    // until real-game validation proves one is required, in which case a
    // minimal targeted stub gets added with a recorded reason.
    return plan;
}

}  // namespace hoyoflux::game
