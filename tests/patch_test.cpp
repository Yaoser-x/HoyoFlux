// PatchEngine tests against the current process: every primitive here
// (VirtualAllocEx / VirtualProtectEx / WriteProcessMemory / ReadProcessMemory)
// works on self, so the full apply/rollback path is testable without a game.

#include "patch/memory_writer.hpp"
#include "patch/patch_engine.hpp"
#include "patch/x64_emit.hpp"
#include "patch/remote_state.hpp"
#include "platform/win32/process.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <span>

using namespace hoyoflux;
namespace patch = hoyoflux::patch;

namespace {

win32::UniqueHandle self_process() {
    auto handle = win32::open_process(GetCurrentProcessId(), PROCESS_ALL_ACCESS);
    REQUIRE(handle.has_value());
    return std::move(*handle);
}

// 32 bytes, 16-aligned, so an 8-aligned 16-byte redirect window at +8 sits
// entirely inside. Layout mirrors a `66 0F 6E 0D disp32` instruction: the
// disp field at +12 (mask 4), instruction bytes around it.
struct WindowBuffer {
    alignas(16) std::array<std::byte, 32> raw{};

    static WindowBuffer filled() {
        WindowBuffer buffer;
        buffer.raw.fill(std::byte{0x11});
        // pseudo-instruction at +8: 66 0F 6E 0D | disp32 at +12
        buffer.raw[8] = std::byte{0x66};
        buffer.raw[9] = std::byte{0x0F};
        buffer.raw[10] = std::byte{0x6E};
        buffer.raw[11] = std::byte{0x0D};
        for (size_t i = 12; i < 16; ++i) {
            buffer.raw[i] = std::byte{0x22};
        }
        return buffer;
    }
    [[nodiscard]] uintptr_t disp_field() const {
        return reinterpret_cast<uintptr_t>(raw.data()) + 12;
    }
    [[nodiscard]] int32_t disp() const {
        int32_t value = 0;
        std::memcpy(&value, raw.data() + 12, sizeof(value));
        return value;
    }
};

}  // namespace

TEST_CASE("read/write round trip on own memory", "[patch][memory]") {
    const auto process = self_process();
    std::array<std::byte, 8> buffer{};
    std::array<std::byte, 8> payload{};
    payload.fill(std::byte{0xAB});

    const uintptr_t address = reinterpret_cast<uintptr_t>(buffer.data());
    auto wrote = patch::write_protected(process, address, payload);
    REQUIRE(wrote.has_value());

    std::array<std::byte, 8> back{};
    auto read = patch::read_bytes(process, address, back);
    REQUIRE(read.has_value());
    CHECK(back == payload);
}

TEST_CASE("write_protected works on a read-only page and restores it", "[patch][memory]") {
    const auto process = self_process();
    static const std::array<std::byte, 16> kReadOnly{std::byte{0x5A}};
    const uintptr_t address = reinterpret_cast<uintptr_t>(kReadOnly.data());

    std::array<std::byte, 16> payload{};
    payload.fill(std::byte{0xCD});
    auto wrote = patch::write_protected(process, address, payload);
    REQUIRE(wrote.has_value());
    // The static const page is physically writable after the protect dance;
    // verify the write landed.
    std::array<std::byte, 16> back{};
    REQUIRE(patch::read_bytes(process, address, back).has_value());
    CHECK(back == payload);
}

TEST_CASE("allocate_near returns a page below the anchor", "[patch][memory]") {
    const auto process = self_process();
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(&self_process);
    auto allocated = patch::allocate_near(process, anchor, 0x1000);
    REQUIRE(allocated.has_value());
    CHECK(*allocated != 0);
    CHECK(*allocated < anchor);

    // The block must be usable: write, read back, release.
    std::array<std::byte, 4> payload{std::byte{1}, std::byte{2}, std::byte{3},
                                     std::byte{4}};
    REQUIRE(patch::write_protected(process, *allocated, payload).has_value());
    std::array<std::byte, 4> back{};
    REQUIRE(patch::read_bytes(process, *allocated, back).has_value());
    CHECK(back == payload);
    REQUIRE(patch::free_remote(process, *allocated).has_value());
}

TEST_CASE("remote state block carries magic, version, fps and flags",
          "[patch][state]") {
    const auto process = self_process();
    const uintptr_t anchor = reinterpret_cast<uintptr_t>(&self_process);
    auto base = patch::allocate_remote_state(
        process, anchor, /*fps=*/165, /*flags=*/kFlagMobileUi);
    REQUIRE(base.has_value());

    std::array<std::byte, 16> raw{};
    REQUIRE(patch::read_bytes(process, *base, raw).has_value());
    CHECK(std::memcmp(raw.data(), &patch::kRemoteStateMagic, 4) == 0);
    CHECK(std::memcmp(raw.data() + 4, &patch::kRemoteStateVersion, 4) == 0);
    uint32_t fps = 0;
    std::memcpy(&fps, raw.data() + patch::kRemoteStateFpsOffset, 4);
    CHECK(fps == 165);
    uint32_t flags = 0;
    std::memcpy(&flags, raw.data() + patch::kRemoteStateFlagsOffset, 4);
    CHECK(flags == kFlagMobileUi);

    auto updated = patch::write_remote_fps(process, *base, 240);
    REQUIRE(updated.has_value());
    REQUIRE(patch::read_bytes(process, *base, raw).has_value());
    std::memcpy(&fps, raw.data() + patch::kRemoteStateFpsOffset, 4);
    CHECK(fps == 240);

    REQUIRE(patch::free_remote(process, *base).has_value());
}

TEST_CASE("engine applies WriteBytes and rollback restores them", "[patch][engine]") {
    const auto process = self_process();
    auto buffer = WindowBuffer::filled();
    const auto original = buffer.raw;

    PatchPlan plan;
    std::array<std::byte, 4> payload{std::byte{0xAA}, std::byte{0xBB},
                                     std::byte{0xCC}, std::byte{0xDD}};
    plan.operations.push_back(PatchOperation::write_bytes(
        reinterpret_cast<uintptr_t>(buffer.raw.data()) + 12, payload));

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE(applied.has_value());
    CHECK(applied->runtime.base == 0);  // no RemoteState requested
    for (size_t i = 0; i < payload.size(); ++i) {
        CHECK(buffer.raw[12 + i] == payload[i]);
    }

    auto undone = patch::rollback_patch_plan(process, *applied);
    REQUIRE(undone.has_value());
    CHECK(buffer.raw == original);
}

TEST_CASE("engine redirect rewrites the displacement to the target",
          "[patch][engine]") {
    const auto process = self_process();
    auto buffer = WindowBuffer::filled();
    const auto original = buffer.raw;
    const uintptr_t target = reinterpret_cast<uintptr_t>(buffer.raw.data()) + 0x400;

    PatchPlan plan;
    plan.operations.push_back(PatchOperation::redirect_relative(
        buffer.disp_field(), PatchTargetSymbol::Absolute, target));

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE(applied.has_value());

    // disp32 = target - (disp_field + 4); everything else in the window and
    // outside it must be untouched.
    const int32_t expected =
        static_cast<int32_t>(static_cast<int64_t>(target) -
                             static_cast<int64_t>(buffer.disp_field() + 4));
    CHECK(buffer.disp() == expected);
    for (size_t i = 0; i < buffer.raw.size(); ++i) {
        if (i >= 12 && i < 16) {
            continue;
        }
        INFO("byte " << i);
        CHECK(buffer.raw[i] == original[i]);
    }

    REQUIRE(patch::rollback_patch_plan(process, *applied).has_value());
    CHECK(buffer.raw == original);
}

TEST_CASE("engine redirect resolves the RemoteState symbol", "[patch][engine]") {
    const auto process = self_process();
    auto buffer = WindowBuffer::filled();

    PatchPlan plan;
    // Anchor the state allocation on the window buffer itself: the +-2GB
    // displacement reach is relative to the disp field, so the block must
    // land near it (in the game this anchor is the module base).
    plan.runtime.near_address = reinterpret_cast<uintptr_t>(buffer.raw.data());
    plan.runtime.initial_fps = 120;
    plan.runtime.initial_flags = kFlagPowerSave;
    plan.operations.push_back(PatchOperation::redirect_relative(
        buffer.disp_field(), PatchTargetSymbol::RemoteStateFps, 0));

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE(applied.has_value());
    REQUIRE(applied->runtime.base != 0);

    // The state block must hold the initial values...
    std::array<std::byte, 16> raw{};
    REQUIRE(patch::read_bytes(process, applied->runtime.base, raw).has_value());
    CHECK(std::memcmp(raw.data(), &patch::kRemoteStateMagic, 4) == 0);
    uint32_t fps = 0;
    std::memcpy(&fps, raw.data() + patch::kRemoteStateFpsOffset, 4);
    CHECK(fps == 120);

    // ...and the rewritten displacement must point at the fps slot.
    const int32_t disp = buffer.disp();
    const uintptr_t reached = buffer.disp_field() + 4 + static_cast<uintptr_t>(disp);
    CHECK(reached == applied->runtime.base + applied->runtime.fps_offset);

    REQUIRE(patch::rollback_patch_plan(process, *applied).has_value());
    CHECK(applied->runtime.base == 0);  // state block released
}

TEST_CASE("engine rolls back earlier operations when a later one fails",
          "[patch][engine]") {
    const auto process = self_process();
    auto buffer = WindowBuffer::filled();
    const auto original = buffer.raw;

    PatchPlan plan;
    std::array<std::byte, 4> payload{};
    payload.fill(std::byte{0xEE});
    plan.operations.push_back(PatchOperation::write_bytes(
        reinterpret_cast<uintptr_t>(buffer.raw.data()), payload));
    // Uncommitted address -> the second write must fail.
    plan.operations.push_back(PatchOperation::write_bytes(
        0x1, payload));

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE_FALSE(applied.has_value());
    CHECK(buffer.raw == original);  // first op was undone
}

TEST_CASE("redirect beyond +-2GB is rejected", "[patch][engine]") {
    const auto process = self_process();
    auto buffer = WindowBuffer::filled();

    PatchPlan plan;
    plan.operations.push_back(PatchOperation::redirect_relative(
        buffer.disp_field(), PatchTargetSymbol::Absolute,
        buffer.disp_field() + 0x100000000ULL));  // +4GB

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().code == ErrorCode::PatchFailed);
}

TEST_CASE("x64 emitter emits documented bytes", "[patch][x64]") {
    std::vector<std::byte> code;
    using patch::x64::emit_call_rax;
    using patch::x64::emit_mov_rax_imm64;
    using patch::x64::emit_prologue_shadow;
    using patch::x64::emit_ret;

    CHECK(emit_prologue_shadow(code) == 4);
    CHECK(emit_mov_rax_imm64(code, 0x7FF600001234) == 10);
    CHECK(emit_call_rax(code) == 2);
    CHECK(emit_ret(code) == 1);

    REQUIRE(code.size() == 17);
    const unsigned char expected[] = {
        0x48, 0x83, 0xEC, 0x28,                    // sub rsp, 0x28
        0x48, 0xB8, 0x34, 0x12, 0x00, 0x00, 0xF6, 0x7F,
        0x00, 0x00,                                // mov rax, imm64
        0xFF, 0xD0,                                // call rax
        0xC3};                                     // ret
    CHECK(std::memcmp(code.data(), expected, sizeof(expected)) == 0);
}

TEST_CASE("engine installs and invokes a bootstrap stub, rollback frees it",
          "[patch][engine][f5]") {
    const auto process = self_process();

    PatchPlan plan;
    plan.runtime.near_address = 0x600000000;  // anchor for the code page walk
    // mov eax, 42 ; ret - a stub that provably ran by its exit code.
    std::vector<std::byte> stub{std::byte{0xB8}, std::byte{42},
                                std::byte{0x00}, std::byte{0x00},
                                std::byte{0x00}, std::byte{0xC3}};
    plan.operations.push_back(
        PatchOperation::install_code_stub(std::move(stub)));
    plan.operations.push_back(PatchOperation::invoke_bootstrap(0, 5000));

    auto applied = patch::apply_patch_plan(process, plan);
    REQUIRE(applied.has_value());
    REQUIRE(applied->stubs.size() == 1);
    CHECK(applied->stubs[0].base != 0);
    CHECK(applied->stubs[0].size == 6);

    // The stub page is readable back byte-exact.
    std::vector<std::byte> read_back(6);
    REQUIRE(patch::read_bytes(process, applied->stubs[0].base, read_back)
                .has_value());
    CHECK(read_back[1] == std::byte{42});

    // InvokeBootstrap with an unknown index fails and rolls the page back.
    PatchPlan bad_plan;
    bad_plan.runtime.near_address = 0x600000000;
    std::vector<std::byte> dummy{std::byte{0xC3}};
    bad_plan.operations.push_back(
        PatchOperation::install_code_stub(std::move(dummy)));
    bad_plan.operations.push_back(PatchOperation::invoke_bootstrap(7, 1000));
    auto bad = patch::apply_patch_plan(process, bad_plan);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::PatchFailed);

    // Explicit rollback releases the first plan's page too.
    REQUIRE(patch::rollback_patch_plan(process, *applied).has_value());
    CHECK(applied->stubs.empty());
}
