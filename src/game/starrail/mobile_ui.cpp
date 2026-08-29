#include "game/starrail/mobile_ui.hpp"

#include "patch/x64_emit.hpp"

namespace hoyoflux::game::starrail {
namespace {

constexpr std::string_view kUiSetIds[] = {"starrail.uiset.v1",
                                          "starrail.uiset.v2",
                                          "starrail.uiset.v3"};

// The uiset resolvers point at the setter function's write site; the legacy
// tool treated the resolved address as callable. Assumption recorded here
// pending the real-game experiment - the gate in build_patch_plan keeps
// this from running until then.
Result<std::vector<uintptr_t>> resolve_setters(
    const std::vector<ResolvedSignature>& resolved) {
    std::vector<uintptr_t> setters;
    for (const auto id : kUiSetIds) {
        if (const auto* entry = find_resolved(resolved, id);
            entry != nullptr && !entry->fields.empty()) {
            setters.push_back(entry->fields[0]);
        }
    }
    if (setters.empty()) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "mobile UI requested but no starrail.uiset signature resolved "
            "(run `hoyoflux doctor`)"));
    }
    return setters;
}

}  // namespace

std::vector<std::byte> StarRailMobileUiPatchBuilder::build_stub(
    const std::vector<ResolvedSignature>& resolved) {
    auto setters = resolve_setters(resolved);
    if (!setters) {
        return {};
    }

    std::vector<std::byte> code;
    patch::x64::emit_prologue_shadow(code);
    for (const uintptr_t setter : *setters) {
        patch::x64::emit_mov_ecx_imm32(code, 3);  // mobile UI mode constant
        patch::x64::emit_mov_rax_imm64(code, setter);
        patch::x64::emit_call_rax(code);
    }
    patch::x64::emit_epilogue_shadow(code);
    patch::x64::emit_ret(code);
    patch::x64::emit_int3_padding(code, 4);
    return code;
}

Result<void> StarRailMobileUiPatchBuilder::add_operations(
    PatchPlan& plan, const PatchContext& context) {
    if (auto setters = resolve_setters(context.resolved); !setters) {
        return std::unexpected(setters.error());
    }
    auto stub = build_stub(context.resolved);
    if (stub.empty()) {
        return std::unexpected(
            Error::make(ErrorCode::SignatureNotFound,
                        "mobile UI stub could not be composed"));
    }
    plan.operations.push_back(
        PatchOperation::install_code_stub(std::move(stub)));
    plan.operations.push_back(PatchOperation::invoke_bootstrap(
        /*stub_index=*/0, /*wait_timeout_ms=*/10000));
    return {};
}

}  // namespace hoyoflux::game::starrail
