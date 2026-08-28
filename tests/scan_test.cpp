// scan component tests. These exercise the real ReadProcessMemory path against
// the test process's own image, so no game is required.

#include "scan/module_snapshot.hpp"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace hoyoflux;
namespace scan = hoyoflux::scan;

namespace {

// A handle to our own process with VM_READ so snapshot reads work.
hoyoflux::win32::UniqueHandle self_read_handle() {
    HANDLE raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                             FALSE, GetCurrentProcessId());
    return hoyoflux::win32::UniqueHandle(raw);
}

}  // namespace

TEST_CASE("remote_module_base matches our own base", "[scan][snapshot]") {
    auto handle = self_read_handle();
    REQUIRE(handle);
    auto base = scan::remote_module_base(handle);
    REQUIRE(base.has_value());
    CHECK(*base == reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}

TEST_CASE("snapshot own .text section", "[scan][snapshot]") {
    auto handle = self_read_handle();
    REQUIRE(handle);
    auto base = scan::remote_module_base(handle);
    REQUIRE(base.has_value());

    const std::string_view names[] = {".text"};
    auto snap = scan::snapshot_module(handle, *base, names);
    REQUIRE(snap.has_value());
    CHECK(snap->module_base == *base);

    const auto* text = snap->find_section(".text");
    REQUIRE(text != nullptr);
    CHECK_FALSE(text->bytes.empty());
    CHECK(text->remote_address == *base + text->rva);
}

TEST_CASE("snapshot skips absent sections", "[scan][snapshot]") {
    auto handle = self_read_handle();
    REQUIRE(handle);
    auto base = scan::remote_module_base(handle);
    REQUIRE(base.has_value());

    const std::string_view names[] = {".text", ".no-such-section-xyz"};
    auto snap = scan::snapshot_module(handle, *base, names);
    REQUIRE(snap.has_value());
    CHECK(snap->find_section(".text") != nullptr);
    CHECK(snap->find_section(".no-such-section-xyz") == nullptr);
}

TEST_CASE("snapshot of a bogus base fails cleanly", "[scan][snapshot]") {
    auto handle = self_read_handle();
    REQUIRE(handle);
    const std::string_view names[] = {".text"};
    auto snap = scan::snapshot_module(handle, 0x1000, names);
    CHECK_FALSE(snap.has_value());
    CHECK(snap.error().code == ErrorCode::ReadProcessMemoryFailed);
}

TEST_CASE("local_to_remote maps offsets", "[scan][snapshot]") {
    auto handle = self_read_handle();
    REQUIRE(handle);
    auto base = scan::remote_module_base(handle);
    REQUIRE(base.has_value());
    const std::string_view names[] = {".text"};
    auto snap = scan::snapshot_module(handle, *base, names);
    REQUIRE(snap.has_value());
    const auto* text = snap->find_section(".text");
    REQUIRE(text != nullptr);
    const uintptr_t remote = snap->local_to_remote(*text, 16);
    CHECK(remote == text->remote_address + 16);
}
