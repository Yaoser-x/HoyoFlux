#include "game/starrail/starrail_adapter.hpp"

#include "game/starrail/mobile_ui.hpp"
#include "game/starrail/signatures.hpp"
#include "platform/win32/registry.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

#include <array>
#include <cstdint>

namespace hoyoflux::game {

GameId StarRailAdapter::id() const { return GameId::StarRail; }

CapabilityReport StarRailAdapter::capabilities(const GameInstall& /*install*/,
                                               const Profile& profile) const {
    // Build facts, same contract as GenshinAdapter::capabilities: anything
    // Unsupported stops a launch that requests it (validate_profile), so no
    // configured feature can degrade into a silent no-op.
    CapabilityReport report;
    auto add = [&](Capability capability, CapabilityStatus status,
                   std::string reason) {
        report.entries.push_back({capability, status, std::move(reason)});
    };

    const bool drive_render = profile.render.resolution.has_value();
    const auto& fullscreen = profile.render.fullscreen;

    add(Capability::FpsUnlock, CapabilityStatus::Supported,
        "fps variable write + mov flip (starrail.fps / starrail.fpsmovflip)");

    add(Capability::DynamicFps, CapabilityStatus::Unsupported,
        "in-game fps changes (hotkeys) are not implemented in this build");

    add(Capability::CustomResolution,
        drive_render ? CapabilityStatus::Supported : CapabilityStatus::NotRequired,
        "-screen-width/-screen-height launch arguments (real-machine gate "
        "pending: plan F1)");

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
            "borderless fullscreen cannot be set via Star Rail launch "
            "arguments; use \"windowed\"/\"exclusive\" or set it in-game";
    }
    add(Capability::FullscreenMode, fullscreen_status, std::move(fullscreen_reason));

    add(Capability::MonitorSelection,
        profile.render.monitor.has_value() ? CapabilityStatus::Unsupported
                                           : CapabilityStatus::NotRequired,
        "monitor selection has no verified mechanism for Star Rail in this "
        "build; remove \"monitor\" from the profile");

    // F5: per-game builder exists; payload gated off until validated on the
    // live game (plan B1).
    add(Capability::MobileUi, CapabilityStatus::Unsupported,
        "Mobile UI is not implemented for Star Rail in this build: the "
        "builder exists but its payload is unvalidated (plan B1)");

    add(Capability::CustomDpi, CapabilityStatus::Unsupported,
        "custom DPI has no mechanism for Star Rail in this build");

    add(Capability::PowerSave,
        profile.runtime.power_save == PowerSavePolicy::Enabled
            ? CapabilityStatus::Unsupported
            : CapabilityStatus::NotRequired,
        "power-save throttling is not implemented in this build (plan F6); "
        "launching with power_save enabled would silently do nothing");

    const bool guard_needed =
        drive_render && profile.render.persistence == ResolutionPersistence::Session;
    add(Capability::PersistentStateGuard,
        guard_needed ? CapabilityStatus::Supported : CapabilityStatus::NotRequired,
        "snapshot + RegNotifyChangeKeyValue guard + final restore of the "
        "game's Screenmanager values (real-machine gate pending)");

    return report;
}

Result<GameInstall> StarRailAdapter::locate_installation(Region region) const {
    auto paths = win32::read_launcher_paths();
    if (!paths) {
        return std::unexpected(paths.error());
    }

    const bool try_cn = region != Region::Global;
    const bool try_global = region != Region::Cn;

    if (try_cn && paths->starrail_cn.has_value()) {
        return GameInstall{GameId::StarRail, true, *paths->starrail_cn};
    }
    if (try_global && paths->starrail_global.has_value()) {
        return GameInstall{GameId::StarRail, false, *paths->starrail_global};
    }
    return std::unexpected(Error::make(
        ErrorCode::ProcessNotFound,
        "Honkai: Star Rail installation not found (CN or Global launcher registry)"));
}

Result<GameLaunchPlan> StarRailAdapter::build_launch_plan(
    const GameInstall& install, const LaunchRequest& request) const {
    auto render_args = build_render_arguments(request.profile.render);
    if (!render_args) {
        return std::unexpected(render_args.error());
    }
    auto arguments = merge_passthrough(*render_args, request.game_args,
                                       to_string(GameId::StarRail));
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

Result<bool> StarRailAdapter::is_old_version(const GameInstall& /*install*/) const {
    // Star Rail has no old/new split in the legacy tool; single layout.
    return false;
}

std::vector<std::wstring> StarRailAdapter::persistent_state_roots() const {
    // See GenshinAdapter::persistent_state_roots for the experiment that
    // validates this list.
    return {L"Software\\miHoYo\\崩坏：星穹铁道",
            L"Software\\Cognosphere\\Star Rail"};
}

Result<ModuleRequirements> StarRailAdapter::module_requirements(
    const GameInstall& /*install*/, const Profile& profile) const {
    ModuleRequirements requirements;
    requirements.modules.push_back(
        {"", {starrail::kTextSection.data()}, /*inject=*/false});
    if (profile.ui.mobile_ui) {
        // Mobile-UI set function lives in GameAssembly.dll's il2cpp, which the
        // legacy loaded into the suspended process.
        requirements.modules.push_back(
            {"GameAssembly.dll", {starrail::kIl2CppSection.data()}, /*inject=*/true});
    }
    return requirements;
}

Result<std::vector<ResolvedSignature>> StarRailAdapter::resolve_signatures(
    const std::vector<scan::ModuleSnapshot>& snapshots) const {
    std::vector<scan::Signature> signatures;
    signatures.reserve(starrail::all_ids().size());
    for (const auto id : starrail::all_ids()) {
        auto sig = starrail::signature(id);
        if (!sig) {
            return std::unexpected(sig.error());
        }
        signatures.push_back(std::move(*sig));
    }
    return resolve_all_signatures(signatures, snapshots);
}

Result<PatchPlan> StarRailAdapter::build_patch_plan(const PatchContext& context) const {
    PatchPlan plan;

    // FPS: write the profile value straight into the game's fps variable.
    const ResolvedSignature* fps = find_resolved(context.resolved, "starrail.fps");
    if (fps == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "the starrail.fps signature did not resolve; game version likely "
            "unsupported (run `hoyoflux doctor`)"));
    }
    plan.operations.push_back(
        PatchOperation::write_u32(fps->fields[0], context.profile.runtime.fps));

    // Mov flip: when the game's fps-write instruction targets the same
    // variable, flip `mov [var], ecx` into `mov ecx, [var]`
    // (main.cpp:2225-2231) so the game can never lower our value again.
    if (const ResolvedSignature* flip =
            find_resolved(context.resolved, "starrail.fpsmovflip")) {
        if (flip->fields[0] == fps->fields[0]) {
            const std::array<std::byte, 1> flip_opcode{std::byte{0x8B}};
            plan.operations.push_back(
                PatchOperation::write_bytes(flip->fields[1], flip_opcode));
        }
    }

    // No RemoteState allocation: nothing here references it. A future
    // resident component (dynamic FPS) re-writes the game variable via the
    // same address; the mov flip keeps our value authoritative.

    // Mobile UI (F5): same real-game gate as Genshin - the mechanism exists
    // (starrail::StarRailMobileUiPatchBuilder) but the payload is unvalidated.
    if (context.profile.ui.mobile_ui) {
        if (!starrail::StarRailMobileUiPatchBuilder::kPayloadValidated) {
            return std::unexpected(Error::make(
                ErrorCode::NotSupported,
                "Mobile UI stub payload has not been validated against the "
                "live game yet (real-game gate, plan B1); refusing to patch "
                "blindly"));
        }
        if (auto added = starrail::StarRailMobileUiPatchBuilder::add_operations(plan, context);
            !added) {
            return std::unexpected(added.error());
        }
    }
    return plan;
}

}  // namespace hoyoflux::game
