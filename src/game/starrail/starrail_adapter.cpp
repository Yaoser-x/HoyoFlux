#include "game/starrail/starrail_adapter.hpp"

#include "game/starrail/signatures.hpp"
#include "platform/win32/registry.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

#include <array>
#include <cstdint>

namespace hoyoflux::game {

GameId StarRailAdapter::id() const { return GameId::StarRail; }

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

Result<bool> StarRailAdapter::is_old_version(const GameInstall& /*install*/) const {
    // Star Rail has no old/new split in the legacy tool; single layout.
    return false;
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
    return plan;
}

}  // namespace hoyoflux::game
