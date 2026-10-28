#pragma once

#include <cstdint>
#include <array>
#include <cstring>

namespace pvr {

// PVR register offsets from base address 0x005F8000.
// Values are byte offsets; the underlying storage is indexed by offset >> 2.
enum class RegisterName : uint32_t {
    ID                  = 0x000,
    REVISION            = 0x004,
    SOFTRESET           = 0x008,
    STARTRENDER         = 0x014,
    TEST_SELECT         = 0x018,
    PARAM_BASE          = 0x020,
    REGION_BASE         = 0x02C,
    SPAN_SORT_CFG       = 0x030,
    VO_BORDER_COL       = 0x040,
    FB_R_CTRL           = 0x044,
    FB_W_CTRL           = 0x048,
    FB_W_LINESTRIDE     = 0x04C,
    FB_R_SOF1           = 0x050,
    FB_R_SOF2           = 0x054,
    FB_R_SIZE           = 0x05C,
    FB_W_SOF1           = 0x060,
    FB_W_SOF2           = 0x064,
    FB_X_CLIP           = 0x068,
    FB_Y_CLIP           = 0x06C,
    FPU_SHAD_SCALE      = 0x074,
    FPU_CULL_VAL        = 0x078,
    FPU_PARAM_CFG       = 0x07C,
    HALF_OFFSET         = 0x080,
    FPU_PERP_VAL        = 0x084,
    ISP_BACKGND_D       = 0x088,
    ISP_BACKGND_T       = 0x08C,
    ISP_FEED_CFG        = 0x098,
    SDRAM_REFRESH       = 0x0A0,
    SDRAM_ARB_CFG       = 0x0A4,
    SDRAM_CFG           = 0x0A8,
    FOG_COL_RAM         = 0x0B0,
    FOG_COL_VERT        = 0x0B4,
    FOG_DENSITY         = 0x0B8,
    FOG_CLAMP_MAX       = 0x0BC,
    FOG_CLAMP_MIN       = 0x0C0,
    SPG_TRIGGER_POS     = 0x0C4,
    SPG_HBLANK_INT      = 0x0C8,
    SPG_VBLANK_INT      = 0x0CC,
    SPG_CONTROL         = 0x0D0,
    SPG_HBLANK          = 0x0D4,
    SPG_LOAD            = 0x0D8,
    SPG_VBLANK          = 0x0DC,
    SPG_WIDTH           = 0x0E0,
    TEXT_CONTROL        = 0x0E4,
    VO_CONTROL          = 0x0E8,
    VO_STARTX           = 0x0EC,
    VO_STARTY           = 0x0F0,
    SCALER_CTL          = 0x0F4,
    PAL_RAM_CTRL        = 0x108,
    SPG_STATUS          = 0x10C,
    FB_BURSTCTRL        = 0x110,
    FB_C_SOF            = 0x114,
    Y_COEFF             = 0x118,
    PT_ALPHA_REF        = 0x11C,
    TA_OL_BASE          = 0x124,
    TA_ISP_BASE         = 0x128,
    TA_OL_LIMIT         = 0x12C,
    TA_ISP_LIMIT        = 0x130,
    TA_NEXT_OPB         = 0x134,
    TA_ITP_CURRENT      = 0x138,
    TA_GLOB_TILE_CLIP   = 0x13C,
    TA_ALLOC_CTRL       = 0x140,
    TA_LIST_INIT        = 0x144,
    TA_YUV_TEX_BASE     = 0x148,
    TA_YUV_TEX_CTRL     = 0x14C,
    TA_YUV_TEX_CNT      = 0x150,
    TA_LIST_CONT        = 0x160,
    TA_NEXT_OPB_INIT    = 0x164,
    FOG_TABLE           = 0x200,   // 128 entries, 0x200..0x3FC
    PALETTE_RAM         = 0x1000,  // 1024 entries, 0x1000..0x1FFC
};

// Base address of the PVR register block in the Dreamcast address space.
static constexpr uint32_t PVR_REG_BASE = 0x005F8000;

// Total size of the register space in bytes (0x005F8000 - 0x005F9FFC inclusive).
static constexpr uint32_t PVR_REG_SPACE_BYTES = 0x2000;

// Total number of uint32_t entries in the register space.
static constexpr uint32_t PVR_REG_SPACE_WORDS = PVR_REG_SPACE_BYTES / sizeof(uint32_t);

// HOLLY2 device identification constants.
static constexpr uint32_t HOLLY2_DEVICE_ID = 0x17FD11DB;
static constexpr uint32_t HOLLY2_REVISION  = 0x0011;

class RegisterBank {
public:
    RegisterBank() {
        std::memset(regs_.data(), 0, sizeof(regs_));
        write(RegisterName::ID, HOLLY2_DEVICE_ID);
        write(RegisterName::REVISION, HOLLY2_REVISION);
    }

    // ---------------------------------------------------------------
    // Core read / write by RegisterName
    // ---------------------------------------------------------------

    uint32_t read(RegisterName name) const {
        return regs_[static_cast<uint32_t>(name) >> 2];
    }

    void write(RegisterName name, uint32_t value) {
        regs_[static_cast<uint32_t>(name) >> 2] = value;
    }

    // ---------------------------------------------------------------
    // Core read / write by raw byte offset (from 0x005F8000)
    // ---------------------------------------------------------------

    uint32_t read(uint32_t offset) const {
        return regs_[offset >> 2];
    }

    void write(uint32_t offset, uint32_t value) {
        regs_[offset >> 2] = value;
    }

    // ---------------------------------------------------------------
    // FOG_TABLE helpers  (128 entries, index 0..127)
    // ---------------------------------------------------------------

    uint32_t fog_table(int index) const {
        return regs_[(static_cast<uint32_t>(RegisterName::FOG_TABLE) >> 2) + index];
    }

    // ---------------------------------------------------------------
    // PALETTE_RAM helpers  (1024 entries, index 0..1023)
    // ---------------------------------------------------------------

    uint32_t palette_ram(int index) const {
        return regs_[(static_cast<uint32_t>(RegisterName::PALETTE_RAM) >> 2) + index];
    }

    // ---------------------------------------------------------------
    // TA_GLOB_TILE_CLIP field helpers
    //   bits  5: 0  Tile_X_Num (X coordinate of lower-right tile)
    //   bits 21:16  Tile_Y_Num (Y coordinate of lower-right tile)
    // ---------------------------------------------------------------

    uint32_t tile_x_num() const {
        return read(RegisterName::TA_GLOB_TILE_CLIP) & 0x3F;
    }

    uint32_t tile_y_num() const {
        return (read(RegisterName::TA_GLOB_TILE_CLIP) >> 16) & 0x3F;
    }

    // ---------------------------------------------------------------
    // TA_ALLOC_CTRL field helpers
    //   bits  1: 0  Opaque OPB size
    //   bits  5: 4  Opaque Modifier Volume OPB size
    //   bits  9: 8  Translucent OPB size
    //   bits 13:12  Translucent Modifier Volume OPB size
    //   bits 17:16  Punch Through list OPB size
    //   bit  20     OPB_Mode (0=increasing, 1=decreasing)
    //
    // OPB size encoding: 0 = no list, 1 = 8 ptrs, 2 = 16 ptrs, 3 = 32 ptrs
    // ---------------------------------------------------------------

    bool opb_mode() const {
        return (read(RegisterName::TA_ALLOC_CTRL) >> 20) & 1;
    }

    uint32_t opaque_opb_size() const {
        return decode_opb_size((read(RegisterName::TA_ALLOC_CTRL) >> 0) & 0x3);
    }

    uint32_t opaque_modifier_opb_size() const {
        return decode_opb_size((read(RegisterName::TA_ALLOC_CTRL) >> 4) & 0x3);
    }

    uint32_t translucent_opb_size() const {
        return decode_opb_size((read(RegisterName::TA_ALLOC_CTRL) >> 8) & 0x3);
    }

    uint32_t translucent_modifier_opb_size() const {
        return decode_opb_size((read(RegisterName::TA_ALLOC_CTRL) >> 12) & 0x3);
    }

    uint32_t punch_through_opb_size() const {
        return decode_opb_size((read(RegisterName::TA_ALLOC_CTRL) >> 16) & 0x3);
    }

    // ---------------------------------------------------------------
    // FB_W_CTRL field helpers
    //   bits  2: 0  fb_packmode
    //                 0=0555KRGB  1=565RGB    2=4444ARGB  3=1555ARGB
    //                 4=888RGB    5=0888KRGB  6=8888ARGB
    //   bit  3      fb_dither
    //   bits 15: 8  fb_kval
    //   bits 23:16  fb_alpha_threshold
    // ---------------------------------------------------------------

    uint32_t fb_packmode() const {
        return read(RegisterName::FB_W_CTRL) & 0x7;
    }

    bool fb_dither() const {
        return (read(RegisterName::FB_W_CTRL) >> 3) & 1;
    }

    uint32_t fb_kval() const {
        return (read(RegisterName::FB_W_CTRL) >> 8) & 0xFF;
    }

    uint32_t fb_alpha_threshold() const {
        return (read(RegisterName::FB_W_CTRL) >> 16) & 0xFF;
    }

    // ---------------------------------------------------------------
    // FB_R_CTRL field helpers
    //   bits  1: 0  fb_depth (pixel format)
    //   bit  2      fb_concat
    //   bit  3      fb_line_double
    //   bit  8      fb_enable
    //   bit  9      fb_strip_buf_en
    //   bits 12:10  fb_stripsize
    //   bits 23:16  fb_chroma_threshold
    // ---------------------------------------------------------------

    uint32_t fb_depth() const {
        return read(RegisterName::FB_R_CTRL) & 0x3;
    }

    bool fb_concat() const {
        return (read(RegisterName::FB_R_CTRL) >> 2) & 1;
    }

    bool fb_line_double() const {
        return (read(RegisterName::FB_R_CTRL) >> 3) & 1;
    }

    bool fb_enable() const {
        return (read(RegisterName::FB_R_CTRL) >> 8) & 1;
    }

    bool fb_strip_buf_en() const {
        return (read(RegisterName::FB_R_CTRL) >> 9) & 1;
    }

    uint32_t fb_stripsize() const {
        return (read(RegisterName::FB_R_CTRL) >> 10) & 0x7;
    }

    uint32_t fb_chroma_threshold() const {
        return (read(RegisterName::FB_R_CTRL) >> 16) & 0xFF;
    }

    // ---------------------------------------------------------------
    // FB_R_SIZE field helpers
    //   bits  9: 0  fb_x_size
    //   bits 19:10  fb_y_size
    //   bits 29:20  fb_modulus
    // ---------------------------------------------------------------

    uint32_t fb_x_size() const {
        return read(RegisterName::FB_R_SIZE) & 0x3FF;
    }

    uint32_t fb_y_size() const {
        return (read(RegisterName::FB_R_SIZE) >> 10) & 0x3FF;
    }

    uint32_t fb_modulus() const {
        return (read(RegisterName::FB_R_SIZE) >> 20) & 0x3FF;
    }

    // ---------------------------------------------------------------
    // FB_X_CLIP field helpers
    //   bits 10: 0  x_min
    //   bits 26:16  x_max
    // ---------------------------------------------------------------

    uint32_t fb_x_clip_min() const {
        return read(RegisterName::FB_X_CLIP) & 0x7FF;
    }

    uint32_t fb_x_clip_max() const {
        return (read(RegisterName::FB_X_CLIP) >> 16) & 0x7FF;
    }

    // ---------------------------------------------------------------
    // FB_Y_CLIP field helpers
    //   bits  9: 0  y_min
    //   bits 25:16  y_max
    // ---------------------------------------------------------------

    uint32_t fb_y_clip_min() const {
        return read(RegisterName::FB_Y_CLIP) & 0x3FF;
    }

    uint32_t fb_y_clip_max() const {
        return (read(RegisterName::FB_Y_CLIP) >> 16) & 0x3FF;
    }

    // ---------------------------------------------------------------
    // ISP_BACKGND_T field helpers
    //   bits  2: 0  tag_offset
    //   bits 23: 3  tag_address   (21-bit 32-bit word address, added to PARAM_BASE)
    //   bits 26:24  skip
    //   bit     27  shadow
    //   bit     28  cache_bypass
    // ---------------------------------------------------------------

    uint32_t isp_backgnd_tag_offset() const {
        return read(RegisterName::ISP_BACKGND_T) & 0x7;
    }

    uint32_t isp_backgnd_tag_address() const {
        return (read(RegisterName::ISP_BACKGND_T) >> 3) & 0x1FFFFF;
    }

    uint32_t isp_backgnd_skip() const {
        return (read(RegisterName::ISP_BACKGND_T) >> 24) & 0x7;
    }

    bool isp_backgnd_shadow() const {
        return (read(RegisterName::ISP_BACKGND_T) >> 27) & 0x1;
    }

    bool isp_backgnd_cache_bypass() const {
        return (read(RegisterName::ISP_BACKGND_T) >> 28) & 0x1;
    }

    // ---------------------------------------------------------------
    // ISP_FEED_CFG field helpers
    //   bits  9: 0  cache_size
    //   bits 21:16  punch_through_chunk_size
    //   bit  24     discard_mode
    // ---------------------------------------------------------------

    uint32_t isp_cache_size() const {
        return read(RegisterName::ISP_FEED_CFG) & 0x3FF;
    }

    uint32_t isp_punch_through_chunk_size() const {
        return (read(RegisterName::ISP_FEED_CFG) >> 16) & 0x3F;
    }

    bool isp_discard_mode() const {
        return (read(RegisterName::ISP_FEED_CFG) >> 24) & 1;
    }

    // ---------------------------------------------------------------
    // SCALER_CTL field helpers
    //   bits 15: 0  vscalefactor (vertical scale factor)
    //   bit  16     hscale (horizontal scaling: 0=full, 1=half)
    // ---------------------------------------------------------------

    uint32_t scaler_vscalefactor() const {
        return read(RegisterName::SCALER_CTL) & 0xFFFF;
    }

    bool scaler_hscale() const {
        return (read(RegisterName::SCALER_CTL) >> 16) & 1;
    }

    // ---------------------------------------------------------------
    // TEXT_CONTROL field helpers
    //   bits 4:0  stride (texture stride value)
    // ---------------------------------------------------------------

    uint32_t text_stride() const {
        return read(RegisterName::TEXT_CONTROL) & 0x1F;
    }

    // ---------------------------------------------------------------
    // PAL_RAM_CTRL field helpers
    //   bits 1:0  palette_format
    //              0=ARGB1555  1=RGB565  2=ARGB4444  3=ARGB8888
    // ---------------------------------------------------------------

    uint32_t palette_format() const {
        return read(RegisterName::PAL_RAM_CTRL) & 0x3;
    }

    // ---------------------------------------------------------------
    // FPU_PARAM_CFG field helpers
    //   bit 21  region_header_type
    //            0 = Type 1 region header
    //            1 = Type 2 region header
    // ---------------------------------------------------------------

    bool region_header_type() const {
        return (read(RegisterName::FPU_PARAM_CFG) >> 21) & 1;
    }

    // ---------------------------------------------------------------
    // HALF_OFFSET field helpers (0x005F8080)
    //   bit 2  TSP texel sampling position  (0 = (0,0), 1 = (0.5,0.5)  default=1)
    //   bit 1  TSP pixel sampling position  (0 = (0,0), 1 = (0.5,0.5)  default=1)
    //   bit 0  FPU pixel sampling position  (0 = (0,0), 1 = (0.5,0.5)  default=1)
    // Normally all three fields are set to 1 (the spec-recommended default).
    // ---------------------------------------------------------------

    /// True when the TSP texel neighbourhood should be centred at (0.5, 0.5)
    /// relative to each texel; controls the -0.5 bias in bilinear filtering.
    bool half_offset_texel() const {
        return (read(RegisterName::HALF_OFFSET) >> 2) & 1;
    }

    /// True when the TSP pixel sample point is at pixel centre (0.5, 0.5).
    /// Controls whether rasterised pixel coordinates include a +0.5 offset.
    bool half_offset_tsp_pixel() const {
        return (read(RegisterName::HALF_OFFSET) >> 1) & 1;
    }

    /// True when the FPU depth-comparison sample point is at pixel centre.
    bool half_offset_fpu_pixel() const {
        return (read(RegisterName::HALF_OFFSET) >> 0) & 1;
    }

    // ---------------------------------------------------------------
    // Raw access to the underlying storage
    // ---------------------------------------------------------------

    const std::array<uint32_t, PVR_REG_SPACE_WORDS>& data() const { return regs_; }
    std::array<uint32_t, PVR_REG_SPACE_WORDS>& data() { return regs_; }

private:
    std::array<uint32_t, PVR_REG_SPACE_WORDS> regs_;

    // Decode the 2-bit OPB size field to actual number of pointers.
    static uint32_t decode_opb_size(uint32_t code) {
        switch (code) {
            case 0: return 0;   // No list
            case 1: return 8;   // 8 pointers
            case 2: return 16;  // 16 pointers
            case 3: return 32;  // 32 pointers
            default: return 0;
        }
    }
};

} // namespace pvr
