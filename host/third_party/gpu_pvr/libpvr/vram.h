#pragma once

// ============================================================================
// VRAM path translation (HOLLY / CLX2)
//
// The Dreamcast's 8 MB of graphics SDRAM is organised as two 4 MB banks
// (Bank A and Bank B) that the HOLLY chip exposes through two different
// logical address paths:
//
//   * The 64-bit path -- used for texture data, render parameters
//     (ISP/TSP/TCW), the Region Array, Object Pointer Blocks, the fog
//     LUT backing data, and for render-to-texture writes (FB_W_SOF2).
//     On this path a byte address maps straight through to the physical
//     byte at the same offset.
//
//   * The 32-bit path -- used for framebuffer access by both the CORE
//     (when writing the final display image via FB_W_SOF1) and the
//     video output stage (when reading for TV display via FB_R_SOF1 /
//     FB_R_SOF2). On this path consecutive 32-bit words are interleaved
//     between Bank A and Bank B in 4-byte chunks:
//
//         bit 2 of the address selects the bank,
//         so even 32-bit words land in Bank A and odd ones in Bank B.
//
// Because both paths address the same physical SDRAM, data written on
// one path is visible on the other only at a *different* byte offset.
// This is the mechanism that makes the "32-bit frame buffer area" and
// the "64-bit texture area" described in devbox.md §3.4.10 appear as
// two disjoint logical regions, and it is essential for correctly
// modelling environment-mapping / render-to-texture scenarios.
//
// devbox.md §3.4.10 summarises the split as:
//
//     Register      Specified addresses        Access area
//     FB_W_SOF1,    0x0000000-0x0FFFFFC        32-bit (frame buffer)
//     FB_W_SOF2     0x1000000-0x1FFFFFC        64-bit (texture data)
//
// This header centralises the translation between logical path
// addresses and the flat physical byte buffer the library holds.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pvr {

// Standard Dreamcast VRAM size (8 MB). The library still supports
// alternative sizes (the physical buffer size is passed in at runtime),
// but the path interleave is defined in terms of this split.
constexpr uint32_t VRAM_BANK_SIZE = 0x00400000u;   // 4 MB per bank
constexpr uint32_t VRAM_TOTAL_SIZE = 0x00800000u;  // 8 MB total

// ---------------------------------------------------------------------------
// Path translation
//
// All addresses here are *byte* addresses expressed on the corresponding
// logical path. The returned value is a byte offset into the flat
// physical VRAM buffer held by the library.
// ---------------------------------------------------------------------------

// NOTE on the lxdream/Nitrocast embedding:
//
// The host emulator owns a single 8 MB `pvr2_main_ram` buffer that
// libpvr shares as its backing VRAM store. The guest (KOS and games
// built on it) writes region arrays, OPBs and ISP/TSP parameter
// blocks through the SH4 vram32 region (0x05xxxxxx), which deposits
// them linearly at `pvr2_main_ram[addr & 0x7FFFFF]`. libpvr's own TA
// and CORE writes also use linear addressing, so the plain `*_64` and
// `*_32` helpers are both identity mappings here.
//
// Textures are the one exception: the guest typically uploads them
// through the SH4 vram64 region (0x04xxxxxx), which lxdream stores
// using the bank-interleave translation in `vram_addr_tex` below.
// The texture sampler therefore goes through the `*_tex` helpers;
// everything else stays on the identity helpers.

// ============================================================================
// pcsx_rearmed / bloom embedding (patched from the lxdream embedding above)
//
// The host buffer IS the 64-bit texture address space: bloom's renderer holds
// host pointers into VRAM and does pointer arithmetic on them as pvr_ptr_t, so
// the texture path and the parameter/OPB/region path are identity. The 32-bit
// framebuffer path is the inverse of lxdream's XLAT: a 32-bit-path byte address
// a32 (bit 22 = bank B) lands at 64-bit-path address
//
//     a64 = ((a32 & 0x3FFFFC) << 1) | (((a32 >> 22) & 1) << 2) | (a32 & 3)
//
// which is the relation KOS relies on (pvr_get_front_buffer = 2 * frame32,
// bank B = +4 bytes in the 64-bit view; bloom's pvr_render_fb reads the front
// buffer back as an x32-stride RGB565 texture and shifts u by 2 texels for
// bank B). Everything in ta.cpp/core.cpp/texture.cpp is unchanged.
// ============================================================================

// 64-bit path: identity (params, OPB, region array).
static inline uint32_t vram_addr_64(uint32_t logical_addr,
                                    size_t physical_size) {
    return logical_addr & static_cast<uint32_t>(physical_size - 1);
}

// 32-bit path: bank interleave, inverse of XLAT (framebuffer reads/writes).
static inline uint32_t vram_addr_32(uint32_t logical_addr,
                                    size_t physical_size) {
    uint32_t a = logical_addr;
    uint32_t a64 = ((a & 0x003FFFFCu) << 1)
                 | (((a >> 22) & 1u) << 2)
                 | (a & 0x00000003u);
    return a64 & static_cast<uint32_t>(physical_size - 1);
}

// ---------------------------------------------------------------------------
// Texture-read path (lxdream embedding)
//
// Textures on the Dreamcast are uploaded by the guest through the SH4
// vram64 region (SH4 0x04xxxxxx, typically via P2-uncached writes or
// DMA). lxdream's `pvr2_vram64_write_*` stores those bytes into
// `pvr2_main_ram` using the bank-interleave translation
//
//     XLAT(a) = ((a & 0x00FFFFF8) >> 1)
//             | ((a & 0x00000004) << 20)
//             | ( a & 0x03);
//
// The HOLLY2 TCW.texture_address field is a byte offset in the *logical*
// 64-bit area, so to actually hit the bytes the guest deposited we must
// apply the same XLAT before indexing into `pvr2_main_ram`. Every other
// libpvr access (region array, OPBs, ISP/TSP parameters) is either
// internally written and read by libpvr itself, or was written by the
// guest through the vram32 path, so those stay on the identity helpers
// above.
// ---------------------------------------------------------------------------

static inline uint32_t vram_addr_tex(uint32_t logical_addr,
                                     size_t physical_size) {
    // pcsx_rearmed embedding: textures are written by the host straight
    // into the buffer, so the texture path is identity (see above).
    return logical_addr & static_cast<uint32_t>(physical_size - 1);
}

static inline uint8_t vram_read8_tex(const uint8_t* vram, size_t size,
                                     uint32_t addr) {
    if (!vram || size == 0) return 0;
    return vram[vram_addr_tex(addr, size)];
}

static inline uint16_t vram_read16_tex(const uint8_t* vram, size_t size,
                                       uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint8_t b0 = vram[vram_addr_tex(addr + 0, size)];
    uint8_t b1 = vram[vram_addr_tex(addr + 1, size)];
    return static_cast<uint16_t>(b0) |
           (static_cast<uint16_t>(b1) << 8);
}

static inline uint32_t vram_read32_tex(const uint8_t* vram, size_t size,
                                       uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= static_cast<uint32_t>(vram[vram_addr_tex(addr + i, size)]) << (i * 8);
    }
    return v;
}

static inline uint64_t vram_read64_tex(const uint8_t* vram, size_t size,
                                       uint32_t addr) {
    uint32_t lo = vram_read32_tex(vram, size, addr);
    uint32_t hi = vram_read32_tex(vram, size, addr + 4);
    return static_cast<uint64_t>(lo) |
           (static_cast<uint64_t>(hi) << 32);
}

// ---------------------------------------------------------------------------
// Typed helpers that go through one of the two paths.
//
// These helpers take a raw byte pointer to the physical VRAM buffer
// and the buffer size, translate the logical address through the
// requested path, and perform an aligned little-endian transfer.
// All transfers are byte-by-byte against the translated address, which
// means a multi-byte access that straddles the 4-byte interleave
// boundary on the 32-bit path will land on the correct physical bytes
// in each bank -- matching how the real hardware behaves.
// ---------------------------------------------------------------------------

static inline uint8_t vram_read8_64(const uint8_t* vram, size_t size,
                                    uint32_t addr) {
    if (!vram || size == 0) return 0;
    return vram[vram_addr_64(addr, size)];
}

static inline uint16_t vram_read16_64(const uint8_t* vram, size_t size,
                                      uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint8_t b0 = vram[vram_addr_64(addr + 0, size)];
    uint8_t b1 = vram[vram_addr_64(addr + 1, size)];
    return static_cast<uint16_t>(b0) |
           (static_cast<uint16_t>(b1) << 8);
}

static inline uint32_t vram_read32_64(const uint8_t* vram, size_t size,
                                      uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint32_t v = 0;
    std::memcpy(&v, vram + vram_addr_64(addr, size), 4);
    return v;
}

static inline uint64_t vram_read64_64(const uint8_t* vram, size_t size,
                                      uint32_t addr) {
    uint32_t lo = vram_read32_64(vram, size, addr);
    uint32_t hi = vram_read32_64(vram, size, addr + 4);
    return static_cast<uint64_t>(lo) |
           (static_cast<uint64_t>(hi) << 32);
}

static inline void vram_write8_64(uint8_t* vram, size_t size,
                                  uint32_t addr, uint8_t value) {
    if (!vram || size == 0) return;
    vram[vram_addr_64(addr, size)] = value;
}

static inline void vram_write16_64(uint8_t* vram, size_t size,
                                   uint32_t addr, uint16_t value) {
    if (!vram || size == 0) return;
    std::memcpy(vram + vram_addr_64(addr, size), &value, 2);
}

static inline void vram_write32_64(uint8_t* vram, size_t size,
                                   uint32_t addr, uint32_t value) {
    if (!vram || size == 0) return;
    std::memcpy(vram + vram_addr_64(addr, size), &value, 4);
}

static inline uint8_t vram_read8_32(const uint8_t* vram, size_t size,
                                    uint32_t addr) {
    if (!vram || size == 0) return 0;
    return vram[vram_addr_32(addr, size)];
}

static inline uint16_t vram_read16_32(const uint8_t* vram, size_t size,
                                      uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint8_t b0 = vram[vram_addr_32(addr + 0, size)];
    uint8_t b1 = vram[vram_addr_32(addr + 1, size)];
    return static_cast<uint16_t>(b0) |
           (static_cast<uint16_t>(b1) << 8);
}

static inline uint32_t vram_read32_32(const uint8_t* vram, size_t size,
                                      uint32_t addr) {
    if (!vram || size == 0) return 0;
    uint32_t v = 0;
    std::memcpy(&v, vram + vram_addr_32(addr, size), 4);
    return v;
}

static inline void vram_write8_32(uint8_t* vram, size_t size,
                                  uint32_t addr, uint8_t value) {
    if (!vram || size == 0) return;
    vram[vram_addr_32(addr, size)] = value;
}

static inline void vram_write16_32(uint8_t* vram, size_t size,
                                   uint32_t addr, uint16_t value) {
    if (!vram || size == 0) return;
    vram[vram_addr_32(addr + 0, size)] = static_cast<uint8_t>(value & 0xFF);
    vram[vram_addr_32(addr + 1, size)] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static inline void vram_write32_32(uint8_t* vram, size_t size,
                                   uint32_t addr, uint32_t value) {
    if (!vram || size == 0) return;
    std::memcpy(vram + vram_addr_32(addr, size), &value, 4);
}

// ---------------------------------------------------------------------------
// FB_W_SOF2 region validation
//
// §3.4.10 distinguishes render-to-texture writes (which target the
// 64-bit area, nominally 0x1000000-0x1FFFFFC in the documented address
// space) from normal framebuffer writes (FB_W_SOF1, 32-bit area). The
// hardware derives the path from which register triggered the write
// rather than from the address itself; the helper below reflects the
// documented convention but isn't used to gate access.
// ---------------------------------------------------------------------------

static inline bool fb_w_sof2_is_texture_region(uint32_t addr) {
    return (addr >= 0x01000000u) && (addr <= 0x01FFFFFCu);
}

} // namespace pvr