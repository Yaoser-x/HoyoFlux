#include "game/genshin/mobile_ui.hpp"

#include "patch/x64_emit.hpp"

#include <cstddef>
#include <cstring>

namespace hoyoflux::game::genshin {
namespace {

constexpr std::string_view kMobileUiIds[] = {"genshin.mobileui.v1",
                                             "genshin.mobileui.v2"};

struct MobileUiFields {
    std::string_view variant;
    uintptr_t grph_class_global{0};
    int32_t grph_ui_offset{0};
    int32_t grph_input_offset{0};
    uintptr_t func_gui_set{0};
    uintptr_t func_input_set{0};
    uintptr_t lifecycle_call_disp_diagnostic{0};
    uintptr_t lifecycle_function_entry{0};
};

Result<MobileUiFields> resolve_fields(
    const std::vector<ResolvedSignature>& resolved) {
    const ResolvedSignature* mobile = nullptr;
    std::string_view variant;
    for (const auto id : kMobileUiIds) {
        mobile = find_resolved(resolved, id);
        if (mobile != nullptr) {
            variant = id.ends_with("v1") ? "v1" : "v2";
            break;
        }
    }
    const auto* input = find_resolved(resolved, "genshin.mobileui.input");
    const auto* lifecycle = find_resolved(resolved, "genshin.unhooktime");
    if (mobile == nullptr || input == nullptr || lifecycle == nullptr) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "mobile UI requires mobileui.v1/v2, mobileui.input and "
            "unhooktime signatures; at least one did not resolve"));
    }
    if (mobile->fields.size() < 4 || input->fields.empty() ||
        lifecycle->fields.size() < 2) {
        return std::unexpected(Error::make(
            ErrorCode::SignatureNotFound,
            "resolved mobile UI signatures lack required typed fields"));
    }

    MobileUiFields fields;
    fields.variant = variant;
    fields.grph_class_global = mobile->fields[0];
    fields.grph_ui_offset =
        static_cast<int32_t>(static_cast<uint32_t>(mobile->fields[1]));
    fields.grph_input_offset =
        static_cast<int32_t>(static_cast<uint32_t>(input->fields[0]));
    fields.func_gui_set = mobile->fields[2];
    fields.func_input_set = mobile->fields[3];
    fields.lifecycle_call_disp_diagnostic = lifecycle->fields[0];
    fields.lifecycle_function_entry = lifecycle->fields[1];
    return fields;
}

void append(std::vector<std::byte>& out,
            std::initializer_list<unsigned char> bytes) {
    for (const auto byte : bytes) {
        out.push_back(static_cast<std::byte>(byte));
    }
}

void append_i32(std::vector<std::byte>& out, int32_t value) {
    const auto* raw = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), raw, raw + sizeof(value));
}

void patch_rel32(std::vector<std::byte>& code, size_t field, size_t target) {
    const int32_t disp = static_cast<int32_t>(target - (field + 4));
    std::memcpy(code.data() + field, &disp, sizeof(disp));
}

struct BuiltStub {
    std::vector<std::byte> code;
    uint32_t telemetry_offset{0};
    uint32_t original_bytes_offset{0};
    uint32_t virtual_protect_offset{0};
};

size_t emit_increment_counter(std::vector<std::byte>& code) {
    append(code, {0xFF, 0x05});  // inc dword ptr [rip+telemetry]
    const size_t field = code.size();
    append_i32(code, 0);
    return field;
}

size_t emit_set_counter(std::vector<std::byte>& code) {
    append(code, {0xC7, 0x05});  // mov dword ptr [rip+telemetry], 1
    const size_t field = code.size();
    append_i32(code, 0);
    append_i32(code, 1);
    return field;
}

BuiltStub build_function_entry_stub(const MobileUiFields& fields) {
    std::vector<std::byte> code;
    append(code, {0x53, 0x55, 0x56, 0x57});  // push rbx/rbp/rsi/rdi
    append(code, {0x48, 0x83, 0xEC, 0x58});  // shadow + locals, aligned
    const size_t function_entry_hits = emit_increment_counter(code);
    append(code, {0x48, 0x89, 0x4C, 0x24, 0x20});  // save original rcx
    append(code, {0x48, 0x89, 0x54, 0x24, 0x28});  // mirror upstream rdx save

    // VirtualProtect(entry, 16, PAGE_EXECUTE_READWRITE, &old_protect)
    patch::x64::emit_mov_rcx_imm64(code, fields.lifecycle_function_entry);
    patch::x64::emit_mov_edx_imm32(code, 16);
    append(code, {0x41, 0xB8});
    append_i32(code, 0x40);  // PAGE_EXECUTE_READWRITE
    append(code, {0x4C, 0x8D, 0x4C, 0x24, 0x30});
    append(code, {0x48, 0x8B, 0x05});  // mov rax, [rip+vp_slot]
    const size_t vp_call_1 = code.size();
    append_i32(code, 0);
    patch::x64::emit_call_rax(code);

    // Restore the exact 16 entry bytes before calling the original function.
    append(code, {0x48, 0xBF});  // mov rdi, entry
    const auto entry = fields.lifecycle_function_entry;
    const auto* entry_raw = reinterpret_cast<const std::byte*>(&entry);
    code.insert(code.end(), entry_raw, entry_raw + sizeof(entry));
    append(code, {0xF3, 0x0F, 0x6F, 0x05});  // movdqu xmm0, [rip+original]
    const size_t original_load = code.size();
    append_i32(code, 0);
    append(code, {0xF3, 0x0F, 0x7F, 0x07});  // movdqu [rdi], xmm0
    const size_t self_unhooked = emit_set_counter(code);

    // Restore the page's original protection.
    patch::x64::emit_mov_rcx_imm64(code, fields.lifecycle_function_entry);
    patch::x64::emit_mov_edx_imm32(code, 16);
    append(code, {0x44, 0x8B, 0x44, 0x24, 0x30});  // mov r8d, old_protect
    append(code, {0x4C, 0x8D, 0x4C, 0x24, 0x34});
    append(code, {0x48, 0x8B, 0x05});
    const size_t vp_call_2 = code.size();
    append_i32(code, 0);
    patch::x64::emit_call_rax(code);

    // Upstream restores RCX and calls the now-unhooked original entry.
    append(code, {0x48, 0x8B, 0x4C, 0x24, 0x20});
    patch::x64::emit_mov_rax_imm64(code, fields.lifecycle_function_entry);
    patch::x64::emit_call_rax(code);
    const size_t original_resumed = emit_set_counter(code);

    patch::x64::emit_mov_rax_imm64(code, fields.grph_class_global);
    append(code, {0x48, 0x8B, 0x18});  // rbx = [grph_class_global]
    append(code, {0x48, 0x85, 0xDB, 0x0F, 0x84});
    const size_t null_global_jump = code.size();
    append_i32(code, 0);
    const size_t graph_ready = emit_set_counter(code);

    append(code, {0x48, 0x8B, 0x8B});  // rcx = [rbx+ui_offset]
    append_i32(code, fields.grph_ui_offset);
    append(code, {0x48, 0x85, 0xC9, 0x0F, 0x84});  // telemetry only
    const size_t ui_not_ready = code.size();
    append_i32(code, 0);
    const size_t ui_ready = emit_set_counter(code);
    const size_t ui_call = code.size();
    patch_rel32(code, ui_not_ready, ui_call);
    append(code, {0x31, 0xD2});  // upstream: edx = 0
    append(code, {0x41, 0xB8});  // mov r8d, 1
    append_i32(code, 1);
    patch::x64::emit_mov_rax_imm64(code, fields.func_gui_set);
    patch::x64::emit_call_rax(code);
    const size_t gui_set_called = emit_set_counter(code);

    append(code, {0x48, 0x8B, 0x8B});  // rcx = [rbx+input_offset]
    append_i32(code, fields.grph_input_offset);
    append(code, {0x48, 0x85, 0xC9, 0x0F, 0x84});
    const size_t input_not_ready = code.size();
    append_i32(code, 0);
    const size_t input_ready = emit_set_counter(code);
    const size_t input_call = code.size();
    patch_rel32(code, input_not_ready, input_call);
    append(code, {0x31, 0xD2});  // upstream: edx = 0
    append(code, {0x45, 0x31, 0xC0});  // r8d = false
    patch::x64::emit_mov_rax_imm64(code, fields.func_input_set);
    patch::x64::emit_call_rax(code);
    const size_t input_set_called = emit_set_counter(code);
    const size_t completed = emit_set_counter(code);

    const size_t done_label = code.size();
    append(code, {0x48, 0x83, 0xC4, 0x58});
    append(code, {0x5F, 0x5E, 0x5D, 0x5B, 0xC3});

    while ((code.size() % alignof(uintptr_t)) != 0) {
        code.push_back(std::byte{0});
    }
    const size_t virtual_protect = code.size();
    code.resize(code.size() + sizeof(uintptr_t), std::byte{0});
    const size_t original_bytes = code.size();
    code.resize(code.size() + 16, std::byte{0});
    while ((code.size() % alignof(MobileUiTelemetry)) != 0) {
        code.push_back(std::byte{0});
    }
    const size_t telemetry = code.size();
    code.resize(code.size() + sizeof(MobileUiTelemetry), std::byte{0});
    patch::x64::emit_int3_padding(code, 8);

    patch_rel32(code, vp_call_1, virtual_protect);
    patch_rel32(code, vp_call_2, virtual_protect);
    patch_rel32(code, original_load, original_bytes);
    patch_rel32(code, null_global_jump, done_label);
    patch_rel32(code, function_entry_hits,
                telemetry + offsetof(MobileUiTelemetry, function_entry_hits));
    patch_rel32(code, graph_ready,
                telemetry + offsetof(MobileUiTelemetry, graph_ready));
    patch_rel32(code, ui_ready,
                telemetry + offsetof(MobileUiTelemetry, ui_ready));
    patch_rel32(code, input_ready,
                telemetry + offsetof(MobileUiTelemetry, input_ready));
    patch_rel32(code, gui_set_called,
                telemetry + offsetof(MobileUiTelemetry, gui_set_called));
    patch_rel32(code, input_set_called,
                telemetry + offsetof(MobileUiTelemetry, input_set_called));
    patch_rel32(code, self_unhooked,
                telemetry + offsetof(MobileUiTelemetry, self_unhooked));
    patch_rel32(code, original_resumed,
                telemetry + offsetof(MobileUiTelemetry, original_resumed));
    patch_rel32(code, completed,
                telemetry + offsetof(MobileUiTelemetry, completed));
    return BuiltStub{std::move(code), static_cast<uint32_t>(telemetry),
                     static_cast<uint32_t>(original_bytes),
                     static_cast<uint32_t>(virtual_protect)};
}

}  // namespace

std::vector<std::byte> GenshinMobileUiPatchBuilder::build_stub(
    const std::vector<ResolvedSignature>& resolved) {
    auto fields = resolve_fields(resolved);
    return fields ? build_function_entry_stub(*fields).code
                  : std::vector<std::byte>{};
}

Result<void> GenshinMobileUiPatchBuilder::add_operations(
    PatchPlan& plan, const PatchContext& context) {
    auto fields = resolve_fields(context.resolved);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    auto stub = build_function_entry_stub(*fields);
    plan.operations.push_back(PatchOperation::install_function_entry_detour(
        fields->lifecycle_function_entry, std::move(stub.code),
        stub.original_bytes_offset, stub.virtual_protect_offset));
    plan.mobile_ui_diagnostic = MobileUiDiagnostic{
        std::string(fields->variant), fields->grph_class_global,
        fields->grph_ui_offset, fields->grph_input_offset,
        fields->func_gui_set, fields->func_input_set,
        fields->lifecycle_call_disp_diagnostic,
        fields->lifecycle_function_entry,
        stub.telemetry_offset};
    return {};
}

}  // namespace hoyoflux::game::genshin
