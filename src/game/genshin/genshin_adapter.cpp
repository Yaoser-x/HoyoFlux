#include "game/genshin/genshin_adapter.hpp"

#include "game/genshin/signatures.hpp"
#include "platform/win32/registry.hpp"
#include "scan/pattern_scanner.hpp"
#include "scan/signature.hpp"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace hoyoflux::game {
namespace {

constexpr uintmax_t kOldExeThresholdBytes = 0x800000;  // legacy: < 8 MB == old

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
    std::vector<ResolvedSignature> out;
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
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
