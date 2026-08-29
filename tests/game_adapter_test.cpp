// GameAdapter tests. Signature wiring is proven against synthetic snapshots
// (no game required); version detection uses temp files; plan building uses
// directly-constructed resolved signatures.

#include "game/game_adapter.hpp"
#include "game/genshin/genshin_adapter.hpp"
#include "game/genshin/mobile_ui.hpp"
#include "game/genshin/signatures.hpp"
#include "game/starrail/starrail_adapter.hpp"
#include "game/starrail/mobile_ui.hpp"
#include "game/starrail/signatures.hpp"
#include "scan/signature.hpp"
#include "platform/win32/registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace hoyoflux;
namespace w32 = hoyoflux::win32;
namespace genshin = hoyoflux::game::genshin;
namespace starrail = hoyoflux::game::starrail;
namespace scan = hoyoflux::scan;

namespace {

using hoyoflux::game::ResolvedSignature;
using hoyoflux::PatchOperationKind;
using hoyoflux::PatchTargetSymbol;

using SigFactory = std::function<scan::Signature(std::string_view id)>;

// Return a signature by its string id (empty signature when unknown).
scan::Signature genshin_sig_by_id(std::string_view id) {
    for (const auto gid : genshin::all_ids()) {
        auto s = genshin::signature(gid);
        if (s && s->id == id) {
            return *s;
        }
    }
    return {};
}

scan::Signature starrail_sig_by_id(std::string_view id) {
    for (const auto sid : starrail::all_ids()) {
        auto s = starrail::signature(sid);
        if (s && s->id == id) {
            return *s;
        }
    }
    return {};
}

// Build a ModuleSnapshot whose `section` copy contains every signature in
// `ids`, each placed at its own offset so all of them match. Resolver read
// fields (rip-relative displacements, raw int32 loads) get 0x40 written into
// every wildcard byte they touch, so every resolver of every signature
// resolves to a non-zero address.
scan::ModuleSnapshot build_snapshot(std::string_view section,
                                    const std::vector<std::string_view>& ids,
                                    const SigFactory& make_sig) {
    scan::ModuleSnapshot snap;
    snap.module_base = 0x140000000;
    scan::SectionCopy copy;
    copy.name = section;
    copy.rva = 0x1000;
    copy.remote_address = snap.module_base + copy.rva;
    copy.bytes.assign(0x1000, std::byte{0xCC});

    const auto write_resolver_data = [&](const scan::Signature& sig, size_t cursor) {
        for (const auto& resolver : sig.resolvers) {
            if (resolver.strategy != scan::ResolveStrategy::RipRelative &&
                resolver.strategy != scan::ResolveStrategy::RawInt32At) {
                continue;  // FieldDisp / Direct read nothing
            }
            for (size_t i = 0; i < 4; ++i) {
                const size_t pos = cursor + static_cast<size_t>(resolver.disp_offset) + i;
                if (pos >= copy.bytes.size()) {
                    continue;
                }
                // Beyond the pattern, or inside it only at wildcards - the
                // fixed pattern bytes must stay untouched for the match.
                const bool in_pattern = pos < cursor + sig.pattern.length();
                if (!in_pattern || !sig.pattern.mask[pos - cursor]) {
                    copy.bytes[pos] = std::byte{0x40};
                }
            }
        }
    };

    size_t cursor = 0x40;
    for (const auto id : ids) {
        auto sig = make_sig(id);
        if (sig.id.empty()) {
            continue;
        }
        const size_t pattern_len = sig.pattern.length();
        if (cursor + pattern_len + 0x40 > copy.bytes.size()) {
            copy.bytes.resize(cursor + pattern_len + 0x80, std::byte{0xCC});
        }
        for (size_t i = 0; i < pattern_len; ++i) {
            if (sig.pattern.mask[i]) {
                copy.bytes[cursor + i] = sig.pattern.bytes[i];
            }
        }
        write_resolver_data(sig, cursor);
        cursor += pattern_len + 0x40;  // room for resolvers reading past the end
    }
    snap.sections.push_back(std::move(copy));
    return snap;
}

// A temp file of the given byte size; removed at scope end.
class TempFile {
public:
    explicit TempFile(uintmax_t size) {
        path = std::filesystem::temp_directory_path() /
               ("hoyoflux_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                ".bin");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        std::vector<char> block(4096, 'x');
        uintmax_t written = 0;
        while (written < size) {
            const auto chunk = std::min<uintmax_t>(4096, size - written);
            out.write(block.data(), static_cast<std::streamsize>(chunk));
            written += chunk;
        }
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

}  // namespace

TEST_CASE("adapter factory returns correct ids", "[game][adapter]") {
    auto g = hoyoflux::game::make_adapter(GameId::Genshin);
    REQUIRE(g != nullptr);
    CHECK(g->id() == GameId::Genshin);

    auto s = hoyoflux::game::make_adapter(GameId::StarRail);
    REQUIRE(s != nullptr);
    CHECK(s->id() == GameId::StarRail);
}

TEST_CASE("genshin adapter resolves every .text signature", "[game][genshin]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    std::vector<std::string_view> ids;
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
        if (sig && sig->section == ".text") {
            ids.push_back(sig->id);
        }
    }
    REQUIRE_FALSE(ids.empty());

    const auto snap = build_snapshot(".text", ids, genshin_sig_by_id);
    auto resolved = adapter.resolve_signatures({snap});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        if (std::find(ids.begin(), ids.end(), rs.id) != ids.end()) {
            INFO("expected resolved: " << rs.id);
            CHECK(rs.resolved);
            REQUIRE(rs.fields.size() == 1);
            CHECK(rs.fields[0] != 0);
        }
    }
}

TEST_CASE("genshin adapter resolves every il2cpp signature", "[game][genshin]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    std::vector<std::string_view> ids;
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
        if (sig && sig->section == "il2cpp") {
            ids.push_back(sig->id);
        }
    }
    REQUIRE_FALSE(ids.empty());
    const auto snap = build_snapshot("il2cpp", ids, genshin_sig_by_id);
    auto resolved = adapter.resolve_signatures({snap});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        if (std::find(ids.begin(), ids.end(), rs.id) != ids.end()) {
            INFO("expected resolved: " << rs.id);
            CHECK(rs.resolved);
            // MobileUi signatures carry 4 resolvers; check they all resolved.
            for (const auto field : rs.fields) {
                CHECK(field != 0);
            }
        }
    }
}

TEST_CASE("genshin adapter leaves missing signatures unresolved", "[game][genshin]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    auto resolved = adapter.resolve_signatures({});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        CHECK_FALSE(rs.resolved);
    }
}

TEST_CASE("starrail adapter resolves fps and uiset", "[game][starrail]") {
    auto adapter = hoyoflux::game::StarRailAdapter{};
    auto ids_in = [](std::string_view section) {
        std::vector<std::string_view> out;
        for (const auto id : starrail::all_ids()) {
            auto sig = starrail::signature(id);
            if (sig && sig->section == section) {
                out.push_back(sig->id);
            }
        }
        return out;
    };
    const auto text_ids = ids_in(".text");
    const auto il2cpp_ids = ids_in("il2cpp");
    REQUIRE_FALSE(text_ids.empty());
    REQUIRE_FALSE(il2cpp_ids.empty());

    const auto text_snap = build_snapshot(".text", text_ids, starrail_sig_by_id);
    const auto il2cpp_snap = build_snapshot("il2cpp", il2cpp_ids, starrail_sig_by_id);
    auto resolved = adapter.resolve_signatures({text_snap, il2cpp_snap});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        INFO("starrail id: " << rs.id);
        CHECK(rs.resolved);
    }
}

TEST_CASE("genshin version split by exe size", "[game][genshin]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    {
        TempFile small(0x100000);  // 1 MB < 8 MB -> old
        GameInstall install{GameId::Genshin, true, small.path};
        auto old_version = adapter.is_old_version(install);
        REQUIRE(old_version.has_value());
        CHECK(*old_version);
        auto reqs = adapter.module_requirements(install, {});
        REQUIRE(reqs.has_value());
        REQUIRE(reqs->modules.size() == 2);
        CHECK(reqs->modules[0].module == "UnityPlayer.dll");
        CHECK(reqs->modules[1].module == "UserAssembly.dll");
        CHECK(reqs->modules[0].inject_if_missing);
        CHECK(reqs->modules[1].inject_if_missing);
    }
    {
        TempFile big(0x1000000);  // 16 MB -> new
        GameInstall install{GameId::Genshin, true, big.path};
        auto old_version = adapter.is_old_version(install);
        REQUIRE(old_version.has_value());
        CHECK_FALSE(*old_version);
        auto reqs = adapter.module_requirements(install, {});
        REQUIRE(reqs.has_value());
        REQUIRE(reqs->modules.size() == 1);
        CHECK(reqs->modules[0].module.empty());  // main module
        CHECK(reqs->modules[0].sections.size() == 2);
    }
}

TEST_CASE("starrail adds GameAssembly only for mobile UI", "[game][starrail]") {
    auto adapter = hoyoflux::game::StarRailAdapter{};
    GameInstall install{GameId::StarRail, true, L"D:\\games\\StarRail.exe"};

    Profile desktop;
    desktop.id = "desktop";
    auto no_mobile = adapter.module_requirements(install, desktop);
    REQUIRE(no_mobile.has_value());
    CHECK(no_mobile->modules.size() == 1);

    Profile mobile;
    mobile.id = "ipad";
    mobile.ui.mobile_ui = true;
    auto with_mobile = adapter.module_requirements(install, mobile);
    REQUIRE(with_mobile.has_value());
    REQUIRE(with_mobile->modules.size() == 2);
    CHECK(with_mobile->modules[1].module == "GameAssembly.dll");
    CHECK(with_mobile->modules[1].inject_if_missing);
}

TEST_CASE("locate_installation either succeeds or reports not found",
          "[game][adapter]") {
    auto adapter = hoyoflux::game::make_adapter(GameId::Genshin);
    REQUIRE(adapter);
    auto install = adapter->locate_installation(hoyoflux::game::Region::Auto);
    if (install.has_value()) {
        CHECK_FALSE(install->exe_path.empty());
    } else {
        CHECK(install.error().code == ErrorCode::ProcessNotFound);
    }
}

// ---------------------------------------------------------------------------
// build_patch_plan
// ---------------------------------------------------------------------------

namespace {

ResolvedSignature resolved(std::string_view id, std::vector<uintptr_t> fields) {
    return ResolvedSignature{id, true, std::move(fields)};
}

ResolvedSignature unresolved(std::string_view id) {
    return ResolvedSignature{id, false, {}};
}

}  // namespace

TEST_CASE("genshin plan redirects fps at RemoteState and honors dpi",
          "[game][genshin][plan]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};

    std::vector<ResolvedSignature> signatures;
    // Only the 3.7-5.3-generation signature resolved - the priority order
    // must pick it even though 5.5 is higher priority but unresolved.
    signatures.push_back(unresolved("genshin.fps.5.5"));
    signatures.push_back(resolved("genshin.fps.3.7-5.3", {0x7FF612345678}));

    Profile profile;
    profile.runtime.fps = 165;
    profile.runtime.power_save = PowerSavePolicy::Enabled;
    // mobile_ui stays off here: since F5 the mobile-UI gate refuses
    // unvalidated payloads, and that path has its own test below.

    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    auto plan = adapter.build_patch_plan(ctx);
    REQUIRE(plan.has_value());
    REQUIRE(plan->operations.size() == 1);

    const auto& redirect = plan->operations[0];
    CHECK(redirect.kind == PatchOperationKind::RedirectRelative);
    CHECK(redirect.address == 0x7FF612345678);
    CHECK(redirect.target_symbol == PatchTargetSymbol::RemoteStateFps);

    CHECK(plan->runtime.near_address == 0x7FF600000000);
    CHECK(plan->runtime.initial_fps == 165);
    // mobile_ui is off (F5 gate) - only the power-save flag is requested.
    CHECK(plan->runtime.initial_flags == kFlagPowerSave);
}

TEST_CASE("genshin plan fails without any fps signature", "[game][genshin][plan]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("genshin.unitywndclass", {0x1000}));

    Profile profile;
    game::PatchContext ctx{signatures, profile, 0x1000, false};
    auto plan = adapter.build_patch_plan(ctx);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::SignatureNotFound);
}

TEST_CASE("genshin plan emits the dpi prologue when the profile asks for it",
          "[game][genshin][plan]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};

    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("genshin.fps.5.5", {0x7FF610000010}));
    signatures.push_back(resolved("genshin.dpi", {0x7FF61000A000}));

    Profile profile;
    profile.runtime.fps = 60;
    profile.ui.dpi_scale = 1.5f;

    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    auto plan = adapter.build_patch_plan(ctx);
    REQUIRE(plan.has_value());
    REQUIRE(plan->operations.size() == 2);

    const auto& dpi = plan->operations[1];
    CHECK(dpi.kind == PatchOperationKind::WriteBytes);
    CHECK(dpi.address == 0x7FF61000A000);
    REQUIRE(dpi.data.size() == 16);
    CHECK(dpi.data[0] == std::byte{0xB8});  // mov eax, imm32
    float written = 0.0f;
    std::memcpy(&written, dpi.data.data() + 1, sizeof(written));
    CHECK(written == 1.5f * 96.0f);
    CHECK(dpi.data[5] == std::byte{0x66});   // movd xmm0, eax
    CHECK(dpi.data[9] == std::byte{0xC3});   // ret
    CHECK(dpi.data[10] == std::byte{0xCC});  // int3 padding

    // No dpi_scale -> no dpi operation.
    profile.ui.dpi_scale.reset();
    auto without = adapter.build_patch_plan(
        game::PatchContext{signatures, profile, 0x7FF600000000, false});
    REQUIRE(without.has_value());
    CHECK(without->operations.size() == 1);
}

TEST_CASE("genshin plan fails when dpi requested but signature missing",
          "[game][genshin][plan]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("genshin.fps.5.5", {0x7FF610000010}));

    Profile profile;
    profile.ui.dpi_scale = 1.25f;
    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    auto plan = adapter.build_patch_plan(ctx);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::SignatureNotFound);
}

TEST_CASE("starrail plan writes fps and flips the mov when targets agree",
          "[game][starrail][plan]") {
    auto adapter = hoyoflux::game::StarRailAdapter{};

    Profile profile;
    profile.runtime.fps = 144;
    const uintptr_t fps_var = 0x7FF630001000;

    SECTION("flip applies when the write site targets the fps variable") {
        std::vector<ResolvedSignature> signatures;
        signatures.push_back(resolved("starrail.fps", {fps_var}));
        // resolver 0: rip target == fps variable; resolver 1: patch site.
        signatures.push_back(resolved("starrail.fpsmovflip", {fps_var, 0x7FF610000041}));

        auto plan = adapter.build_patch_plan(
            game::PatchContext{signatures, profile, 0x7FF600000000, false});
        REQUIRE(plan.has_value());
        REQUIRE(plan->operations.size() == 2);

        const auto& write = plan->operations[0];
        CHECK(write.kind == PatchOperationKind::WriteBytes);
        CHECK(write.address == fps_var);
        REQUIRE(write.data.size() == 4);
        uint32_t value = 0;
        std::memcpy(&value, write.data.data(), sizeof(value));
        CHECK(value == 144);

        const auto& flip = plan->operations[1];
        CHECK(flip.kind == PatchOperationKind::WriteBytes);
        CHECK(flip.address == 0x7FF610000041);
        REQUIRE(flip.data.size() == 1);
        CHECK(flip.data[0] == std::byte{0x8B});  // 89 -> 8B

        CHECK(plan->runtime.base == 0);  // no RemoteState requested
    }

    SECTION("flip skipped when the write site targets something else") {
        std::vector<ResolvedSignature> signatures;
        signatures.push_back(resolved("starrail.fps", {fps_var}));
        signatures.push_back(
            resolved("starrail.fpsmovflip", {fps_var + 0x40, 0x7FF610000041}));

        auto plan = adapter.build_patch_plan(
            game::PatchContext{signatures, profile, 0x7FF600000000, false});
        REQUIRE(plan.has_value());
        REQUIRE(plan->operations.size() == 1);  // fps write only
    }
}

TEST_CASE("starrail plan fails without the fps signature", "[game][starrail][plan]") {
    auto adapter = hoyoflux::game::StarRailAdapter{};
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("starrail.uiset.v1", {0x1000, 0x2000}));

    Profile profile;
    auto plan = adapter.build_patch_plan(
        game::PatchContext{signatures, profile, 0x1000, false});
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::SignatureNotFound);
}

// ---------------------------------------------------------------------------
// F0 capability contract
// ---------------------------------------------------------------------------

TEST_CASE("every capability is reported by each adapter", "[game][capability]") {
    const GameInstall genshin_install{GameId::Genshin, true, "YuanShen.exe"};
    const GameInstall starrail_install{GameId::StarRail, true, "StarRail.exe"};
    const Profile profile;

    hoyoflux::game::GenshinAdapter genshin;
    hoyoflux::game::StarRailAdapter starrail;
    const std::pair<const hoyoflux::game::GameAdapter*, GameInstall> adapters[] = {
        {&genshin, genshin_install},
        {&starrail, starrail_install},
    };

    for (const auto& [adapter, install] : adapters) {
        const auto report = adapter->capabilities(install, profile);
        CAPTURE(to_string(adapter->id()));
        for (int i = 0; i <= static_cast<int>(Capability::PersistentStateGuard);
             ++i) {
            const auto capability = static_cast<Capability>(i);
            INFO("capability " << to_string(capability));
            const auto* entry = report.find(capability);
            REQUIRE(entry != nullptr);
            // A status that blocks a launch must always carry its reason.
            REQUIRE((entry->status != CapabilityStatus::Unsupported ||
                     !entry->reason.empty()));
        }
    }
}

TEST_CASE("desktop-only profile validates against both adapters",
          "[game][capability]") {
    const GameInstall genshin_install{GameId::Genshin, true, "YuanShen.exe"};
    const GameInstall starrail_install{GameId::StarRail, true, "StarRail.exe"};
    const Profile profile;  // fps only; no render/ui extras

    hoyoflux::game::GenshinAdapter genshin;
    hoyoflux::game::StarRailAdapter starrail;
    REQUIRE(game::validate_profile(profile,
                                   genshin.capabilities(genshin_install, profile))
                .has_value());
    REQUIRE(game::validate_profile(profile,
                                   starrail.capabilities(starrail_install, profile))
                .has_value());
}

TEST_CASE("unsupported features stop validation with a reason",
          "[game][capability]") {
    const GameInstall genshin_install{GameId::Genshin, true, "YuanShen.exe"};
    hoyoflux::game::GenshinAdapter adapter;

    SECTION("mobile_ui is the plan's canonical example") {
        Profile profile;
        profile.ui.mobile_ui = true;
        auto valid = game::validate_profile(
            profile, adapter.capabilities(genshin_install, profile));
        REQUIRE_FALSE(valid.has_value());
        CHECK(valid.error().code == ErrorCode::NotSupported);
        CHECK(valid.error().message.find(
                  "Mobile UI is not implemented for Genshin") !=
              std::string::npos);
    }

    SECTION("monitor selection has no verified mechanism") {
        Profile profile;
        profile.render.monitor = 1;
        auto valid = game::validate_profile(
            profile, adapter.capabilities(genshin_install, profile));
        REQUIRE_FALSE(valid.has_value());
        CHECK(valid.error().code == ErrorCode::NotSupported);
        CHECK(valid.error().message.find("monitor selection") !=
              std::string::npos);
    }

    SECTION("power_save enabled would be a silent no-op today") {
        Profile profile;
        profile.runtime.power_save = PowerSavePolicy::Enabled;
        auto valid = game::validate_profile(
            profile, adapter.capabilities(genshin_install, profile));
        REQUIRE_FALSE(valid.has_value());
        CHECK(valid.error().code == ErrorCode::NotSupported);
        CHECK(valid.error().message.find("power-save") != std::string::npos);
    }

    SECTION("session-scoped resolution rides on the persistent-state guard") {
        // Since F2/F3 the guard is real in this build: snapshot + watch +
        // final restore. A session-scoped render therefore validates.
        Profile profile;
        profile.render.resolution = Resolution{1080, 1920};
        profile.render.fullscreen = FullscreenMode::Windowed;
        auto valid = game::validate_profile(
            profile, adapter.capabilities(genshin_install, profile));
        REQUIRE(valid.has_value());
    }

    SECTION("borderless cannot be promised via launch arguments") {
        Profile profile;
        profile.render.resolution = Resolution{2560, 1440};
        profile.render.fullscreen = FullscreenMode::Borderless;
        profile.render.persistence = ResolutionPersistence::Persistent;
        auto valid = game::validate_profile(
            profile, adapter.capabilities(genshin_install, profile));
        REQUIRE_FALSE(valid.has_value());
        CHECK(valid.error().message.find("borderless") != std::string::npos);
    }

    SECTION("star rail has no dpi mechanism") {
        const GameInstall starrail_install{GameId::StarRail, true,
                                           "StarRail.exe"};
        hoyoflux::game::StarRailAdapter starrail;
        Profile profile;
        profile.ui.dpi_scale = 1.5f;
        auto valid = game::validate_profile(
            profile, starrail.capabilities(starrail_install, profile));
        REQUIRE_FALSE(valid.has_value());
        CHECK(valid.error().message.find("Star Rail") != std::string::npos);
    }
}

TEST_CASE("genshin custom dpi validates when supported", "[game][capability]") {
    const GameInstall genshin_install{GameId::Genshin, true, "YuanShen.exe"};
    hoyoflux::game::GenshinAdapter adapter;
    Profile profile;
    profile.ui.dpi_scale = 2.0f;
    profile.render.persistence = ResolutionPersistence::Persistent;
    auto valid = game::validate_profile(
        profile, adapter.capabilities(genshin_install, profile));
    REQUIRE(valid.has_value());
}

// ---------------------------------------------------------------------------
// F2 persistent display state
// ---------------------------------------------------------------------------

TEST_CASE("persistent roots snapshot/restore protects Screenmanager values",
          "[game][persistent-state]") {
    const std::wstring root =
        L"Software\\HoyoFluxTest\\" + std::to_wstring(GetCurrentProcessId()) +
        L"\\snap";
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());

    const std::array<uint32_t, 1> width{2266};
    const std::array<uint32_t, 1> height{1488};
    const std::array<uint32_t, 1> other{1};
    const w32::RegistryValue stored[] = {
        {L"Screenmanager Resolution Width H907608738", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(width.data()),
             reinterpret_cast<const std::byte*>(width.data() + 1))},
        {L"Screenmanager Resolution Height H907608738", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(height.data()),
             reinterpret_cast<const std::byte*>(height.data() + 1))},
        {L"UnrelatedSetting", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(other.data()),
             reinterpret_cast<const std::byte*>(other.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, stored).has_value());

    // A missing root alongside the live one is skipped silently.
    const std::wstring missing = root + L"\\absent";

    auto snapshot = game::snapshot_persistent_roots({root, missing});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->sets.size() == 1);
    REQUIRE(snapshot->sets[0].settings.size() == 2);
    // Unrelated values are not captured; check by name, not position.
    std::vector<std::wstring> names;
    for (const auto& setting : snapshot->sets[0].settings) {
        names.push_back(setting.name);
    }
    REQUIRE(names.size() == 2);
    CHECK(std::find(names.begin(), names.end(),
                    L"Screenmanager Resolution Width H907608738") != names.end());
    CHECK(std::find(names.begin(), names.end(),
                    L"Screenmanager Resolution Height H907608738") != names.end());
    // ...and the decoded diagnostic view sees the resolution.
    REQUIRE(snapshot->resolution.has_value());
    CHECK(snapshot->resolution->width == 2266);
    CHECK(snapshot->resolution->height == 1488);

    // Simulate the game rewriting its persistent settings.
    const std::array<uint32_t, 1> new_width{1080};
    const std::array<uint32_t, 1> new_height{1920};
    const w32::RegistryValue game_wrote[] = {
        {L"Screenmanager Resolution Width H907608738", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(new_width.data()),
             reinterpret_cast<const std::byte*>(new_width.data() + 1))},
        {L"Screenmanager Resolution Height H907608738", REG_DWORD,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(new_height.data()),
             reinterpret_cast<const std::byte*>(new_height.data() + 1))},
    };
    REQUIRE(w32::write_registry_values(root, game_wrote).has_value());

    // Restore replays the snapshot verbatim.
    REQUIRE(game::restore_persistent_roots(*snapshot).has_value());
    auto read_back = w32::read_registry_values(root);
    REQUIRE(read_back.has_value());
    for (const auto& value : *read_back) {
        if (value.name == L"Screenmanager Resolution Width H907608738") {
            CHECK(*reinterpret_cast<const uint32_t*>(value.data.data()) == 2266);
        }
        if (value.name == L"Screenmanager Resolution Height H907608738") {
            CHECK(*reinterpret_cast<const uint32_t*>(value.data.data()) == 1488);
        }
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
}

TEST_CASE("genshin persistent state snapshot reports absence honestly",
          "[game][persistent-state]") {
    // On a machine where the game has run, the snapshot succeeds; where it
    // has not, it must be an explicit error - never an empty "success".
    const GameInstall install{GameId::Genshin, true, "YuanShen.exe"};
    hoyoflux::game::GenshinAdapter adapter;
    auto state = adapter.snapshot_persistent_display_state();
    if (state.has_value()) {
        CHECK_FALSE(state->sets.empty());
    } else {
        CHECK(state.error().code == ErrorCode::NotSupported);
    }
}

// ---------------------------------------------------------------------------
// F1 render launch pipeline
// ---------------------------------------------------------------------------

TEST_CASE("mobile UI gate refuses unvalidated payloads", "[game][mobileui]") {
    hoyoflux::game::GenshinAdapter genshin;
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(
        resolved("genshin.fps.3.7-5.3", {0x7FF612345678}));
    signatures.push_back(resolved("genshin.mobileui.v1",
                                  {0x1000, 0x2000, 0x3000, 0x4000}));
    signatures.push_back(resolved("genshin.mobileui.input", {0x5000}));
    signatures.push_back(resolved("genshin.unhooktime", {0x6000}));

    Profile profile;
    profile.ui.mobile_ui = true;
    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    auto plan = genshin.build_patch_plan(ctx);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::NotSupported);
    CHECK(plan.error().message.find("not been validated") != std::string::npos);
}

TEST_CASE("mobile UI builders compose bootstrap ops from resolved signatures",
          "[game][mobileui]") {
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("genshin.mobileui.v1",
                                  {0x1000, 0x2000, 0x3000, 0x4000}));

    Profile profile;
    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    PatchPlan plan;
    REQUIRE(
        hoyoflux::game::genshin::GenshinMobileUiPatchBuilder::add_operations(
            plan, ctx)
            .has_value());
    REQUIRE(plan.operations.size() == 2);
    CHECK(plan.operations[0].kind == PatchOperationKind::InstallCodeStub);
    CHECK(plan.operations[0].data.size() > 16);
    CHECK(plan.operations[1].kind == PatchOperationKind::InvokeBootstrap);
    CHECK(plan.operations[1].stub_index == 0);

    // Missing signatures are an explicit error, never a silent skip.
    PatchPlan empty_plan;
    game::PatchContext missing{std::vector<ResolvedSignature>{}, profile,
                               0x7FF600000000, false};
    auto failed =
        hoyoflux::game::genshin::GenshinMobileUiPatchBuilder::add_operations(
            empty_plan, missing);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::SignatureNotFound);
}

TEST_CASE("mobile UI builders compose bootstrap ops from resolved signatures (starrail)",
          "[game][mobileui]") {
    std::vector<ResolvedSignature> signatures;
    signatures.push_back(resolved("starrail.uiset.v1", {0x1000}));
    signatures.push_back(resolved("starrail.uiset.v3", {0x2000}));

    Profile profile;
    game::PatchContext ctx{signatures, profile, 0x7FF600000000, false};
    PatchPlan plan;
    REQUIRE(
        hoyoflux::game::starrail::StarRailMobileUiPatchBuilder::add_operations(
            plan, ctx)
            .has_value());
    REQUIRE(plan.operations.size() == 2);
    CHECK(plan.operations[0].kind == PatchOperationKind::InstallCodeStub);

    // Two resolved setters -> two call sequences in one stub.
    const auto& stub = plan.operations[0].data;
    size_t calls = 0;
    for (size_t i = 0; i + 1 < stub.size(); ++i) {
        if (stub[i] == std::byte{0xFF} && stub[i + 1] == std::byte{0xD0}) {
            ++calls;
        }
    }
    CHECK(calls == 2);
}

TEST_CASE("build_render_arguments maps only verified mechanisms",
          "[game][launch]") {
    using hoyoflux::game::build_render_arguments;

    SECTION("no render policy -> no arguments") {
        RenderPolicy render;
        auto args = build_render_arguments(render);
        REQUIRE(args.has_value());
        CHECK(args->empty());
    }

    SECTION("resolution -> -screen-width/-screen-height") {
        RenderPolicy render;
        render.resolution = Resolution{2266, 1488};
        auto args = build_render_arguments(render);
        REQUIRE(args.has_value());
        const std::vector<std::wstring> expected = {L"-screen-width", L"2266",
                                                    L"-screen-height", L"1488"};
        CHECK(*args == expected);
    }

    SECTION("windowed -> -screen-fullscreen 0") {
        RenderPolicy render;
        render.fullscreen = FullscreenMode::Windowed;
        auto args = build_render_arguments(render);
        REQUIRE(args.has_value());
        CHECK(*args == std::vector<std::wstring>{L"-screen-fullscreen", L"0"});
    }

    SECTION("exclusive -> -screen-fullscreen 1") {
        RenderPolicy render;
        render.fullscreen = FullscreenMode::Exclusive;
        auto args = build_render_arguments(render);
        REQUIRE(args.has_value());
        CHECK(*args == std::vector<std::wstring>{L"-screen-fullscreen", L"1"});
    }

    SECTION("borderless is refused, never guessed") {
        RenderPolicy render;
        render.fullscreen = FullscreenMode::Borderless;
        auto args = build_render_arguments(render);
        REQUIRE_FALSE(args.has_value());
        CHECK(args.error().code == ErrorCode::NotSupported);
    }
}

TEST_CASE("merge_passthrough appends verbatim and rejects managed conflicts",
          "[game][launch]") {
    using hoyoflux::game::merge_passthrough;

    const std::vector<std::wstring> managed = {L"-screen-width", L"1080"};

    SECTION("unrelated passthrough is appended in order") {
        auto merged = merge_passthrough(managed, {L"-window-mode", L"exclusive"},
                                        "genshin");
        REQUIRE(merged.has_value());
        const std::vector<std::wstring> expected = {L"-screen-width", L"1080",
                                                    L"-window-mode", L"exclusive"};
        CHECK(*merged == expected);
    }

    SECTION("defining a managed field twice is an error") {
        auto merged = merge_passthrough(managed, {L"-screen-height", L"1920"},
                                        "genshin");
        REQUIRE_FALSE(merged.has_value());
        CHECK(merged.error().code == ErrorCode::InvalidArgument);
        CHECK(merged.error().message.find("screen-height") != std::string::npos);
    }

    SECTION("conflict detection is case-insensitive") {
        auto merged =
            merge_passthrough(managed, {L"-SCREEN-WIDTH", L"800"}, "genshin");
        REQUIRE_FALSE(merged.has_value());
    }
}

TEST_CASE("genshin build_launch_plan composes the full argv",
          "[game][genshin][launch]") {
    const GameInstall install{GameId::Genshin, true, "D:\\Games\\Genshin\\YuanShen.exe"};
    hoyoflux::game::GenshinAdapter adapter;

    LaunchRequest request;
    request.profile.id = "ipad";
    request.profile.render.resolution = Resolution{1080, 1920};
    request.profile.render.fullscreen = FullscreenMode::Windowed;
    request.profile.runtime.priority = ProcessPriority::High;
    request.game_args = {L"-sound", L"off"};

    auto plan = adapter.build_launch_plan(install, request);
    REQUIRE(plan.has_value());
    CHECK(plan->executable == install.exe_path);
    CHECK(plan->working_directory == install.exe_path.parent_path());
    CHECK(plan->priority == ProcessPriority::High);
    const std::vector<std::wstring> expected = {
        L"-screen-width", L"1080", L"-screen-height", L"1920",
        L"-screen-fullscreen", L"0", L"-sound", L"off"};
    CHECK(plan->arguments == expected);
}

TEST_CASE("genshin build_launch_plan rejects passthrough render conflicts",
          "[game][genshin][launch]") {
    const GameInstall install{GameId::Genshin, true, "YuanShen.exe"};
    hoyoflux::game::GenshinAdapter adapter;

    LaunchRequest request;
    request.profile.render.resolution = Resolution{1080, 1920};
    request.game_args = {L"-screen-width", L"800"};
    auto plan = adapter.build_launch_plan(install, request);
    REQUIRE_FALSE(plan.has_value());
    CHECK(plan.error().code == ErrorCode::InvalidArgument);
}
