#include "game/genshin/mobile_ui.hpp"

#include "patch/x64_emit.hpp"

namespace hoyoflux::game::genshin {
namespace {

// Signature ids the bootstrap needs, in the order they are consumed.
constexpr std::string_view kMobileUiIds[] = {"genshin.mobileui.v1",
                                             "genshin.mobileui.v2"};

struct MobileUiFields {
    // genshin.mobileui.v1/v2 resolver order (recorded in the signature
    // table): Grph_class, Grph_UIcl_VA, Func_gui_set, Func_input_set.
    uintptr_t grph_class{0};
    uintptr_t grph_uicl_va{0};
    uintptr_t func_gui_set{0};
    uintptr_t func_input_set{0};
};

Result<MobileUiFields> resolve_fields(
    const std::vector<ResolvedSignature>& resolved) {
    const ResolvedSignature* entry = nullptr;
    for (const auto id : kMobileUiIds) {
        entry = find_resolved(resolved, id);
        if (entry != nullptr) {
            break;
        }
    }
    if (entry == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "mobile UI requested but no genshin.mobileui signature resolved "
            "(run `hoyoflux doctor`)"));
    }
    if (entry->fields.size() < 4) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "resolved genshin.mobileui signature lacks its fields"));
    }
    MobileUiFields fields;
    fields.grph_class = entry->fields[0];
    fields.grph_uicl_va = entry->fields[1];
    fields.func_gui_set = entry->fields[2];
    fields.func_input_set = entry->fields[3];
    return fields;
}

}  // namespace

std::vector<std::byte> GenshinMobileUiPatchBuilder::build_stub(
    const std::vector<ResolvedSignature>& resolved) {
    auto fields = resolve_fields(resolved);
    if (!fields) {
        return {};
    }

    std::vector<std::byte> code;
    patch::x64::emit_prologue_shadow(code);
    // gui_set(Grph_class, Grph_UIcl_VA)
    patch::x64::emit_mov_rcx_imm64(code, fields->grph_class);
    patch::x64::emit_mov_rdx_imm64(code, fields->grph_uicl_va);
    patch::x64::emit_mov_rax_imm64(code, fields->func_gui_set);
    patch::x64::emit_call_rax(code);
    // input_set(Grph_class, ...)
    patch::x64::emit_mov_rcx_imm64(code, fields->grph_class);
    patch::x64::emit_mov_rax_imm64(code, fields->func_input_set);
    patch::x64::emit_call_rax(code);
    patch::x64::emit_epilogue_shadow(code);
    patch::x64::emit_ret(code);
    patch::x64::emit_int3_padding(code, 4);
    return code;
}

Result<void> GenshinMobileUiPatchBuilder::add_operations(
    PatchPlan& plan, const PatchContext& context) {
    if (auto fields = resolve_fields(context.resolved); !fields) {
        return std::unexpected(fields.error());
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

}  // namespace hoyoflux::game::genshin
