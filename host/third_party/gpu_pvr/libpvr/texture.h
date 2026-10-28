#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

#include "pvr_types.h"
#include "registers.h"

namespace pvr {

class TextureSampler {
public:
    TextureSampler();

    void set_vram(const uint8_t* vram, size_t vram_size);
    void set_registers(const RegisterBank* regs);

    // Point the sampler at a frozen copy of the TA parameter area so that
    // texture bytes whose physical address (after bank-interleave translation)
    // falls inside that range are served from the snapshot rather than from
    // live VRAM.  Call with nullptr to disable.
    void set_snapshot(const uint8_t* data, uint32_t base, uint32_t size);

    // Sample a texture at the given UV coordinates with the given control words.
    // Returns ARGB8888 pixel.
    //
    // lod              – floating-point mip level (0 = full resolution).
    //                    Computed per-triangle by the rasterizer from screen-space
    //                    UV derivatives.  Non-zero only for mipmapped twiddled
    //                    textures (scan_order == 0 && mip_mapped == true).
    //                    TrilinearPassA uses floor(lod), TrilinearPassB uses
    //                    floor(lod)+1 so that the two passes sample adjacent mips.
    //
    // texel_half_offset – when true (default, matches HALF_OFFSET bit 2 = 1) the
    //                    bilinear 2×2 neighbourhood is centred on the sample point
    //                    by subtracting 0.5 before the floor.  When false the
    //                    neighbourhood starts at the top-left corner of the texel.
    Pixel sample(const TSPInstructionWord& tsp,
                 const TextureControlWord& tcw,
                 float u, float v,
                 float lod = 0.0f,
                 bool texel_half_offset = true) const;

private:
    const uint8_t* vram_ = nullptr;
    size_t vram_size_ = 0;
    const RegisterBank* regs_ = nullptr;

    // Snapshot of the physical VRAM byte range shared between TA parameters
    // and the bank-interleave texture path.  When non-null, any texture read
    // whose translated physical address falls within [snap_base_, snap_base_+snap_size_)
    // is served from snap_data_ rather than from live vram_.
    const uint8_t* snap_data_ = nullptr;
    uint32_t       snap_base_ = 0;
    uint32_t       snap_size_ = 0;

    // Fetch a single texel at integer coordinates (u_int, v_int)
    Pixel fetch_texel(const TextureControlWord& tcw,
                     int u_size, int v_size,
                     int u_int, int v_int) const;

    // Calculate twiddled address for a texel
    static uint32_t twiddle_address(int u, int v, int size_u, int size_v);

    // Read raw texel data from VRAM
    uint16_t read_vram16(uint32_t byte_addr) const;
    uint8_t read_vram8(uint32_t byte_addr) const;
    uint32_t read_vram32(uint32_t byte_addr) const;
    uint64_t read_vram64(uint32_t byte_addr) const;

    // Decode pixel formats
    static Pixel decode_argb1555(uint16_t raw);
    static Pixel decode_rgb565(uint16_t raw);
    static Pixel decode_argb4444(uint16_t raw);
    static Pixel decode_yuv422(uint8_t y, uint8_t u, uint8_t v);
    Pixel decode_palette_4bpp(uint8_t index, uint32_t palette_selector) const;
    Pixel decode_palette_8bpp(uint8_t index, uint32_t palette_selector) const;

    // Color data extension for Twiddled format
    static uint8_t extend_color_twiddled(uint8_t val, int bits);
    // Color data extension for Non-Twiddled format
    static uint8_t extend_color_non_twiddled(uint8_t val, int bits);

    // VQ texture support. `u_size` / `v_size` are the *texel* dimensions
    // of the texture; VQ operates on 2x2 texel blocks and the on-VRAM
    // index array is therefore (u_size/2) x (v_size/2) twiddled bytes.
    Pixel fetch_vq_texel(const TextureControlWord& tcw,
                        int u_size, int v_size,
                        int u_int, int v_int) const;

    // Bilinear filter helper
    static Pixel bilinear_filter(const Pixel& p00, const Pixel& p10,
                                 const Pixel& p01, const Pixel& p11,
                                 float frac_u, float frac_v);


};

} // namespace pvr
