#pragma once

// PatchEngine: applies a GameAdapter's PatchPlan to the (suspended) game
// process and can undo everything it did.
//
// Division of responsibility: the adapter says *what* (plan, in domain
// terms); this module says *how* (protect-flow writes, the 16-byte
// redirect window, RemoteState allocation). It never resolves signatures
// and never decides policy.
//
// Application order is the plan order; every operation records the bytes it
// overwrote so rollback() restores them in reverse.

#include "domain/error.hpp"
#include "domain/patch_plan.hpp"
#include "platform/win32/unique_handle.hpp"

#include <cstdint>
#include <vector>

namespace hoyoflux::patch {

struct AppliedOperation {
    PatchOperation op;
    std::vector<std::byte> original;  // bytes as they were before the write
    uintptr_t allocated_base{0};      // InstallCodeStub: page we own
};

// A bootstrap stub installed on its own executable page.
struct AppliedStub {
    uintptr_t base{0};
    size_t size{0};
};

struct AppliedPatch {
    std::vector<AppliedOperation> operations;  // application order
    std::vector<AppliedStub> stubs;            // installed code stubs
    RemoteStateLayout runtime;         // base filled during apply
};

// Apply every operation of `plan` to `process`. The plan's runtime block is
// allocated on demand (when an operation targets it); its address is filled
// into the returned AppliedPatch::runtime. On failure the already-applied
// operations are rolled back before the error is returned.
//
// Requires the plan's anchor (runtime.near_address) when the plan contains
// InstallCodeStub or InstallOneShotDetour operations.
Result<AppliedPatch> apply_patch_plan(const win32::UniqueHandle& process,
                                      const PatchPlan& plan);

// Undo an applied plan: restore original bytes in reverse order, free
// installed stub pages and release the RemoteState block. Only safe while
// the game has not run the patched code yet (or after it exited).
Result<void> rollback_patch_plan(const win32::UniqueHandle& process,
                                 AppliedPatch& applied);

}  // namespace hoyoflux::patch
