#pragma once

// The in-game RemoteState block: a small RW page allocated inside the game
// process, holding everything a (future) resident component needs to steer
// the session - the self-contained replacement for the legacy shellcode
// channel that embedded the launcher's PID and &FpsValue (main.cpp:1434).

#include "domain/error.hpp"
#include "domain/patch_plan.hpp"
#include "platform/win32/unique_handle.hpp"

#include <cstdint>

namespace hoyoflux::patch {

// Memory layout of the block (little-endian):
//   +0x00 magic 'HOYF'   +0x04 layout version   +0x08 fps   +0x0C flags
inline constexpr uint32_t kRemoteStateMagic = 0x46594F48;  // 'HOYF'
inline constexpr uint32_t kRemoteStateVersion = 1;
inline constexpr uint32_t kRemoteStateFpsOffset = 8;
inline constexpr uint32_t kRemoteStateFlagsOffset = 12;

// Allocate a zeroed page in the game process and write the header + values.
// `near_address` anchors the allocation (module base) so that rip-relative
// redirects can reach the fps slot; see memory_writer.hpp.
Result<uintptr_t> allocate_remote_state(const win32::UniqueHandle& process,
                                        uintptr_t near_address, uint32_t fps,
                                        uint32_t flags);

// Update only the fps slot (dynamic-FPS extension point; a resident
// component writes this, it never re-resolves signatures).
Result<void> write_remote_fps(const win32::UniqueHandle& process,
                              uintptr_t state_base, uint32_t fps);

}  // namespace hoyoflux::patch
