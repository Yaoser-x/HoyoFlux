#pragma once

// Patch plan: what a GameAdapter wants applied to a launched game process.
//
// The adapter is the only module that knows *what* to patch; the patch
// engine is the only module that knows *how* to write remote memory. This
// file holds the plain-data contract between them. Exact op kinds are
// extended as the patch engine lands (A6).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace hoyoflux {

enum class PatchOperationKind {
    WriteBytes,             // overwrite `data.size()` bytes at `address`
    WriteDisplacement,      // patch a RIP-relative 32-bit displacement at `address`
};

struct PatchOperation {
    PatchOperationKind kind{PatchOperationKind::WriteBytes};
    uintptr_t address{0};  // absolute address in the target process
    std::vector<std::byte> data;

    [[nodiscard]] static PatchOperation write_bytes(uintptr_t address,
                                                    std::span<const std::byte> bytes) {
        return PatchOperation{PatchOperationKind::WriteBytes, address,
                              {bytes.begin(), bytes.end()}};
    }
    [[nodiscard]] static PatchOperation write_u32(uintptr_t address, uint32_t value) {
        std::vector<std::byte> bytes(sizeof(value));
        std::memcpy(bytes.data(), &value, sizeof(value));
        return PatchOperation{PatchOperationKind::WriteBytes, address, std::move(bytes)};
    }
};

// Layout of the in-game runtime state block allocated by the patch engine.
struct RemoteStateLayout {
    uintptr_t base{0};
    uint32_t fps_offset{0};
    uint32_t flags_offset{0};
};

struct PatchPlan {
    std::vector<PatchOperation> operations;
    RemoteStateLayout runtime;
};

}  // namespace hoyoflux
