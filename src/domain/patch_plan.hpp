#pragma once

// Patch plan: what a GameAdapter wants applied to a launched game process.
//
// The adapter is the only module that knows *what* to patch; the patch
// engine is the only module that knows *how* to write remote memory. This
// file holds the plain-data contract between them.
//
// 1.0.0 runtime model (deliberate break from the legacy shellcode): a fixed
// profile needs no resident launcher component. Every operation below is a
// pure external write - after the engine applies the plan the game runs on
// its own and the launcher may exit. Hooks that must execute *inside* the
// game use an explicit lifecycle-detour operation; arbitrary remote-thread
// invocation remains separate and is never used for Genshin Mobile UI.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hoyoflux {

enum class PatchOperationKind {
    // Overwrite `data.size()` bytes at `address` (protect-flow write).
    WriteBytes,

    // The 32-bit displacement at `address` is the final operand of a
    // rip-relative instruction (target == address + 4 + disp). Rewrite the
    // displacement so the instruction reaches `target` instead. The engine
    // writes the containing 8-byte-aligned 16-byte window, exactly like the
    // legacy basefps hook (main.cpp:1512-1532), so neighbouring instruction
    // bytes are restored unchanged.
    RedirectRelative,

    // F5: install position-independent code (data = machine code) on a fresh
    // executable page near the plan's anchor. Nothing is overwritten and
    // nothing executes until an InvokeBootstrap names the stub.
    InstallCodeStub,

    // F5: run a previously installed stub via a remote thread and wait for
    // its exit (wait_timeout_ms). `stub_index` refers to the InstallCodeStub
    // operations earlier in the same plan.
    InvokeBootstrap,

    // Replace the first 16 bytes at a function entry with an absolute jump to
    // an executable stub. The engine supplies the saved entry bytes and the
    // target-process VirtualProtect address at the declared stub offsets so
    // the stub can self-unhook before resuming the original function.
    InstallFunctionEntryDetour,
};

// Where an operation's target lives. RemoteStateFps refers to the fps slot
// of the in-game RemoteState block the engine allocates while applying the
// plan - adapters build plans before any remote address exists.
enum class PatchTargetSymbol : unsigned char {
    Absolute,      // target_address is the absolute address
    RemoteStateFps,  // target = runtime.base + runtime.fps_offset
};

struct PatchOperation {
    PatchOperationKind kind{PatchOperationKind::WriteBytes};
    uintptr_t address{0};
    // WriteBytes / InstallCodeStub / InstallFunctionEntryDetour payload.
    std::vector<std::byte> data;

    // F10: when non-empty, the engine reads the remote bytes first and
    // refuses to patch unless they match exactly. A signature that matched
    // by coincidence then fails loudly instead of corrupting memory.
    std::vector<std::byte> expected_original;

    PatchTargetSymbol target_symbol{PatchTargetSymbol::Absolute};
    uintptr_t target_address{0};  // RedirectRelative when symbol == Absolute
    uint32_t stub_index{0};       // InvokeBootstrap: index of installed stub
    uint32_t wait_timeout_ms{5000};  // InvokeBootstrap
    uint32_t original_bytes_offset{0};
    uint32_t virtual_protect_offset{0};

    [[nodiscard]] static PatchOperation write_bytes(
        uintptr_t address, std::span<const std::byte> bytes,
        std::span<const std::byte> expected = {}) {
        PatchOperation op;
        op.kind = PatchOperationKind::WriteBytes;
        op.address = address;
        op.data.assign(bytes.begin(), bytes.end());
        op.expected_original.assign(expected.begin(), expected.end());
        return op;
    }
    [[nodiscard]] static PatchOperation write_u32(uintptr_t address, uint32_t value) {
        std::vector<std::byte> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        PatchOperation op;
        op.kind = PatchOperationKind::WriteBytes;
        op.address = address;
        op.data = std::move(bytes);
        return op;
    }
    [[nodiscard]] static PatchOperation redirect_relative(uintptr_t disp_field,
                                                          PatchTargetSymbol symbol,
                                                          uintptr_t target) {
        PatchOperation op;
        op.kind = PatchOperationKind::RedirectRelative;
        op.address = disp_field;
        op.target_symbol = symbol;
        op.target_address = target;
        return op;
    }
    [[nodiscard]] static PatchOperation install_code_stub(
        std::vector<std::byte> code) {
        PatchOperation op;
        op.kind = PatchOperationKind::InstallCodeStub;
        op.data = std::move(code);
        return op;
    }
    [[nodiscard]] static PatchOperation invoke_bootstrap(uint32_t stub_index,
                                                         uint32_t wait_timeout_ms) {
        PatchOperation op;
        op.kind = PatchOperationKind::InvokeBootstrap;
        op.stub_index = stub_index;
        op.wait_timeout_ms = wait_timeout_ms;
        return op;
    }
    [[nodiscard]] static PatchOperation install_function_entry_detour(
        uintptr_t function_entry, std::vector<std::byte> code,
        uint32_t original_bytes_offset, uint32_t virtual_protect_offset) {
        PatchOperation op;
        op.kind = PatchOperationKind::InstallFunctionEntryDetour;
        op.address = function_entry;
        op.data = std::move(code);
        op.original_bytes_offset = original_bytes_offset;
        op.virtual_protect_offset = virtual_protect_offset;
        return op;
    }
};

struct MobileUiDiagnostic {
    std::string variant;
    uintptr_t grph_class_global{0};
    int32_t grph_ui_offset{0};
    int32_t grph_input_offset{0};
    uintptr_t func_gui_set{0};
    uintptr_t func_input_set{0};
    uintptr_t lifecycle_call_disp_diagnostic{0};
    uintptr_t lifecycle_function_entry{0};
    uint32_t telemetry_offset{0};
};

struct MobileUiTelemetry {
    uint32_t function_entry_hits{0};
    uint32_t graph_ready{0};
    uint32_t ui_ready{0};
    uint32_t input_ready{0};
    uint32_t gui_set_called{0};
    uint32_t input_set_called{0};
    uint32_t self_unhooked{0};
    uint32_t original_resumed{0};
    uint32_t completed{0};
};

// Flags for RemoteStateLayout::initial_flags.
inline constexpr uint32_t kFlagMobileUi = 0x1;
inline constexpr uint32_t kFlagPowerSave = 0x2;

// Layout of the in-game RemoteState block, allocated by the patch engine in
// the game process. Self-contained: a future resident component (dynamic FPS,
// DisplayGuard) updates fps/flags by writing here - it never needs the
// launcher's variable addresses (the legacy shellcode embedded &FpsValue,
// main.cpp:1434, which forced launcher residency; that channel is gone).
struct RemoteStateLayout {
    // Page anchor for the in-game allocation. Rip-relative displacements are
    // +-2GB, so the block is allocated below this address (module base).
    uintptr_t near_address{0};

    uint32_t initial_fps{0};     // written once at allocation
    uint32_t initial_flags{0};   // bit 0: mobile_ui, bit 1: power_save

    // Filled by the engine during apply().
    uintptr_t base{0};
    uint32_t fps_offset{8};
    uint32_t flags_offset{12};
};

struct PatchPlan {
    std::vector<PatchOperation> operations;
    RemoteStateLayout runtime;

    // F7: for plans that patch the fps variable directly (no RemoteState
    // redirect), the address a resident controller writes profile fps to.
    // Empty = the plan has no dynamic fps channel (e.g. fps redirect only).
    std::optional<uintptr_t> fps_direct_address;
    std::optional<MobileUiDiagnostic> mobile_ui_diagnostic;
};

}  // namespace hoyoflux
