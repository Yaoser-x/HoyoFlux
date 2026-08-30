#include "patch/patch_engine.hpp"

#include "platform/win32/process.hpp"
#include "patch/memory_writer.hpp"
#include "patch/remote_state.hpp"
#include "scan/module_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

namespace hoyoflux::patch {
namespace {

// The redirect window mirrors the legacy basefps hook: an 8-byte-aligned
// 16-byte read-modify-write around the displacement field
// (main.cpp:1518-1532). Neighbouring instruction bytes are rewritten
// unchanged, which keeps the operation idempotent and rollback exact.
constexpr size_t kRedirectWindow = 16;

int32_t redirect_displacement(uintptr_t disp_field, uintptr_t new_target) {
    const int64_t delta =
        static_cast<int64_t>(new_target) - static_cast<int64_t>(disp_field + 4);
    return static_cast<int32_t>(delta);
}

[[nodiscard]] bool displacement_in_range(uintptr_t disp_field,
                                         uintptr_t new_target) {
    const int64_t delta =
        static_cast<int64_t>(new_target) - static_cast<int64_t>(disp_field + 4);
    return delta >= std::numeric_limits<int32_t>::min() &&
           delta <= std::numeric_limits<int32_t>::max();
}

Result<uintptr_t> remote_virtual_protect(
    const win32::UniqueHandle& process) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto local = reinterpret_cast<uintptr_t>(
        GetProcAddress(kernel32, "VirtualProtect"));
    if (local == 0) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "GetProcAddress(VirtualProtect) failed",
            GetLastError()));
    }
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local), &owner)) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "cannot locate VirtualProtect owner module",
            GetLastError()));
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(owner, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return std::unexpected(Error::make(
            ErrorCode::OsError, "cannot read VirtualProtect owner path",
            GetLastError()));
    }
    const auto module_name =
        std::filesystem::path(std::wstring_view(path, length)).filename().string();
    auto remote_base = scan::remote_module_base(process, module_name);
    if (!remote_base) {
        return std::unexpected(remote_base.error());
    }
    return *remote_base +
           (local - reinterpret_cast<uintptr_t>(owner));
}

uintptr_t resolve_target(const PatchOperation& op,
                         const RemoteStateLayout& runtime) {
    if (op.target_symbol == PatchTargetSymbol::RemoteStateFps) {
        return runtime.base + runtime.fps_offset;
    }
    return op.target_address;
}

Result<AppliedOperation> apply_one(const win32::UniqueHandle& process,
                                   const PatchOperation& op,
                                   const RemoteStateLayout& runtime,
                                   std::vector<AppliedStub>& stubs) {
    AppliedOperation record;
    record.op = op;

    switch (op.kind) {
    case PatchOperationKind::WriteBytes:
        record.original.assign(op.data.size(), std::byte{0});
        if (auto read = read_bytes(process, op.address, record.original); !read) {
            return std::unexpected(read.error());
        }
        if (!op.expected_original.empty() &&
            (record.original.size() != op.expected_original.size() ||
             !std::equal(op.expected_original.begin(),
                         op.expected_original.end(),
                         record.original.begin()))) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                "expected-bytes mismatch at " + std::to_string(op.address) +
                    ": the target does not look like the pattern this patch "
                    "was built for; refusing to write"));
        }
        if (auto wrote = write_protected(process, op.address, op.data); !wrote) {
            return std::unexpected(wrote.error());
        }
        // Code pages may be cached: flush so the game never executes stale
        // bytes (plan 19.1).
        FlushInstructionCache(process.get(),
                              reinterpret_cast<LPCVOID>(op.address),
                              op.data.size());
        break;

    case PatchOperationKind::RedirectRelative: {
        const uintptr_t target = resolve_target(op, runtime);
        if (runtime.base == 0 &&
            op.target_symbol == PatchTargetSymbol::RemoteStateFps) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed, "RemoteState not allocated but required"));
        }
        if (!displacement_in_range(op.address, target)) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                "redirect target out of +-2GB reach of " + std::to_string(op.address)));
        }
        const uintptr_t window_base = op.address & ~uintptr_t{7};
        record.original.assign(kRedirectWindow, std::byte{0});
        if (auto read =
                read_bytes(process, window_base, record.original);
            !read) {
            return std::unexpected(read.error());
        }

        std::byte window[kRedirectWindow];
        std::memcpy(window, record.original.data(), kRedirectWindow);
        const int32_t disp = redirect_displacement(op.address, target);
        const size_t offset = op.address & 7;
        std::memcpy(window + offset, &disp, sizeof(disp));
        if (auto wrote = write_protected(
                process, window_base, {window, kRedirectWindow});
            !wrote) {
            return std::unexpected(wrote.error());
        }
        FlushInstructionCache(process.get(),
                              reinterpret_cast<LPCVOID>(window_base),
                              kRedirectWindow);
        break;
    }

    case PatchOperationKind::InstallCodeStub: {
        if (runtime.near_address == 0) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                "InstallCodeStub requires the plan's module anchor"));
        }
        auto base = allocate_code_near(process, runtime.near_address,
                                       op.data.size());
        if (!base) {
            return std::unexpected(base.error());
        }
        SIZE_T written = 0;
        if (!WriteProcessMemory(process.get(),
                                reinterpret_cast<LPVOID>(*base), op.data.data(),
                                op.data.size(), &written) ||
            written != op.data.size()) {
            (void)free_remote(process, *base);
            return std::unexpected(Error::make(
                ErrorCode::RemoteWriteFailed,
                "stub write failed at " + std::to_string(*base),
                GetLastError()));
        }
        // FlushInstructionCache: the page must be executable as soon as a
        // bootstrap thread runs it (plan F10 correctness rule).
        FlushInstructionCache(process.get(), reinterpret_cast<LPCVOID>(*base),
                              op.data.size());
        record.allocated_base = *base;
        stubs.push_back(AppliedStub{*base, op.data.size()});
        break;
    }

    case PatchOperationKind::InvokeBootstrap: {
        if (op.stub_index >= stubs.size()) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                "InvokeBootstrap references stub " +
                    std::to_string(op.stub_index) + " which is not installed"));
        }
        auto thread = win32::create_remote_thread(
            process, stubs[op.stub_index].base);
        if (!thread) {
            return std::unexpected(thread.error());
        }
        const DWORD wait = WaitForSingleObject(thread->get(),
                                               op.wait_timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                wait == WAIT_TIMEOUT
                    ? "bootstrap stub did not finish in time"
                    : "bootstrap stub wait failed"));
        }
        break;
    }

    case PatchOperationKind::InstallFunctionEntryDetour: {
        constexpr size_t kEntrySize = 16;
        if (op.original_bytes_offset + kEntrySize > op.data.size() ||
            op.virtual_protect_offset + sizeof(uintptr_t) > op.data.size()) {
            return std::unexpected(Error::make(
                ErrorCode::PatchFailed,
                "function-entry detour stub offsets are out of bounds"));
        }
        record.original.assign(kEntrySize, std::byte{0});
        if (auto read = read_bytes(process, op.address, record.original); !read) {
            return std::unexpected(read.error());
        }
        auto virtual_protect = remote_virtual_protect(process);
        if (!virtual_protect) {
            return std::unexpected(virtual_protect.error());
        }
        auto code = op.data;
        std::memcpy(code.data() + op.original_bytes_offset,
                    record.original.data(), kEntrySize);
        std::memcpy(code.data() + op.virtual_protect_offset,
                    &*virtual_protect, sizeof(*virtual_protect));

        auto base = allocate_code(process, code.size());
        if (!base) {
            return std::unexpected(base.error());
        }
        SIZE_T written = 0;
        if (!WriteProcessMemory(process.get(), reinterpret_cast<LPVOID>(*base),
                                code.data(), code.size(), &written) ||
            written != code.size()) {
            (void)free_remote(process, *base);
            return std::unexpected(Error::make(
                ErrorCode::RemoteWriteFailed,
                "function-entry detour stub write failed", GetLastError()));
        }
        FlushInstructionCache(process.get(), reinterpret_cast<LPCVOID>(*base),
                              code.size());
        record.allocated_base = *base;
        std::array<std::byte, kEntrySize> jump{
            std::byte{0xFF}, std::byte{0x25}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}};
        std::memcpy(jump.data() + 6, &*base, sizeof(*base));
        jump[14] = std::byte{0x90};
        jump[15] = std::byte{0x90};
        if (auto wrote = write_protected(process, op.address, jump);
            !wrote) {
            (void)free_remote(process, *base);
            return std::unexpected(wrote.error());
        }
        stubs.push_back(AppliedStub{*base, code.size()});
        FlushInstructionCache(process.get(), reinterpret_cast<LPCVOID>(op.address),
                              kEntrySize);
        break;
    }
    }
    return record;
}

Result<void> undo_one(const win32::UniqueHandle& process,
                      const AppliedOperation& record) {
    if (record.original.empty()) {
        return {};
    }
    uintptr_t address = record.op.address;
    if (record.op.kind == PatchOperationKind::RedirectRelative) {
        address &= ~uintptr_t{7};  // window base
    }
    auto restored = write_protected(process, address, record.original);
    if (!restored) {
        return std::unexpected(restored.error());
    }
    if (record.op.kind == PatchOperationKind::WriteBytes ||
        record.op.kind == PatchOperationKind::RedirectRelative ||
        record.op.kind == PatchOperationKind::InstallFunctionEntryDetour) {
        FlushInstructionCache(process.get(), reinterpret_cast<LPCVOID>(address),
                              record.original.size());
    }
    return {};
}

}  // namespace

Result<AppliedPatch> apply_patch_plan(const win32::UniqueHandle& process,
                                      const PatchPlan& plan) {
    AppliedPatch applied;
    applied.runtime = plan.runtime;

    const bool needs_state = std::any_of(
        plan.operations.begin(), plan.operations.end(), [](const PatchOperation& op) {
            return op.target_symbol == PatchTargetSymbol::RemoteStateFps;
        });
    if (needs_state) {
        auto base = allocate_remote_state(process, plan.runtime.near_address,
                                          plan.runtime.initial_fps,
                                          plan.runtime.initial_flags);
        if (!base) {
            return std::unexpected(base.error());
        }
        applied.runtime.base = *base;
    }

    for (const auto& op : plan.operations) {
        auto record = apply_one(process, op, applied.runtime, applied.stubs);
        if (!record) {
            // rollback_patch_plan also releases the RemoteState block.
            rollback_patch_plan(process, applied);
            return std::unexpected(record.error());
        }
        applied.operations.push_back(std::move(*record));
    }
    return applied;
}

Result<void> rollback_patch_plan(const win32::UniqueHandle& process,
                                 AppliedPatch& applied) {
    for (auto it = applied.operations.rbegin(); it != applied.operations.rend();
         ++it) {
        if (auto undone = undo_one(process, *it); !undone) {
            return std::unexpected(undone.error());
        }
    }
    applied.operations.clear();
    for (auto it = applied.stubs.rbegin(); it != applied.stubs.rend(); ++it) {
        if (auto freed = free_remote(process, it->base); !freed) {
            return std::unexpected(freed.error());
        }
    }
    applied.stubs.clear();
    if (applied.runtime.base != 0) {
        if (auto freed = free_remote(process, applied.runtime.base); !freed) {
            applied.runtime.base = 0;
            return std::unexpected(freed.error());
        }
        applied.runtime.base = 0;
    }
    return {};
}

}  // namespace hoyoflux::patch
