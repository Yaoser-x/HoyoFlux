// GameAdapter tests. Signature wiring is proven against synthetic snapshots
// (no game required); version detection uses temp files; plan building uses
// directly-constructed resolved signatures.

#include "game/game_adapter.hpp"
#include "game/genshin/genshin_adapter.hpp"
#include "game/genshin/signatures.hpp"
#include "game/starrail/starrail_adapter.hpp"
#include "game/starrail/signatures.hpp"
#include "scan/signature.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace hoyoflux;
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
    profile.ui.mobile_ui = true;

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
    CHECK(plan->runtime.initial_flags ==
          (kFlagMobileUi | kFlagPowerSave));
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
