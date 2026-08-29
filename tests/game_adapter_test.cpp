// GameAdapter tests. Signature wiring is proven against synthetic snapshots
// (no game required); version detection uses temp files.

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
// `ids`, each placed at its own offset so all of them match. The first
// resolver's disp field is filled with a fixed value.
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

    size_t cursor = 0x40;
    for (const auto id : ids) {
        auto sig = make_sig(id);
        if (sig.id.empty()) {
            continue;
        }
        const size_t pattern_len = sig.pattern.length();
        if (cursor + pattern_len > copy.bytes.size()) {
            copy.bytes.resize(cursor + pattern_len + 16, std::byte{0xCC});
        }
        for (size_t i = 0; i < pattern_len; ++i) {
            if (sig.pattern.mask[i]) {
                copy.bytes[cursor + i] = sig.pattern.bytes[i];
            }
        }
        const auto& resolver = sig.resolvers.front();
        if (resolver.strategy != scan::ResolveStrategy::Direct) {
            const int32_t disp = 0x40;
            std::memcpy(copy.bytes.data() + cursor + resolver.disp_offset, &disp,
                        sizeof(disp));
        }
        cursor += pattern_len + 8;
    }
    snap.sections.push_back(std::move(copy));
    return snap;
}

std::vector<std::string_view> genshin_ids_in(std::string_view section) {
    std::vector<std::string_view> ids;
    for (const auto id : genshin::all_ids()) {
        auto sig = genshin::signature(id);
        if (sig && sig->section == section) {
            ids.push_back(sig->id);
        }
    }
    return ids;
}

std::vector<std::string_view> starrail_ids_in(std::string_view section) {
    std::vector<std::string_view> ids;
    for (const auto id : starrail::all_ids()) {
        auto sig = starrail::signature(id);
        if (sig && sig->section == section) {
            ids.push_back(sig->id);
        }
    }
    return ids;
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
    const auto ids = genshin_ids_in(".text");
    REQUIRE_FALSE(ids.empty());

    const auto snap = build_snapshot(".text", ids, genshin_sig_by_id);
    auto resolved = adapter.resolve_signatures({snap});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        if (std::find(ids.begin(), ids.end(), rs.id) != ids.end()) {
            INFO("expected resolved: " << rs.id);
            CHECK(rs.resolved);
            CHECK(rs.address != 0);
        }
    }
}

TEST_CASE("genshin adapter resolves every il2cpp signature", "[game][genshin]") {
    auto adapter = hoyoflux::game::GenshinAdapter{};
    const auto ids = genshin_ids_in("il2cpp");
    REQUIRE_FALSE(ids.empty());
    const auto snap = build_snapshot("il2cpp", ids, genshin_sig_by_id);
    auto resolved = adapter.resolve_signatures({snap});
    REQUIRE(resolved.has_value());
    for (const auto& rs : *resolved) {
        if (std::find(ids.begin(), ids.end(), rs.id) != ids.end()) {
            INFO("expected resolved: " << rs.id);
            CHECK(rs.resolved);
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
    const auto text_ids = starrail_ids_in(".text");
    const auto il2cpp_ids = starrail_ids_in("il2cpp");
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
