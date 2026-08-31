#pragma once

// Minimal x64 code emitter for Mobile UI bootstrap stubs (plan F5).
//
// Only the instruction forms the bootstrap actually needs - loading 64/32-bit
// immediates into Windows x64 argument registers, calling through a register
// with proper shadow space, and returning. No assembler, no general JIT: a
// stub is a fixed byte sequence a game builder composes from these primitives
// and the engine installs into the suspended process.
//
// Every emit_* returns the number of bytes appended (for assertion tests).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hoyoflux::patch::x64 {

inline size_t emit_mov_rax_imm64(std::vector<std::byte>& out, uintptr_t value) {
    // 48 B8 imm64
    out.push_back(std::byte{0x48});
    out.push_back(std::byte{0xB8});
    std::byte raw[8];
    std::memcpy(raw, &value, sizeof(raw));
    out.insert(out.end(), raw, raw + 8);
    return 10;
}

inline size_t emit_call_rax(std::vector<std::byte>& out) {
    // FF D0
    out.push_back(std::byte{0xFF});
    out.push_back(std::byte{0xD0});
    return 2;
}

inline size_t emit_mov_rcx_imm64(std::vector<std::byte>& out, uintptr_t value) {
    // 48 B9 imm64
    out.push_back(std::byte{0x48});
    out.push_back(std::byte{0xB9});
    std::byte raw[8];
    std::memcpy(raw, &value, sizeof(raw));
    out.insert(out.end(), raw, raw + 8);
    return 10;
}

inline size_t emit_mov_rdx_imm64(std::vector<std::byte>& out, uintptr_t value) {
    // 48 BA imm64
    out.push_back(std::byte{0x48});
    out.push_back(std::byte{0xBA});
    std::byte raw[8];
    std::memcpy(raw, &value, sizeof(raw));
    out.insert(out.end(), raw, raw + 8);
    return 10;
}

inline size_t emit_mov_ecx_imm32(std::vector<std::byte>& out, uint32_t value) {
    // B9 imm32
    out.push_back(std::byte{0xB9});
    std::byte raw[4];
    std::memcpy(raw, &value, sizeof(raw));
    out.insert(out.end(), raw, raw + 4);
    return 5;
}

inline size_t emit_mov_edx_imm32(std::vector<std::byte>& out, uint32_t value) {
    // BA imm32
    out.push_back(std::byte{0xBA});
    std::byte raw[4];
    std::memcpy(raw, &value, sizeof(raw));
    out.insert(out.end(), raw, raw + 4);
    return 5;
}

// Reserves the x64 calling convention's 32-byte shadow space plus 8-byte
// alignment padding before any call sequence a stub makes.
inline size_t emit_prologue_shadow(std::vector<std::byte>& out) {
    // sub rsp, 0x28
    out.push_back(std::byte{0x48});
    out.push_back(std::byte{0x83});
    out.push_back(std::byte{0xEC});
    out.push_back(std::byte{0x28});
    return 4;
}

inline size_t emit_epilogue_shadow(std::vector<std::byte>& out) {
    // add rsp, 0x28
    out.push_back(std::byte{0x48});
    out.push_back(std::byte{0x83});
    out.push_back(std::byte{0xC4});
    out.push_back(std::byte{0x28});
    return 4;
}

inline size_t emit_ret(std::vector<std::byte>& out) {
    out.push_back(std::byte{0xC3});
    return 1;
}

inline size_t emit_int3_padding(std::vector<std::byte>& out, size_t count = 1) {
    for (size_t i = 0; i < count; ++i) {
        out.push_back(std::byte{0xCC});
    }
    return count;
}

}  // namespace hoyoflux::patch::x64
