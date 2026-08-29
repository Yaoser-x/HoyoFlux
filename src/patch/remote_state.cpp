#include "patch/remote_state.hpp"

#include "patch/memory_writer.hpp"

#include <array>
#include <cstring>

namespace hoyoflux::patch {
namespace {

// magic | version | fps | flags, in that byte order.
std::array<std::byte, 16> serialize(uint32_t fps, uint32_t flags) {
    std::array<std::byte, 16> raw{};
    std::memcpy(raw.data() + 0, &kRemoteStateMagic, 4);
    std::memcpy(raw.data() + 4, &kRemoteStateVersion, 4);
    std::memcpy(raw.data() + kRemoteStateFpsOffset, &fps, 4);
    std::memcpy(raw.data() + kRemoteStateFlagsOffset, &flags, 4);
    return raw;
}

}  // namespace

Result<uintptr_t> allocate_remote_state(const win32::UniqueHandle& process,
                                        uintptr_t near_address, uint32_t fps,
                                        uint32_t flags) {
    auto base = allocate_near(process, near_address, 0x1000);
    if (!base) {
        return std::unexpected(base.error());
    }
    const auto raw = serialize(fps, flags);
    if (auto wrote = write_protected(process, *base,
                                     {raw.data(), raw.size()});
        !wrote) {
        free_remote(process, *base);  // best effort; report the write error
        return std::unexpected(wrote.error());
    }
    return base;
}

Result<void> write_remote_fps(const win32::UniqueHandle& process,
                              uintptr_t state_base, uint32_t fps) {
    std::array<std::byte, 4> raw{};
    std::memcpy(raw.data(), &fps, 4);
    return write_protected(process, state_base + kRemoteStateFpsOffset, raw);
}

}  // namespace hoyoflux::patch
