#include "game/starrail/starrail_adapter.hpp"

#include "game/starrail/signatures.hpp"
#include "platform/win32/registry.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

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
    std::vector<ResolvedSignature> out;
    for (const auto id : starrail::all_ids()) {
        auto sig = starrail::signature(id);
        if (!sig) {
            return std::unexpected(sig.error());
        }
        ResolvedSignature resolved{sig->id, 0, false};
        for (const auto& snapshot : snapshots) {
            const auto* section = snapshot.find_section(sig->section);
            if (section == nullptr) {
                continue;
            }
            auto match = scan::scan_first(sig->pattern, section->bytes);
            if (!match) {
                continue;
            }
            auto address = scan::resolve_match(*sig, *section, *match, 0);
            if (address) {
                resolved.address = *address;
                resolved.resolved = true;
                break;
            }
        }
        out.push_back(resolved);
    }
    return out;
}

}  // namespace hoyoflux::game
