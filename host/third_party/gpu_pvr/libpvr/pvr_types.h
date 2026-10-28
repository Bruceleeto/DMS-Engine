#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>

namespace pvr {

// ============================================================================
// Constants
// ============================================================================

static constexpr uint32_t TILE_SIZE = 32;
// The documented hardware maximum is 40×15 (1280×480), but the BIOS
// sets TA_TILESIZE to 64×15 and the original lxdream TA handled this
// because it wrote directly to VRAM. We match that behaviour by sizing
// our arrays to the register's maximum (6-bit X field, 4-bit Y field).
static constexpr uint32_t MAX_TILES_X = 64;
static constexpr uint32_t MAX_TILES_Y = 16;

// ============================================================================
// Enumerations
// ============================================================================

// Polygon list types (HOLLY2 supports all 5)
enum class ListType : uint8_t {
    Opaque                    = 0,
    OpaqueModifierVolume      = 1,
    Translucent               = 2,
    TranslucentModifierVolume = 3,
    PunchThrough              = 4, // HOLLY2 only
};

static constexpr uint32_t NUM_LIST_TYPES = 5;

// Parameter types (bits 31-29 of Parameter Control Word)
enum class ParaType : uint8_t {
    EndOfList      = 0, // Control parameter
    UserTileClip   = 1, // Control parameter
    ObjectListSet  = 2, // Control parameter
    Reserved3      = 3,
    Polygon        = 4, // Global parameter (Polygon or Modifier Volume)
    Sprite         = 5, // Global parameter
    Reserved6      = 6,
    Vertex         = 7, // Vertex parameter
};

// Color data format (bits 5-4 of Parameter Control Word Obj Control)
enum class ColorType : uint8_t {
    PackedColor    = 0, // 8-bit values for each of A, R, G, B
    FloatingColor  = 1, // 32-bit float for each of A, R, G, B
    IntensityMode1 = 2, // Face Color specified by preceding Global Params
    IntensityMode2 = 3, // Reuses previous Face Color from Mode 1
};

// Depth compare modes (bits 31-29 of ISP/TSP Instruction Word)
enum class DepthCompareMode : uint8_t {
    Never          = 0,
    Less           = 1,
    Equal          = 2,
    LessOrEqual    = 3,
    Greater        = 4,
    NotEqual       = 5,
    GreaterOrEqual = 6,
    Always         = 7,
};

// Culling modes (bits 28-27 of ISP/TSP Instruction Word)
enum class CullingMode : uint8_t {
    NoCulling      = 0,
    CullIfSmall    = 1, // Cull if |det| < fpu_cull_val
    CullIfNegative = 2, // Cull if det < 0 or |det| < fpu_cull_val
    CullIfPositive = 3, // Cull if det > 0 or |det| < fpu_cull_val
};

// Texture pixel formats (bits 29-27 of Texture Control Word)
enum class TexturePixelFormat : uint8_t {
    ARGB1555    = 0,
    RGB565      = 1,
    ARGB4444    = 2,
    YUV422      = 3,
    BumpMap     = 4,
    Palette4BPP = 5,
    Palette8BPP = 6,
    Reserved7   = 7, // Treated as ARGB1555
};

// Filter modes (bits 14-13 of TSP Instruction Word)
enum class FilterMode : uint8_t {
    PointSampled   = 0,
    BilinearFilter = 1,
    TrilinearPassA = 2,
    TrilinearPassB = 3,
};

// Texture/Shading instructions (bits 7-6 of TSP Instruction Word)
enum class TexShadingInstr : uint8_t {
    Decal          = 0,
    Modulate       = 1,
    DecalAlpha     = 2,
    ModulateAlpha  = 3,
};

// Fog control modes (bits 23-22 of TSP Instruction Word)
enum class FogControl : uint8_t {
    LookUpTable      = 0,
    PerVertex        = 1,
    NoFog            = 2,
    LookUpTableMode2 = 3,
};

// Alpha blend source/destination instructions (bits 31-29, 28-26 of TSP)
enum class AlphaInstr : uint8_t {
    Zero              = 0,
    One               = 1,
    OtherColor        = 2,
    InverseOtherColor = 3,
    SrcAlpha          = 4,
    InverseSrcAlpha   = 5,
    DstAlpha          = 6,
    InverseDstAlpha   = 7,
};

// Volume instructions (bits 31-29 of ISP/TSP for Modifier Volume)
enum class VolumeInstruction : uint8_t {
    NormalPolygon      = 0,
    InsideLastPolygon  = 1,
    OutsideLastPolygon = 2,
    // 3-7 reserved
};

// ============================================================================
// Parsed Parameter Control Word
// ============================================================================
//
// Full 32-bit layout (HOLLY2):
//   bit 31-29: Para Type
//   bit 28:    End Of Strip
//   bit 27:    Reserved
//   bit 26-24: List Type (HOLLY2: 3 bits)
//   bit 23:    Group_En
//   bit 22-20: Reserved
//   bit 19-18: Strip_Len
//   bit 17-16: User_Clip
//   bit 15-8:  Reserved
//   bit 7:     Shadow
//   bit 6:     Volume
//   bit 5-4:   Col_Type
//   bit 3:     Texture
//   bit 2:     Offset
//   bit 1:     Gouraud
//   bit 0:     16bit_UV
//

struct ParameterControlWord {
    ParaType  para_type;     // bits 31-29
    bool      end_of_strip;  // bit 28
    ListType  list_type;     // bits 26-24 (HOLLY2)
    bool      group_en;      // bit 23
    uint8_t   strip_len;     // bits 19-18
    uint8_t   user_clip;     // bits 17-16
    bool      shadow;        // bit 7
    bool      volume;        // bit 6
    ColorType col_type;      // bits 5-4
    bool      texture;       // bit 3
    bool      offset;        // bit 2
    bool      gouraud;       // bit 1
    bool      uv_16bit;      // bit 0

    static ParameterControlWord parse(uint32_t word) {
        ParameterControlWord pcw{};
        pcw.para_type    = static_cast<ParaType>((word >> 29) & 0x7);
        pcw.end_of_strip = (word >> 28) & 0x1;
        pcw.list_type    = static_cast<ListType>((word >> 24) & 0x7);
        pcw.group_en     = (word >> 23) & 0x1;
        pcw.strip_len    = (word >> 18) & 0x3;
        pcw.user_clip    = (word >> 16) & 0x3;
        pcw.shadow       = (word >>  7) & 0x1;
        pcw.volume       = (word >>  6) & 0x1;
        pcw.col_type     = static_cast<ColorType>((word >> 4) & 0x3);
        pcw.texture      = (word >>  3) & 0x1;
        pcw.offset       = (word >>  2) & 0x1;
        pcw.gouraud      = (word >>  1) & 0x1;
        pcw.uv_16bit     = (word >>  0) & 0x1;
        return pcw;
    }
};

// ============================================================================
// ISP/TSP Instruction Word
// ============================================================================
//
// Opaque or Translucent:
//   bit 31-29: Depth Compare Mode
//   bit 28-27: Culling Mode
//   bit 26:    Z Write Disable
//   bit 25:    Texture
//   bit 24:    Offset
//   bit 23:    Gouraud Shading
//   bit 22:    16Bit UV
//   bit 21:    Cache Bypass
//   bit 20:    Dcalc Ctrl
//   bit 19-0:  Reserved
//
// Modifier Volume (Opaque/Translucent Modifier Volume):
//   bit 31-29: Volume Instruction
//   bit 28-27: Culling Mode
//   bit 26-0:  Reserved
//

struct ISPTSPInstructionWord {
    // For normal polygons (Opaque / Translucent / PunchThrough)
    DepthCompareMode depth_compare_mode; // bits 31-29
    CullingMode      culling_mode;       // bits 28-27
    bool             z_write_disable;    // bit 26
    bool             texture;            // bit 25
    bool             offset;             // bit 24
    bool             gouraud;            // bit 23
    bool             uv_16bit;           // bit 22
    bool             cache_bypass;       // bit 21
    bool             dcalc_ctrl;         // bit 20

    // For modifier volumes (reinterpretation of bits 31-29)
    VolumeInstruction volume_instruction; // bits 31-29

    static ISPTSPInstructionWord parse(uint32_t word) {
        ISPTSPInstructionWord isp{};
        isp.depth_compare_mode = static_cast<DepthCompareMode>((word >> 29) & 0x7);
        isp.culling_mode       = static_cast<CullingMode>((word >> 27) & 0x3);
        isp.z_write_disable    = (word >> 26) & 0x1;
        isp.texture            = (word >> 25) & 0x1;
        isp.offset             = (word >> 24) & 0x1;
        isp.gouraud            = (word >> 23) & 0x1;
        isp.uv_16bit           = (word >> 22) & 0x1;
        isp.cache_bypass       = (word >> 21) & 0x1;
        isp.dcalc_ctrl         = (word >> 20) & 0x1;

        // Same bits reinterpreted for modifier volumes
        isp.volume_instruction = static_cast<VolumeInstruction>((word >> 29) & 0x7);

        return isp;
    }
};

// ============================================================================
// TSP Instruction Word
// ============================================================================
//
//   bit 31-29: SRC Alpha Instr       (3 bits)
//   bit 28-26: DST Alpha Instr       (3 bits)
//   bit 25:    SRC Select            (1 bit)
//   bit 24:    DST Select            (1 bit)
//   bit 23-22: Fog Control           (2 bits)
//   bit 21:    Color Clamp           (1 bit)
//   bit 20:    Use Alpha             (1 bit)
//   bit 19:    Ignore Tex Alpha      (1 bit)
//   bit 18:    Flip U                (1 bit)
//   bit 17:    Flip V                (1 bit)
//   bit 16:    Clamp U               (1 bit)
//   bit 15:    Clamp V               (1 bit)
//   bit 14-13: Filter Mode           (2 bits)
//   bit 12:    Super-Sample Texture  (1 bit)
//   bit 11-8:  MIP-Map D adjust      (4 bits)
//   bit 7-6:   Tex/Shading Instr     (2 bits)
//   bit 5-3:   Texture U Size        (3 bits)
//   bit 2-0:   Texture V Size        (3 bits)
//

struct TSPInstructionWord {
    AlphaInstr     src_alpha_instr;   // bits 31-29
    AlphaInstr     dst_alpha_instr;   // bits 28-26
    bool           src_select;        // bit 25
    bool           dst_select;        // bit 24
    FogControl     fog_control;       // bits 23-22
    bool           color_clamp;       // bit 21
    bool           use_alpha;         // bit 20
    bool           ignore_tex_alpha;  // bit 19
    bool           flip_u;            // bit 18
    bool           flip_v;            // bit 17
    bool           clamp_u;           // bit 16
    bool           clamp_v;           // bit 15
    FilterMode     filter_mode;       // bits 14-13
    bool           super_sample;      // bit 12
    uint8_t        mipmap_d_adjust;   // bits 11-8 (4-bit unsigned fixed 2.2)
    TexShadingInstr tex_shading_instr; // bits 7-6
    uint8_t        tex_u_size;        // bits 5-3 (0=8, 1=16 .. 7=1024)
    uint8_t        tex_v_size;        // bits 2-0 (0=8, 1=16 .. 7=1024)

    // Return the actual texture U dimension in texels.
    uint32_t texture_u_pixels() const { return 8u << tex_u_size; }

    // Return the actual texture V dimension in texels.
    uint32_t texture_v_pixels() const { return 8u << tex_v_size; }

    static TSPInstructionWord parse(uint32_t word) {
        TSPInstructionWord tsp{};
        tsp.src_alpha_instr   = static_cast<AlphaInstr>((word >> 29) & 0x7);
        tsp.dst_alpha_instr   = static_cast<AlphaInstr>((word >> 26) & 0x7);
        tsp.src_select        = (word >> 25) & 0x1;
        tsp.dst_select        = (word >> 24) & 0x1;
        tsp.fog_control       = static_cast<FogControl>((word >> 22) & 0x3);
        tsp.color_clamp       = (word >> 21) & 0x1;
        tsp.use_alpha         = (word >> 20) & 0x1;
        tsp.ignore_tex_alpha  = (word >> 19) & 0x1;
        tsp.flip_u            = (word >> 18) & 0x1;
        tsp.flip_v            = (word >> 17) & 0x1;
        tsp.clamp_u           = (word >> 16) & 0x1;
        tsp.clamp_v           = (word >> 15) & 0x1;
        tsp.filter_mode       = static_cast<FilterMode>((word >> 13) & 0x3);
        tsp.super_sample      = (word >> 12) & 0x1;
        tsp.mipmap_d_adjust   = (word >>  8) & 0xF;
        tsp.tex_shading_instr = static_cast<TexShadingInstr>((word >> 6) & 0x3);
        tsp.tex_u_size        = (word >>  3) & 0x7;
        tsp.tex_v_size        = (word >>  0) & 0x7;
        return tsp;
    }
};

// ============================================================================
// Texture Control Word
// ============================================================================
//
// RGB/YUV Texture or Bump Map:
//   bit 31:    MIP Mapped
//   bit 30:    VQ Compressed
//   bit 29-27: Pixel Format
//   bit 26:    Scan Order
//   bit 25:    Stride Select
//   bit 24-21: Reserved
//   bit 20-0:  Texture Address (64-bit word address)
//
// Palette Texture:
//   bit 31:    MIP Mapped
//   bit 30:    VQ Compressed
//   bit 29-27: Pixel Format
//   bit 26-21: Palette Selector (6 bits; only upper 2 valid for 8BPP)
//   bit 20-0:  Texture Address (64-bit word address)
//

struct TextureControlWord {
    bool               mip_mapped;        // bit 31
    bool               vq_compressed;     // bit 30
    TexturePixelFormat pixel_format;      // bits 29-27
    bool               scan_order;        // bit 26
    bool               stride_select;     // bit 25
    uint8_t            palette_selector;  // bits 26-21 (for palette textures)
    uint32_t           texture_address;   // bits 20-0 (64-bit word address)

    // Return the byte address of the texture in VRAM.
    uint32_t byte_address() const { return texture_address << 3; }

    bool is_palette() const {
        return pixel_format == TexturePixelFormat::Palette4BPP ||
               pixel_format == TexturePixelFormat::Palette8BPP;
    }

    static TextureControlWord parse(uint32_t word) {
        TextureControlWord tcw{};
        tcw.mip_mapped       = (word >> 31) & 0x1;
        tcw.vq_compressed    = (word >> 30) & 0x1;
        tcw.pixel_format     = static_cast<TexturePixelFormat>((word >> 27) & 0x7);
        tcw.palette_selector = (word >> 21) & 0x3F;
        tcw.texture_address  = word & 0x1FFFFF;
        // For palette textures (4BPP / 8BPP) bits 26-21 are entirely the
        // palette selector; there is no scan_order or stride_select field.
        // Unconditionally reading bit 26 as scan_order causes fetch_texel to
        // use the linear (non-twiddled) address formula whenever the palette
        // selector has its bit 5 set (i.e. the 8BPP bank-select bit is 1 or
        // the 4BPP selector is >= 32), scattering texels to wrong positions.
        if (tcw.is_palette()) {
            tcw.scan_order    = false;  // palette textures are always twiddled
            tcw.stride_select = false;
        } else {
            tcw.scan_order    = (word >> 26) & 0x1;
            tcw.stride_select = (word >> 25) & 0x1;
        }
        return tcw;
    }
};

// ============================================================================
// Vertex
// ============================================================================

struct Vertex {
    float    x;
    float    y;
    float    z;            // 1/W (inverse depth)
    float    u;
    float    v;
    uint32_t base_color;   // Packed ARGB8888
    uint32_t offset_color; // Packed ARGB8888
};

// ============================================================================
// Object Pointer (parsed from Object List entries)
// ============================================================================
//
// Triangle Strip:     bit 31=0, 30-25=mask, 24=shadow, 23-21=skip, 20-0=addr
// Triangle Array:     bits 31-29=100, 28-25=num_tri, 24=shadow, 23-21=skip, 20-0=addr
// Quad Array:         bits 31-29=101, 28-25=num_quad, 24=shadow, 23-21=skip, 20-0=addr
// Pointer Block Link: bits 31-29=111, 28=end_of_list, 23-2=next_ptr, 1-0=00
//

struct ObjectPointer {
    enum class Type : uint8_t {
        TriangleStrip    = 0,
        TriangleArray    = 1,
        QuadArray        = 2,
        PointerBlockLink = 3,
    };

    Type     type;
    uint8_t  mask;            // 6 bits for triangle strip (which triangles hit this tile)
    uint8_t  num_primitives;  // 4 bits for array types (0 = 1 primitive, 1 = 2, etc.)
    bool     shadow;          // bit 24
    uint8_t  skip;            // bits 23-21 (vertex data size = skip+3 32-bit words)
    uint32_t start_address;   // bits 20-0 (32-bit word address, relative to PARAM_BASE)
    uint32_t next_pointer;    // bits 23-2 for block link (32-bit word address)
    bool     end_of_list;     // bit 28 for block link

    static ObjectPointer parse(uint32_t word) {
        ObjectPointer op{};

        uint8_t top3 = (word >> 29) & 0x7;

        if ((word & 0x80000000u) == 0) {
            // Bit 31 is 0: Triangle Strip
            op.type           = Type::TriangleStrip;
            op.mask           = (word >> 25) & 0x3F;
            op.shadow         = (word >> 24) & 0x1;
            op.skip           = (word >> 21) & 0x7;
            op.start_address  = word & 0x1FFFFF;
            op.num_primitives = 0;
            op.next_pointer   = 0;
            op.end_of_list    = false;
        } else if (top3 == 0b100) {
            // Triangle Array
            op.type           = Type::TriangleArray;
            op.num_primitives = (word >> 25) & 0xF;
            op.shadow         = (word >> 24) & 0x1;
            op.skip           = (word >> 21) & 0x7;
            op.start_address  = word & 0x1FFFFF;
            op.mask           = 0;
            op.next_pointer   = 0;
            op.end_of_list    = false;
        } else if (top3 == 0b101) {
            // Quad Array
            op.type           = Type::QuadArray;
            op.num_primitives = (word >> 25) & 0xF;
            op.shadow         = (word >> 24) & 0x1;
            op.skip           = (word >> 21) & 0x7;
            op.start_address  = word & 0x1FFFFF;
            op.mask           = 0;
            op.next_pointer   = 0;
            op.end_of_list    = false;
        } else if (top3 == 0b111) {
            // Pointer Block Link
            op.type           = Type::PointerBlockLink;
            op.end_of_list    = (word >> 28) & 0x1;
            op.next_pointer   = (word >> 2) & 0x3FFFFF;
            op.mask           = 0;
            op.num_primitives = 0;
            op.shadow         = false;
            op.skip           = 0;
            op.start_address  = 0;
        } else {
            // Reserved / unknown -- treat as end-of-list block link
            op.type           = Type::PointerBlockLink;
            op.end_of_list    = true;
            op.next_pointer   = 0;
            op.mask           = 0;
            op.num_primitives = 0;
            op.shadow         = false;
            op.skip           = 0;
            op.start_address  = 0;
        }
        return op;
    }
};

// ============================================================================
// Region Array Entry (HOLLY2, Type 2)
// ============================================================================
//
// Header word:
//   bit 31:    Last Region
//   bit 30:    Z Clear (0 = clear Z buffer, 1 = do not clear)
//   bit 29:    Pre Sort (0 = auto-sort, 1 = pre-sort)
//   bit 28:    Flush Accumulate (0 = copy to FB, 1 = do not copy)
//   bit 27-14: Reserved
//   bit 13-8:  Tile Y position (actual Y = value * 32)
//   bit 7-2:   Tile X position (actual X = value * 32)
//   bit 1-0:   Reserved
//
// Followed by 5 List Pointers (one per ListType):
//   bit 31:    Empty PTR (1 = list not present)
//   bit 30-24: Reserved
//   bit 23-2:  Pointer to Object List (32-bit word address)
//   bit 1-0:   00
//

struct RegionListPointer {
    bool     empty;   // bit 31 -- list does not exist for this tile
    uint32_t address; // bits 23-2 (32-bit word address)

    static RegionListPointer parse(uint32_t word) {
        RegionListPointer lp{};
        lp.empty   = (word >> 31) & 0x1;
        lp.address = (word >> 2) & 0x3FFFFF;
        return lp;
    }

    // Return the byte address in VRAM.
    uint32_t byte_address() const { return address << 2; }
};

struct RegionArrayEntry {
    // Header fields
    bool     last_region;       // bit 31
    bool     z_clear;           // bit 30 (0 = clear Z, 1 = do not clear)
    bool     pre_sort;          // bit 29 (0 = auto-sort, 1 = pre-sort)
    bool     flush_accumulate;  // bit 28 (0 = copy to FB, 1 = skip copy)
    uint8_t  tile_x;            // bits 7-2 (actual X = tile_x * 32)
    uint8_t  tile_y;            // bits 13-8 (actual Y = tile_y * 32)

    // One pointer per list type (indexed by ListType)
    std::array<RegionListPointer, NUM_LIST_TYPES> list_pointers;

    // Pixel coordinates of the tile's top-left corner.
    uint32_t pixel_x() const { return static_cast<uint32_t>(tile_x) * TILE_SIZE; }
    uint32_t pixel_y() const { return static_cast<uint32_t>(tile_y) * TILE_SIZE; }

    // Parse the header word only. List pointer words must be parsed separately.
    static RegionArrayEntry parse_header(uint32_t word) {
        RegionArrayEntry entry{};
        entry.last_region      = (word >> 31) & 0x1;
        entry.z_clear          = (word >> 30) & 0x1;
        entry.pre_sort         = (word >> 29) & 0x1;
        entry.flush_accumulate = (word >> 28) & 0x1;
        entry.tile_y           = (word >>  8) & 0x3F;
        entry.tile_x           = (word >>  2) & 0x3F;
        entry.list_pointers    = {};
        return entry;
    }

    // Parse a complete Type 2 region entry (header + 5 list pointer words).
    // |words| must point to at least 6 consecutive uint32_t values.
    static RegionArrayEntry parse(const uint32_t* words) {
        RegionArrayEntry entry = parse_header(words[0]);
        for (uint32_t i = 0; i < NUM_LIST_TYPES; ++i) {
            entry.list_pointers[i] = RegionListPointer::parse(words[1 + i]);
        }
        return entry;
    }
};

// ============================================================================
// Pixel (ARGB, 8 bits per channel)
// ============================================================================

struct Pixel {
    uint8_t a;
    uint8_t r;
    uint8_t g;
    uint8_t b;

    // Construct from individual channel values.
    static Pixel from_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
        return {a, r, g, b};
    }

    // Construct a fully opaque pixel.
    static Pixel from_rgb(uint8_t r, uint8_t g, uint8_t b) {
        return {0xFF, r, g, b};
    }

    // Unpack from a 32-bit packed ARGB8888 value.
    static Pixel from_packed(uint32_t argb) {
        return {
            static_cast<uint8_t>((argb >> 24) & 0xFF),
            static_cast<uint8_t>((argb >> 16) & 0xFF),
            static_cast<uint8_t>((argb >>  8) & 0xFF),
            static_cast<uint8_t>((argb >>  0) & 0xFF)
        };
    }

    // Pack into a 32-bit ARGB8888 value.
    uint32_t to_packed() const {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) <<  8) |
               (static_cast<uint32_t>(b) <<  0);
    }

    // Clamp channels to the given min/max range.
    Pixel clamped(const Pixel& lo, const Pixel& hi) const {
        return {
            std::clamp(a, lo.a, hi.a),
            std::clamp(r, lo.r, hi.r),
            std::clamp(g, lo.g, hi.g),
            std::clamp(b, lo.b, hi.b)
        };
    }

    // Linearly blend: result = this * (1 - t) + other * t, where t is [0..255].
    Pixel lerp(const Pixel& other, uint8_t t) const {
        auto mix = [](uint8_t a_val, uint8_t b_val, uint8_t t_val) -> uint8_t {
            uint16_t inv = 255 - t_val;
            return static_cast<uint8_t>((a_val * inv + b_val * t_val + 127) / 255);
        };
        return {
            mix(a, other.a, t),
            mix(r, other.r, t),
            mix(g, other.g, t),
            mix(b, other.b, t)
        };
    }

    // Per-channel multiply (result = this * other / 255).
    Pixel modulate(const Pixel& other) const {
        auto mul = [](uint8_t x, uint8_t y) -> uint8_t {
            return static_cast<uint8_t>((static_cast<uint16_t>(x) * y + 127) / 255);
        };
        return {
            mul(a, other.a),
            mul(r, other.r),
            mul(g, other.g),
            mul(b, other.b)
        };
    }

    // Saturating per-channel addition.
    Pixel add_sat(const Pixel& other) const {
        auto sat = [](uint8_t x, uint8_t y) -> uint8_t {
            uint16_t s = static_cast<uint16_t>(x) + y;
            return s > 255 ? static_cast<uint8_t>(255) : static_cast<uint8_t>(s);
        };
        return {
            sat(a, other.a),
            sat(r, other.r),
            sat(g, other.g),
            sat(b, other.b)
        };
    }

    // Scale all channels by a single factor [0..255].
    Pixel scale(uint8_t factor) const {
        auto sc = [](uint8_t v, uint8_t f) -> uint8_t {
            return static_cast<uint8_t>((static_cast<uint16_t>(v) * f + 127) / 255);
        };
        return {
            sc(a, factor),
            sc(r, factor),
            sc(g, factor),
            sc(b, factor)
        };
    }

    bool operator==(const Pixel& other) const {
        return a == other.a && r == other.r && g == other.g && b == other.b;
    }

    bool operator!=(const Pixel& other) const {
        return !(*this == other);
    }
};

} // namespace pvr
