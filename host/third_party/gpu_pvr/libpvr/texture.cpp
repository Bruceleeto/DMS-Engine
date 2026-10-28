#include <cstdlib>
#include "texture.h"
#include "vram.h"

#include <cstring>

namespace pvr {

// ============================================================================
// Construction / setup
// ============================================================================

TextureSampler::TextureSampler() = default;

void TextureSampler::set_vram(const uint8_t* vram, size_t vram_size) {
    vram_ = vram;
    vram_size_ = vram_size;
}

void TextureSampler::set_registers(const RegisterBank* regs) {
    regs_ = regs;
}

void TextureSampler::set_snapshot(const uint8_t* data, uint32_t base, uint32_t size) {
    snap_data_ = data;
    snap_base_ = base;
    snap_size_ = size;
}

// ============================================================================
// VRAM access helpers
//
// Textures live in the HOLLY "64-bit area" and, in the lxdream
// embedding, are deposited into pvr2_main_ram by pvr2_vram64_write_*
// via the bank-interleave translation. The TCW byte address therefore
// has to go through the same translation to hit the bytes the guest
// uploaded -- see the comment on vram_addr_tex() in vram.h. All other
// libpvr VRAM traffic (region array, OPBs, ISP/TSP params, framebuffer
// writes) uses the plain identity helpers; only the texture sampler
// needs the interleave.
// ============================================================================

uint8_t TextureSampler::read_vram8(uint32_t byte_addr) const {
    /* Check whether this texture byte's physical address (after the
     * bank-interleave translation) falls inside the parameter snapshot.
     * If so, serve it from the frozen copy so that concurrent TA writes
     * for the next frame do not corrupt texture reads. */
    if( snap_data_ != nullptr ) {
        uint32_t phys = vram_addr_tex( byte_addr, vram_size_ );
        uint32_t off  = phys - snap_base_;
        if( off < snap_size_ ) {
            return snap_data_[off];
        }
    }
    return vram_read8_tex( vram_, vram_size_, byte_addr );
}

uint16_t TextureSampler::read_vram16(uint32_t byte_addr) const {
    /* Build the 16-bit value byte-by-byte through the protected path so
     * that each byte is individually checked against the snapshot range
     * (the two bytes may translate to non-adjacent physical addresses). */
    uint8_t b0 = read_vram8( byte_addr     );
    uint8_t b1 = read_vram8( byte_addr + 1 );
    return static_cast<uint16_t>( b0 ) |
           ( static_cast<uint16_t>( b1 ) << 8 );
}

uint32_t TextureSampler::read_vram32(uint32_t byte_addr) const {
    uint8_t b0 = read_vram8( byte_addr     );
    uint8_t b1 = read_vram8( byte_addr + 1 );
    uint8_t b2 = read_vram8( byte_addr + 2 );
    uint8_t b3 = read_vram8( byte_addr + 3 );
    return static_cast<uint32_t>( b0 )
         | ( static_cast<uint32_t>( b1 ) <<  8 )
         | ( static_cast<uint32_t>( b2 ) << 16 )
         | ( static_cast<uint32_t>( b3 ) << 24 );
}

uint64_t TextureSampler::read_vram64(uint32_t byte_addr) const {
    uint32_t lo = read_vram32( byte_addr     );
    uint32_t hi = read_vram32( byte_addr + 4 );
    return static_cast<uint64_t>( lo ) |
           ( static_cast<uint64_t>( hi ) << 32 );
}

// ============================================================================
// Color extension helpers
// ============================================================================

uint8_t TextureSampler::extend_color_twiddled(uint8_t val, int bits) {
    // Extends a color value to 8 bits by repeating the MSBs at the LSBs.
    switch (bits) {
        case 1: return val ? 0xFF : 0x00;
        case 4: return static_cast<uint8_t>((val << 4) | val);
        case 5: return static_cast<uint8_t>((val << 3) | (val >> 2));
        case 6: return static_cast<uint8_t>((val << 2) | (val >> 4));
        case 8: return val;
        default: {
            // Generic: shift left by (8-bits), then OR with (val >> (2*bits - 8))
            int shift = 8 - bits;
            if (shift < 0 || shift > 7) return val;
            return static_cast<uint8_t>((val << shift) | (val >> (2 * bits - 8)));
        }
    }
}

uint8_t TextureSampler::extend_color_non_twiddled(uint8_t val, int bits) {
    // Extends by appending zeros at the LSBs.
    switch (bits) {
        case 1: return val ? 0xFF : 0x00;
        case 4: return static_cast<uint8_t>(val << 4);
        case 5: return static_cast<uint8_t>(val << 3);
        case 6: return static_cast<uint8_t>(val << 2);
        case 8: return val;
        default: {
            int shift = 8 - bits;
            if (shift < 0 || shift > 7) return val;
            return static_cast<uint8_t>(val << shift);
        }
    }
}

// ============================================================================
// Pixel format decoders
// ============================================================================

Pixel TextureSampler::decode_argb1555(uint16_t raw) {
    uint8_t a = (raw >> 15) ? 0xFF : 0x00;
    uint8_t r = extend_color_twiddled((raw >> 10) & 0x1F, 5);
    uint8_t g = extend_color_twiddled((raw >> 5) & 0x1F, 5);
    uint8_t b = extend_color_twiddled(raw & 0x1F, 5);
    return Pixel::from_argb(a, r, g, b);
}

Pixel TextureSampler::decode_rgb565(uint16_t raw) {
    uint8_t a = 0xFF;
    uint8_t r = extend_color_twiddled((raw >> 11) & 0x1F, 5);
    uint8_t g = extend_color_twiddled((raw >> 5) & 0x3F, 6);
    uint8_t b = extend_color_twiddled(raw & 0x1F, 5);
    return Pixel::from_argb(a, r, g, b);
}

Pixel TextureSampler::decode_argb4444(uint16_t raw) {
    uint8_t a = extend_color_twiddled((raw >> 12) & 0xF, 4);
    uint8_t r = extend_color_twiddled((raw >> 8) & 0xF, 4);
    uint8_t g = extend_color_twiddled((raw >> 4) & 0xF, 4);
    uint8_t b = extend_color_twiddled(raw & 0xF, 4);
    return Pixel::from_argb(a, r, g, b);
}

Pixel TextureSampler::decode_yuv422(uint8_t y, uint8_t u, uint8_t v) {
    // YUV to RGB conversion per PVR spec:
    //   R = clamp(Y + (11/8)*(V-128), 0, 255)
    //   G = clamp(Y - 0.25*(11/8)*(U-128) - 0.5*(11/8)*(V-128), 0, 255)
    //   B = clamp(Y + 1.25*(11/8)*(U-128), 0, 255)
    float yf = static_cast<float>(y);
    float uf = static_cast<float>(u);
    float vf = static_cast<float>(v);

    constexpr float c = 11.0f / 8.0f;  // 1.375

    float rf = yf + c * (vf - 128.0f);
    float gf = yf - 0.25f * c * (uf - 128.0f) - 0.5f * c * (vf - 128.0f);
    float bf = yf + 1.25f * c * (uf - 128.0f);

    uint8_t r = static_cast<uint8_t>(std::clamp(rf + 0.5f, 0.0f, 255.0f));
    uint8_t g = static_cast<uint8_t>(std::clamp(gf + 0.5f, 0.0f, 255.0f));
    uint8_t b = static_cast<uint8_t>(std::clamp(bf + 0.5f, 0.0f, 255.0f));

    return Pixel::from_argb(0xFF, r, g, b);
}

// ============================================================================
// Palette decoders
// ============================================================================

Pixel TextureSampler::decode_palette_4bpp(uint8_t index, uint32_t palette_selector) const {
    // For 4BPP: The 4-bit texture data combined with the 6-bit palette
    // selector (bits 26-21 of TCW) gives a 10-bit address into palette RAM.
    //   palette_index = (palette_selector << 4) | (index & 0xF)
    uint32_t palette_index = (static_cast<uint32_t>(palette_selector & 0x3F) << 4) |
                             (index & 0xF);

    uint32_t pal_entry = regs_->palette_ram(palette_index & 0x3FF);
    uint32_t pal_fmt = regs_->palette_format();

    switch (pal_fmt) {
        case 0:  return decode_argb1555(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 1:  return decode_rgb565(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 2:  return decode_argb4444(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 3:  return Pixel::from_packed(pal_entry);  // ARGB8888
        default: return decode_argb1555(static_cast<uint16_t>(pal_entry & 0xFFFF));
    }
}

Pixel TextureSampler::decode_palette_8bpp(uint8_t index, uint32_t palette_selector) const {
    // For 8BPP: The 8-bit texture data combined with 2-bit palette selector
    // (bits 26-25 of TCW, i.e. bits 5-4 of palette_selector field) gives a
    // 10-bit address into palette RAM.
    //   palette_index = (pal_sel_2bit << 8) | index
    uint32_t pal_sel_2bit = (palette_selector >> 4) & 0x3;
    uint32_t palette_index = (pal_sel_2bit << 8) | index;

    uint32_t pal_entry = regs_->palette_ram(palette_index & 0x3FF);
    uint32_t pal_fmt = regs_->palette_format();

    switch (pal_fmt) {
        case 0:  return decode_argb1555(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 1:  return decode_rgb565(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 2:  return decode_argb4444(static_cast<uint16_t>(pal_entry & 0xFFFF));
        case 3:  return Pixel::from_packed(pal_entry);  // ARGB8888
        default: return decode_argb1555(static_cast<uint16_t>(pal_entry & 0xFFFF));
    }
}

// ============================================================================
// Twiddle address calculation
// Spread bits: insert a zero between every bit of the 10-bit input.
// e.g. input bits b9..b0 -> output 0b9 0b8 ... 0b1 0b0 (at even positions).
// This is the standard Morton-code 'magic bits' technique.
static inline uint32_t spread_bits(uint32_t x) {
    x &= 0x000003FFu;                        // mask to 10 bits (max PVR coord)
    x = (x | (x << 8u)) & 0x00FF00FFu;
    x = (x | (x << 4u)) & 0x0F0F0F0Fu;
    x = (x | (x << 2u)) & 0x33333333u;
    x = (x | (x << 1u)) & 0x55555555u;
    return x;
}

uint32_t TextureSampler::twiddle_address(int u, int v, int size_u, int size_v) {
    if (size_u == size_v) {
        // Square texture: standard 2D Morton code.
        // V bits at even positions, U bits at odd positions.
        return spread_bits(static_cast<uint32_t>(v))
             | (spread_bits(static_cast<uint32_t>(u)) << 1u);
    }

    // Rectangular texture: interleave min_bits of each coordinate, then
    // append the remaining upper bits of the larger dimension linearly.
    const int min_size = std::min(size_u, size_v);
    const int max_size = std::max(size_u, size_v);

    int min_bits = 0;
    for (int s = min_size; s > 1; s >>= 1) min_bits++;

    // Mask each coordinate to the interleaved part
    const uint32_t mask  = static_cast<uint32_t>(min_size - 1);
    const uint32_t mu    = static_cast<uint32_t>(u) & mask;
    const uint32_t mv    = static_cast<uint32_t>(v) & mask;

    // Interleaved 2*min_bits bits
    uint32_t address = spread_bits(mv) | (spread_bits(mu) << 1u);
    address &= (1u << (2 * min_bits)) - 1u;

    // Append upper bits from the larger dimension at positions [2*min_bits ...]
    const uint32_t extra = static_cast<uint32_t>(size_u > size_v ? u : v)
                           >> min_bits;
    address |= (extra & static_cast<uint32_t>(max_size / min_size - 1))
               << (2 * min_bits);

    return address;
}

// ============================================================================
// Fetch a single texel from VRAM at integer coordinates
// ============================================================================

Pixel TextureSampler::fetch_texel(const TextureControlWord& tcw,
                                   int u_size, int v_size,
                                   int u_int, int v_int) const {
    // Delegate to VQ path if the texture is VQ compressed
    if (tcw.vq_compressed) {
        return fetch_vq_texel(tcw, u_size, v_size, u_int, v_int);
    }

    uint32_t base_addr = tcw.byte_address();
    TexturePixelFormat fmt = tcw.pixel_format;

    // Mipmapped non-VQ twiddled textures store their LOD pyramid
    // starting at the texture base. The offsets below (matching
    // flycast's OtherMipPoint table) are in *texels*, to be scaled by
    // bits-per-pixel later. The base (largest) LOD is at the end of
    // the pyramid. Non-twiddled / stride textures cannot be mipmapped,
    // so we only handle the twiddled case here.
    uint32_t mip_texel_offset = 0;
    if (tcw.mip_mapped && !tcw.scan_order) {
        static const uint32_t other_mip_offset[11] = {
            0x00003, // 1x1
            0x00004, // 2x2
            0x00008, // 4x4
            0x00018, // 8x8
            0x00058, // 16x16
            0x00158, // 32x32
            0x00558, // 64x64
            0x01558, // 128x128
            0x05558, // 256x256
            0x15558, // 512x512
            0x55558, // 1024x1024
        };
        int largest = std::max(u_size, v_size);
        int lod = 0;
        for (int s = largest; s > 1; s >>= 1) lod++;
        if (lod > 10) lod = 10;
        mip_texel_offset = other_mip_offset[lod];
    }

    if (!tcw.scan_order) {
        // -------------------------------------------------------------------
        // Twiddled format (scan_order == 0)
        // -------------------------------------------------------------------
        uint32_t tw_addr = twiddle_address(u_int, v_int, u_size, v_size) +
                           mip_texel_offset;

        switch (fmt) {
            case TexturePixelFormat::ARGB1555:
            case TexturePixelFormat::Reserved7: {
                uint16_t raw = read_vram16(base_addr + tw_addr * 2);
                return decode_argb1555(raw);
            }
            case TexturePixelFormat::RGB565: {
                uint16_t raw = read_vram16(base_addr + tw_addr * 2);
                return decode_rgb565(raw);
            }
            case TexturePixelFormat::ARGB4444: {
                uint16_t raw = read_vram16(base_addr + tw_addr * 2);
                return decode_argb4444(raw);
            }
            case TexturePixelFormat::YUV422: {
                // YUV422: each pair of texels shares U and V components.
                // Twiddle addressing operates on macro-pixel pairs (half u_size).
                int pair_u = u_int >> 1;
                uint32_t pair_tw = twiddle_address(pair_u, v_int,
                                                   std::max(1, u_size / 2), v_size);
                // Each macro-pixel is 4 bytes: [U, Y0, V, Y1]
                uint32_t yuv_data = read_vram32(base_addr + pair_tw * 4);
                uint8_t u_val = static_cast<uint8_t>((yuv_data >>  0) & 0xFF);
                uint8_t y0    = static_cast<uint8_t>((yuv_data >>  8) & 0xFF);
                uint8_t v_val = static_cast<uint8_t>((yuv_data >> 16) & 0xFF);
                uint8_t y1    = static_cast<uint8_t>((yuv_data >> 24) & 0xFF);
                uint8_t y     = (u_int & 1) ? y1 : y0;
                return decode_yuv422(y, u_val, v_val);
            }
            case TexturePixelFormat::BumpMap: {
                // BumpMap: 16-bit per texel containing S and R normal values.
                // Return S in the R channel and R in the G channel so the CORE
                // can compute final brightness using K1K2K3Q parameters.
                uint16_t raw = read_vram16(base_addr + tw_addr * 2);
                uint8_t s_val = static_cast<uint8_t>((raw >> 8) & 0xFF);
                uint8_t r_val = static_cast<uint8_t>(raw & 0xFF);
                return Pixel::from_argb(0xFF, s_val, r_val, 0xFF);
            }
            case TexturePixelFormat::Palette4BPP: {
                // 4 bits per texel
                uint32_t byte_offset = tw_addr / 2;
                uint8_t raw_byte = read_vram8(base_addr + byte_offset);
                uint8_t index;
                if (tw_addr & 1) {
                    index = (raw_byte >> 4) & 0xF;
                } else {
                    index = raw_byte & 0xF;
                }
                return decode_palette_4bpp(index, tcw.palette_selector);
            }
            case TexturePixelFormat::Palette8BPP: {
                // 8 bits per texel
                uint8_t index = read_vram8(base_addr + tw_addr);
                return decode_palette_8bpp(index, tcw.palette_selector);
            }
        }
    } else {
        // -------------------------------------------------------------------
        // Non-Twiddled / Stride format (scan_order == 1)
        // Linear address: v * u_size + u
        // -------------------------------------------------------------------
        // pcsx_rearmed: stride_select (TCW bit 25) makes the row pitch
        // TEXT_CONTROL.stride * 32 texels instead of u_size.  bloom reads
        // the 640-wide frame buffer back this way with u_size 1024.
        uint32_t pitch = static_cast<uint32_t>(u_size);
        if (tcw.stride_select && regs_) {
            uint32_t st = regs_->text_stride() * 32;
            if (st) pitch = st;
        }
        uint32_t linear_addr = static_cast<uint32_t>(v_int) * pitch + u_int;

        switch (fmt) {
            case TexturePixelFormat::ARGB1555:
            case TexturePixelFormat::Reserved7: {
                uint16_t raw = read_vram16(base_addr + linear_addr * 2);
                return decode_argb1555(raw);
            }
            case TexturePixelFormat::RGB565: {
                uint16_t raw = read_vram16(base_addr + linear_addr * 2);
                return decode_rgb565(raw);
            }
            case TexturePixelFormat::ARGB4444: {
                uint16_t raw = read_vram16(base_addr + linear_addr * 2);
                return decode_argb4444(raw);
            }
            case TexturePixelFormat::YUV422: {
                // Linear YUV422: pairs at half-width stride
                int pair_idx = u_int >> 1;
                uint32_t pair_linear = static_cast<uint32_t>(v_int) *
                                       std::max(1, u_size / 2) + pair_idx;
                uint32_t yuv_data = read_vram32(base_addr + pair_linear * 4);
                uint8_t u_val = static_cast<uint8_t>((yuv_data >>  0) & 0xFF);
                uint8_t y0    = static_cast<uint8_t>((yuv_data >>  8) & 0xFF);
                uint8_t v_val = static_cast<uint8_t>((yuv_data >> 16) & 0xFF);
                uint8_t y1    = static_cast<uint8_t>((yuv_data >> 24) & 0xFF);
                uint8_t y     = (u_int & 1) ? y1 : y0;
                return decode_yuv422(y, u_val, v_val);
            }
            case TexturePixelFormat::BumpMap: {
                uint16_t raw = read_vram16(base_addr + linear_addr * 2);
                uint8_t s_val = static_cast<uint8_t>((raw >> 8) & 0xFF);
                uint8_t r_val = static_cast<uint8_t>(raw & 0xFF);
                return Pixel::from_argb(0xFF, s_val, r_val, 0xFF);
            }
            case TexturePixelFormat::Palette4BPP: {
                uint32_t byte_offset = linear_addr / 2;
                uint8_t raw_byte = read_vram8(base_addr + byte_offset);
                uint8_t index;
                if (linear_addr & 1) {
                    index = (raw_byte >> 4) & 0xF;
                } else {
                    index = raw_byte & 0xF;
                }
                return decode_palette_4bpp(index, tcw.palette_selector);
            }
            case TexturePixelFormat::Palette8BPP: {
                uint8_t index = read_vram8(base_addr + linear_addr);
                return decode_palette_8bpp(index, tcw.palette_selector);
            }
        }
    }

    // Fallback: magenta pixel for unsupported / unreachable paths
    return Pixel::from_argb(0xFF, 0xFF, 0x00, 0xFF);
}

// ============================================================================
// VQ texture fetch
// ============================================================================

Pixel TextureSampler::fetch_vq_texel(const TextureControlWord& tcw,
                                      int u_size, int v_size,
                                      int u_int, int v_int) const {
    uint32_t base_addr = tcw.byte_address();

    // VQ code book: 256 entries x 8 bytes = 2048 bytes, stored at texture base.
    uint32_t codebook_addr = base_addr;
    uint32_t index_start   = base_addr + 256 * 8;

    // pcsx_rearmed: scan-order (non-twiddled) VQ.  Not in the docs, but the
    // hardware does it and bloom's renderer is built on it: the index array
    // is linear, (u_size / 4) bytes per row, one row per texel row, and the
    // four 16-bit texels of a code book entry are four consecutive texels
    // of that row.  (Verified against bloom/src/pvr.c load_block_4bpp /
    // load_block_8bpp, which lay rows out at exactly that pitch, and its
    // U scaling of 4 PVR texels per source texel.)
    if (tcw.scan_order && !tcw.is_palette()) {
        uint32_t pitch  = static_cast<uint32_t>(std::max(1, u_size / 4));
        uint32_t idx_a  = index_start + static_cast<uint32_t>(v_int) * pitch
                        + static_cast<uint32_t>(u_int >> 2);
        uint8_t  vq_idx = read_vram8(idx_a);
        uint64_t entry  = read_vram64(codebook_addr + static_cast<uint32_t>(vq_idx) * 8);
        uint16_t raw    = static_cast<uint16_t>((entry >> ((u_int & 3) * 16)) & 0xFFFF);
        switch (tcw.pixel_format) {
            case TexturePixelFormat::RGB565:   return decode_rgb565(raw);
            case TexturePixelFormat::ARGB4444: return decode_argb4444(raw);
            default:                           return decode_argb1555(raw);
        }
    }

    // -----------------------------------------------------------------------
    // Palette VQ textures use larger code book blocks than regular VQ:
    //
    //   8BPP palette VQ: 2 (U) x 4 (V) block, 8 bytes = 8 palette indices
    //                    index array is (U/2) x (V/4) twiddled bytes
    //
    //   4BPP palette VQ: 4 (U) x 4 (V) block, 8 bytes = 16 packed nibbles
    //                    index array is (U/4) x (V/4) twiddled bytes
    //
    // Texels within each entry are laid out in U-major order (all V values
    // for U=0 first, then all V values for U=1, etc.), matching the
    // ordering used by standard 2x2 RGB VQ entries.  Within each byte of a
    // 4BPP entry the low nibble holds the even-nibble-index texel and the
    // high nibble holds the odd-nibble-index texel, consistent with the
    // non-VQ 4BPP twiddled storage convention.
    //
    // Mipmap offsets for palette VQ are not specified by the spec and are
    // uncommon in practice; the mip_mapped flag is therefore ignored for
    // palette VQ (index_start stays at base + 2048).
    // -----------------------------------------------------------------------

    if (tcw.pixel_format == TexturePixelFormat::Palette8BPP) {
        // 8BPP palette: 2(U) x 4(V) block
        int sub_u    = u_int & 1;
        int sub_v    = v_int & 3;
        int blk_u    = u_int >> 1;
        int blk_v    = v_int >> 2;
        int blk_u_sz = std::max(1, u_size >> 1);
        int blk_v_sz = std::max(1, v_size >> 2);

        uint32_t blk_tw  = twiddle_address(blk_u, blk_v, blk_u_sz, blk_v_sz);
        uint8_t  vq_idx  = read_vram8(index_start + blk_tw);
        uint64_t entry   = read_vram64(codebook_addr + static_cast<uint32_t>(vq_idx) * 8);

        // Byte index within the entry follows the same Morton-code (twiddle)
        // ordering used by all other palette texture paths.  The 2×4 sub-block
        // is indexed as twiddle_address(sub_u, sub_v, 2, 4), which for the
        // 8-byte entry gives bytes 0-7 in the order that matches the non-VQ
        // twiddled layout (verified against the reference load_twid packing).
        uint32_t byte_idx = twiddle_address(sub_u, sub_v, 2, 4);
        uint8_t pal_idx = static_cast<uint8_t>((entry >> (byte_idx * 8u)) & 0xFF);
        return decode_palette_8bpp(pal_idx, tcw.palette_selector);
    }

    if (tcw.pixel_format == TexturePixelFormat::Palette4BPP) {
        // 4BPP palette: 4(U) x 4(V) block, 16 nibbles packed into 8 bytes
        int sub_u    = u_int & 3;
        int sub_v    = v_int & 3;
        int blk_u    = u_int >> 2;
        int blk_v    = v_int >> 2;
        int blk_u_sz = std::max(1, u_size >> 2);
        int blk_v_sz = std::max(1, v_size >> 2);

        uint32_t blk_tw  = twiddle_address(blk_u, blk_v, blk_u_sz, blk_v_sz);
        uint8_t  vq_idx  = read_vram8(index_start + blk_tw);
        uint64_t entry   = read_vram64(codebook_addr + static_cast<uint32_t>(vq_idx) * 8);

        // Nibble index within the entry follows the same Morton-code ordering
        // used by all other palette texture paths.  The 4×4 sub-block is
        // indexed as twiddle_address(sub_u, sub_v, 4, 4).
        // Byte = nibble_idx / 2; even nibble -> low 4 bits, odd -> high 4 bits.
        uint32_t nibble_idx = twiddle_address(sub_u, sub_v, 4, 4);
        uint32_t shift = (nibble_idx >> 1u) * 8u + (nibble_idx & 1u) * 4u;
        uint8_t pal_idx = static_cast<uint8_t>((entry >> shift) & 0xF);
        return decode_palette_4bpp(pal_idx, tcw.palette_selector);
    }

    // -----------------------------------------------------------------------
    // Regular (non-palette) VQ: 2x2 texel blocks, four 16-bit texels per entry.
    // -----------------------------------------------------------------------

    // Mipmapped VQ textures store a pyramid of LODs immediately after
    // the codebook: 1x1, 2x2, 4x4, ..., up to the full texture size.
    // Each LOD is (w/2)*(h/2) bytes of VQ indices. The base (largest)
    // LOD is therefore offset from `index_start` by the sum of all
    // smaller LOD sizes. These offsets match flycast's VQMipPoint
    // table. Non-square mipmapped VQ is rare and handled by using the
    // larger dimension's mip levels as a conservative approximation.
    if (tcw.mip_mapped) {
        static const uint32_t vq_mip_offset[11] = {
            0x00000, // 1x1   (implicit, no indices)
            0x00001, // 2x2
            0x00002, // 4x4
            0x00006, // 8x8
            0x00016, // 16x16
            0x00056, // 32x32
            0x00156, // 64x64
            0x00556, // 128x128
            0x01556, // 256x256
            0x05556, // 512x512
            0x15556, // 1024x1024
        };
        int largest = std::max(u_size, v_size);
        int lod = 0;
        for (int s = largest; s > 1; s >>= 1) lod++;
        if (lod > 10) lod = 10;
        index_start += vq_mip_offset[lod];
    }

    // VQ operates on 2x2 texel blocks. The on-VRAM index array is
    // therefore (u_size/2) x (v_size/2) twiddled bytes. VQ textures
    // are usually square but rectangular ones are legal too, so we
    // have to carry both dimensions through to the twiddle helper.
    int block_u      = u_int >> 1;
    int block_v      = v_int >> 1;
    int block_u_size = std::max(1, u_size / 2);
    int block_v_size = std::max(1, v_size / 2);

    // Twiddled index into the block grid
    uint32_t block_tw = twiddle_address(block_u, block_v,
                                        block_u_size, block_v_size);

    // Read the 8-bit VQ index
    uint8_t vq_index = read_vram8(index_start + block_tw);

    // Read the 64-bit code book entry
    uint64_t entry = read_vram64(codebook_addr + static_cast<uint32_t>(vq_index) * 8);

    // The 4 texels are packed into 64 bits in U-major order:
    //   texel(0,0) = bits 15..0
    //   texel(0,1) = bits 31..16
    //   texel(1,0) = bits 47..32
    //   texel(1,1) = bits 63..48
    // Select by (u%2, v%2)
    int sub_u = u_int & 1;
    int sub_v = v_int & 1;
    int texel_idx = sub_u * 2 + sub_v;

    uint16_t raw = static_cast<uint16_t>((entry >> (texel_idx * 16)) & 0xFFFF);

    // Decode based on pixel format
    switch (tcw.pixel_format) {
        case TexturePixelFormat::ARGB1555:
        case TexturePixelFormat::Reserved7:
            return decode_argb1555(raw);
        case TexturePixelFormat::RGB565:
            return decode_rgb565(raw);
        case TexturePixelFormat::ARGB4444:
            return decode_argb4444(raw);
        case TexturePixelFormat::YUV422:
            // VQ YUV is uncommon; treat as ARGB1555 fallback
            return decode_argb1555(raw);
        case TexturePixelFormat::BumpMap:
        default:
            // VQ bump map is not defined; return a visible fallback
            return decode_argb1555(raw);
    }
}

// ============================================================================
// Bilinear filter
// ============================================================================

Pixel TextureSampler::bilinear_filter(const Pixel& p00, const Pixel& p10,
                                       const Pixel& p01, const Pixel& p11,
                                       float frac_u, float frac_v) {
    auto lerp_f = [](float a, float b, float t) -> float {
        return a + (b - a) * t;
    };

    float a = lerp_f(lerp_f(static_cast<float>(p00.a), static_cast<float>(p10.a), frac_u),
                     lerp_f(static_cast<float>(p01.a), static_cast<float>(p11.a), frac_u), frac_v);
    float r = lerp_f(lerp_f(static_cast<float>(p00.r), static_cast<float>(p10.r), frac_u),
                     lerp_f(static_cast<float>(p01.r), static_cast<float>(p11.r), frac_u), frac_v);
    float g = lerp_f(lerp_f(static_cast<float>(p00.g), static_cast<float>(p10.g), frac_u),
                     lerp_f(static_cast<float>(p01.g), static_cast<float>(p11.g), frac_u), frac_v);
    float b = lerp_f(lerp_f(static_cast<float>(p00.b), static_cast<float>(p10.b), frac_u),
                     lerp_f(static_cast<float>(p01.b), static_cast<float>(p11.b), frac_u), frac_v);

    return Pixel::from_argb(
        static_cast<uint8_t>(std::clamp(a + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(r + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(g + 0.5f, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(b + 0.5f, 0.0f, 255.0f))
    );
}

// ============================================================================
// UV addressing helper
//
// Applies flip / clamp / wrap to a scaled texture coordinate.
//   coord  – the UV coordinate already scaled by texture size  (u * width)
//   size   – texture dimension in texels
//   flip   – mirror on every other tile repeat
//   clamp  – clamp to [0, size-1]  (overrides flip when both are set)
//
// Returns the coordinate in [0, size) ready for texel lookup.
// ============================================================================

static float apply_uv_addressing(float coord, int size, bool flip, bool clamp_mode) {
    float fsize = static_cast<float>(size);

    if (clamp_mode) {
        // Clamp overrides flip.
        return std::clamp(coord, 0.0f, fsize - 1.0f);
    }

    if (flip) {
        // Mirror-repeat: if the integer tile index is odd, reverse direction.
        float tile     = std::floor(coord / fsize);
        float local    = coord - tile * fsize;
        if (local < 0.0f) local += fsize;
        int tile_int = static_cast<int>(tile);
        if (tile_int & 1) {
            // Mirror: reverse the position within the tile
            local = (fsize - 1.0f) - local;
            if (local < 0.0f) local = 0.0f;
        }
        return local;
    }

    // Default: wrap (repeat).
    float wrapped = std::fmod(coord, fsize);
    if (wrapped < 0.0f) wrapped += fsize;
    return wrapped;
}

// ============================================================================
// Texel edge margin for point sampling, 1/64 texel; PVR_EDGE=0 turns it off
static float edge_margin() {
    static float margin = -1.0f;
    if (margin < 0.0f) {
        const char* env = std::getenv("PVR_EDGE");
        margin = env ? static_cast<float>(std::atof(env)) : 1.0f / 64;
    }
    return margin;
}

// Main sampling entry point
// ============================================================================

Pixel TextureSampler::sample(const TSPInstructionWord& tsp,
                              const TextureControlWord& tcw,
                              float u, float v,
                              float lod,
                              bool texel_half_offset) const {
    if (!vram_ || vram_size_ == 0) {
        return Pixel::from_argb(0, 0, 0, 0);
    }

    int u_size = static_cast<int>(tsp.texture_u_pixels());
    int v_size = static_cast<int>(tsp.texture_v_pixels());

    // -----------------------------------------------------------------------
    // Mip level selection for mipmapped twiddled textures.
    //
    // The caller (rasterize_triangle) supplies a float LOD computed from the
    // triangle's affine UV derivatives.  We floor it to an integer level and
    // reduce the working texture dimensions accordingly.  fetch_texel() then
    // derives the correct VRAM offset for that level automatically by looking
    // up other_mip_offset[log2(reduced_size)].
    //
    // Non-twiddled (scan_order == 1) textures cannot be mipmapped.
    // VQ-compressed mipmapped textures are handled identically: fetch_texel
    // (and its VQ delegate) both derive the offset from the current u/v sizes.
    //
    // TrilinearPassB samples one level coarser than PassA so that the game's
    // blend-function weighting produces the correct cross-fade between them.
    // -----------------------------------------------------------------------
    if (tcw.mip_mapped && !tcw.scan_order && lod > 0.0f) {
        int lod_level = static_cast<int>(lod); // caller already floors

        // Tri-linear pass B → one level coarser than pass A.
        if (tsp.filter_mode == FilterMode::TrilinearPassB) {
            lod_level += 1;
        }

        // Clamp to the highest valid level (1×1 texel).
        int max_lod = 0;
        for (int s = std::max(u_size, v_size); s > 1; s >>= 1) max_lod++;
        lod_level = std::min(lod_level, max_lod);

        // Reduce the working dimensions; fetch_texel uses these to select the
        // correct offset within the mip pyramid.
        u_size = std::max(1, u_size >> lod_level);
        v_size = std::max(1, v_size >> lod_level);
    }

    // 1. Scale UV from [0,1] (or beyond) into texel coordinates.
    float u_scaled = u * static_cast<float>(u_size);
    float v_scaled = v * static_cast<float>(v_size);

    // 2. Apply flip / clamp / wrap addressing.
    u_scaled = apply_uv_addressing(u_scaled, u_size, tsp.flip_u, tsp.clamp_u);
    v_scaled = apply_uv_addressing(v_scaled, v_size, tsp.flip_v, tsp.clamp_v);

    FilterMode filter = tsp.filter_mode;

    if (filter == FilterMode::PointSampled) {
        // -----------------------------------------------------------------
        // Point sampling: nearest texel
        // -----------------------------------------------------------------
        int ui = static_cast<int>(std::floor(u_scaled));
        int vi = static_cast<int>(std::floor(v_scaled));
        // Real hardware's texture planes lose a little precision (setup is
        // in absolute screen coordinates), so a sample within a hair of a
        // texel edge can land on either side: take the worse, other side
        const float margin = edge_margin();
        if (margin > 0.0f) {
            float fu = u_scaled - ui, fv = v_scaled - vi;
            if (fu < margin) ui = (ui - 1 + u_size) % u_size;
            else if (fu > 1.0f - margin) ui = (ui + 1) % u_size;
            if (fv < margin) vi = (vi - 1 + v_size) % v_size;
            else if (fv > 1.0f - margin) vi = (vi + 1) % v_size;
        }
        ui = std::clamp(ui, 0, u_size - 1);
        vi = std::clamp(vi, 0, v_size - 1);
        return fetch_texel(tcw, u_size, v_size, ui, vi);
    }

    // -----------------------------------------------------------------
    // Bilinear filter (and trilinear passes, which select different mip
    // levels via the lod parameter above but still bilinear-filter within
    // that level).
    //
    // TSP texel sampling position (HALF_OFFSET bit 2):
    //   texel_half_offset = true  → centre the 2×2 neighbourhood by -0.5
    //   texel_half_offset = false → top-left corner of neighbourhood (no bias)
    // -----------------------------------------------------------------
    const float half_off = texel_half_offset ? 0.5f : 0.0f;
    float uf = u_scaled - half_off;
    float vf = v_scaled - half_off;

    int u0 = static_cast<int>(std::floor(uf));
    int v0 = static_cast<int>(std::floor(vf));
    int u1 = u0 + 1;
    int v1 = v0 + 1;

    float frac_u = uf - std::floor(uf);
    float frac_v = vf - std::floor(vf);

    // Resolve a bilinear neighbour coordinate to a texel index honouring
    // clamp / mirror-repeat / wrap.  Bilinear neighbours are at most one
    // texel outside [0, size) so tile ∈ {-1, 0, 1}.
    auto addr_coord = [](int c, int size, bool flip, bool clamp) -> int {
        if (clamp) {
            return std::clamp(c, 0, size - 1);
        }
        if (flip) {
            int tile  = (c < 0) ? -1 : (c < size ? 0 : 1);
            int local = c - tile * size;
            if (tile & 1) local = (size - 1) - local;
            return local;
        }
        int w = c % size;
        if (w < 0) w += size;
        return w;
    };

    int u0c = addr_coord(u0, u_size, tsp.flip_u, tsp.clamp_u);
    int u1c = addr_coord(u1, u_size, tsp.flip_u, tsp.clamp_u);
    int v0c = addr_coord(v0, v_size, tsp.flip_v, tsp.clamp_v);
    int v1c = addr_coord(v1, v_size, tsp.flip_v, tsp.clamp_v);

    Pixel p00 = fetch_texel(tcw, u_size, v_size, u0c, v0c);
    Pixel p10 = fetch_texel(tcw, u_size, v_size, u1c, v0c);
    Pixel p01 = fetch_texel(tcw, u_size, v_size, u0c, v1c);
    Pixel p11 = fetch_texel(tcw, u_size, v_size, u1c, v1c);

    return bilinear_filter(p00, p10, p01, p11, frac_u, frac_v);
}

} // namespace pvr
