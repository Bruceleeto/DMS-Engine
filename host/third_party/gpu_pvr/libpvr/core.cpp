#include "core.h"
#include "vram.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cmath>

namespace pvr {

// Gouraud colour channel. The hardware keeps a flat colour exact: truncating
// weights that sum to 0.9999 would turn 8 into 7 (and 0 in RGB565)
static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t c, float w0, float w1, float w2) {
    if (a == b && b == c) return a;
    float v = a * w0 + b * w1 + c * w2 + 0.5f;
    return static_cast<uint8_t>(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v);
}

// ============================================================================
// Per-pixel modifier-volume stencil bits (TileBuffer::stencil)
// ============================================================================

static constexpr uint8_t STENCIL_AREA1  = 0x01; // pixel is in "area 1"
static constexpr uint8_t STENCIL_VOLPAR = 0x02; // parity of the current volume
static constexpr uint8_t STENCIL_SHADOW = 0x04; // owning pixel is shadow-enabled

// ============================================================================
// 16-bit UV to float conversion
//
// PVR 16-bit UV values are NOT IEEE 754 binary16 half-floats.  They are the
// upper 16 bits of a standard 32-bit IEEE 754 single-precision float, with
// the lower 16 bits discarded.  Decode by placing the 16 bits back into the
// high word of a 32-bit float.
// ============================================================================

static float half_to_float(uint16_t h) {
    uint32_t bits = static_cast<uint32_t>(h) << 16;
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

// Read a float from a uint32_t bit pattern
static float bits_to_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

// ============================================================================
// Per-thread worker id, used by start_render() to pick a TileContext slot
// out of the array sized to worker_count. The main thread's default value
// of 0 means inline (non-pool) rendering picks slot 0 automatically.
// ============================================================================

namespace {
thread_local unsigned int g_worker_id = 0;
} // namespace

// ============================================================================
// Constructor / setup
// ============================================================================

CORE::CORE() {
    reset();
}

CORE::~CORE() {
    if( render_thread_.joinable() )
        render_thread_.join();
}

void CORE::set_thread_count(unsigned int count) {
    configured_thread_count_ = count;
}

void CORE::set_vram(uint8_t* vram, size_t vram_size) {
    vram_ = vram;
    vram_size_ = vram_size;
    texture_sampler_.set_vram(vram, vram_size);
}

void CORE::set_registers(RegisterBank* regs) {
    regs_ = regs;
    texture_sampler_.set_registers(regs);
}

void CORE::reset() {
    render_complete = false;
}

// ============================================================================
// VRAM access helpers
//
// Reads inside the CORE (parameter/region/OPB/background data) always come
// in through the 64-bit access path, because that is how the render
// pipeline is wired up on real pvr hardware. Framebuffer writes on the
// other hand go through whichever path FB_W_SOF1 / FB_W_SOF2 was used:
// SOF1 targets the 32-bit framebuffer area, SOF2 targets the 64-bit
// texture-memory area (used for render-to-texture).
// ============================================================================

uint32_t CORE::vram_read32(uint32_t byte_addr) const {
    /* When a parameter snapshot is active, redirect reads that fall
     * within the captured range so the render thread sees the geometry
     * state as of RENDER_START time, even after the TA has begun writing
     * new data to live VRAM for the next frame. */
    if( !param_snapshot_.empty() ) {
        uint32_t phys = byte_addr & static_cast<uint32_t>( vram_size_ - 1 );
        uint32_t off  = phys - param_snapshot_base_;
        if( off < static_cast<uint32_t>( param_snapshot_.size() ) ) {
            uint32_t v = 0;
            std::memcpy( &v, param_snapshot_.data() + off, 4 );
            return v;
        }
    }
    /* Fall through to live VRAM for everything outside the snapshot
     * (textures, framebuffer, etc.). */
    return vram_read32_64( vram_, vram_size_, byte_addr );
}

void CORE::vram_write32(uint32_t byte_addr, uint32_t value) {
    // Default path for internal writers is the 64-bit path; framebuffer
    // flush paths go through the dedicated helpers below.
    vram_write32_64(vram_, vram_size_, byte_addr, value);
}

void CORE::vram_write16(uint32_t byte_addr, uint16_t value) {
    vram_write16_64(vram_, vram_size_, byte_addr, value);
}

// ============================================================================
// Static math helpers
// ============================================================================

float CORE::edge_function(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

float CORE::interpolate_bary(float v0, float v1, float v2, float w0, float w1, float w2) {
    // Keep a constant depth exact. Independently computed barycentric weights
    // can sum to slightly more or less than one; multiplying a flat Z by that
    // sum makes coplanar layers intermittently fail GEQUAL (or pass GREATER).
    if (v0 == v1 && v1 == v2) return v0;
    return v0 * w0 + v1 * w1 + v2 * w2;
}

float CORE::interpolate_persp(float v0, float v1, float v2,
                               float z0, float z1, float z2,
                               float w0, float w1, float w2) {
    // z0,z1,z2 are 1/W values; w0,w1,w2 are barycentric weights
    float num = w0 * v0 * z0 + w1 * v1 * z1 + w2 * v2 * z2;
    float den = w0 * z0 + w1 * z1 + w2 * z2;
    if (std::abs(den) < 1e-12f) return 0.0f;
    return num / den;
}

// ============================================================================
// Blend factor computation
// ============================================================================

Pixel CORE::compute_blend_factor(AlphaInstr instr, const Pixel& src, const Pixel& dst, bool is_src_factor) {
    switch (instr) {
    case AlphaInstr::Zero:
        return Pixel{0, 0, 0, 0};
    case AlphaInstr::One:
        return Pixel{255, 255, 255, 255};
    case AlphaInstr::OtherColor:
        return is_src_factor ? dst : src;
    case AlphaInstr::InverseOtherColor: {
        const Pixel& other = is_src_factor ? dst : src;
        return Pixel{
            static_cast<uint8_t>(255 - other.a),
            static_cast<uint8_t>(255 - other.r),
            static_cast<uint8_t>(255 - other.g),
            static_cast<uint8_t>(255 - other.b)
        };
    }
    case AlphaInstr::SrcAlpha:
        return Pixel{src.a, src.a, src.a, src.a};
    case AlphaInstr::InverseSrcAlpha: {
        uint8_t inv = 255 - src.a;
        return Pixel{inv, inv, inv, inv};
    }
    case AlphaInstr::DstAlpha:
        return Pixel{dst.a, dst.a, dst.a, dst.a};
    case AlphaInstr::InverseDstAlpha: {
        uint8_t inv = 255 - dst.a;
        return Pixel{inv, inv, inv, inv};
    }
    default:
        return Pixel{0, 0, 0, 0};
    }
}

// ============================================================================
// start_render
// ============================================================================

void CORE::start_render() {
    render_complete = false;
    isp_out_of_cache = false;
    if (!vram_ || !regs_) return;

    // --- 1. Walk the region array, collecting every entry up-front.
    //        Doing this sequentially (rather than interleaving with
    //        rendering) makes it trivial to dispatch tiles in parallel.
    uint32_t region_base = regs_->read(RegisterName::REGION_BASE);
    bool type2 = regs_->region_header_type(); // bit 21 of FPU_PARAM_CFG
    uint32_t entry_words = type2 ? 6u : 5u;   // Type 2 has Punch Through pointer
    uint32_t addr = region_base;

    // Hard cap: the Dreamcast TV output is at most 640x480, which is
    // 20x15 = 300 tiles. Anything beyond a generous multiple of that
    // means we're walking into garbage; bail out rather than spinning
    // forever building a multi-gigabyte vector.
    constexpr std::size_t MAX_REGION_ENTRIES = 4096;

    std::vector<RegionArrayEntry> entries;
    bool overflow = false;
    for (;;) {
        uint32_t words[6];
        for (uint32_t i = 0; i < entry_words; i++) {
            words[i] = vram_read32(addr + i * 4);
        }

        RegionArrayEntry entry;
        if (type2) {
            entry = RegionArrayEntry::parse(words);
        } else {
            // Type 1: header + 4 list pointers (Opaque, OpaqueMV, Translucent, TranslucentMV)
            entry = RegionArrayEntry::parse_header(words[0]);
            for (uint32_t i = 0; i < 4; i++) {
                entry.list_pointers[i] = RegionListPointer::parse(words[1 + i]);
            }
            // No Punch Through in Type 1
            entry.list_pointers[static_cast<int>(ListType::PunchThrough)] = RegionListPointer{true, 0};
        }

        entries.push_back(entry);
        if (entry.last_region) break;
        addr += entry_words * 4;

        if (entries.size() >= MAX_REGION_ENTRIES) {
            overflow = true;
            break;
        }
    }

    if (overflow) {
        // Walked past MAX_REGION_ENTRIES without finding a last_region
        // marker: the region array is unterminated or garbage.
        isp_out_of_cache = true;
        render_complete = true;
        return;
    }

    // --- 1b. Merge consecutive region entries for the same tile.
    //
    // Some software (notably the Dreamcast BIOS) creates two region
    // entries per tile: the first carries geometry with z_clear=0,
    // flush=1, the second is empty with z_clear=1, flush=0. On real
    // hardware these accumulate in the same tile buffer, but since we
    // render each entry in an independent TileContext, the second
    // entry would start fresh and overwrite the first.
    //
    // Merge consecutive entries for the same tile into a single entry
    // so all geometry is processed together in one TileContext.
    std::vector<RegionArrayEntry> merged;
    merged.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ) {
        RegionArrayEntry acc = entries[i];
        std::size_t j = i + 1;
        // Walk ahead while consecutive entries share the same tile
        while (j < entries.size() &&
               entries[j].tile_x == acc.tile_x &&
               entries[j].tile_y == acc.tile_y) {
            // Merge: z_clear from first entry, flush/pre_sort/last from last
            acc.flush_accumulate = entries[j].flush_accumulate;
            acc.pre_sort         = entries[j].pre_sort;
            acc.last_region      = entries[j].last_region;
            acc.z_clear          = acc.z_clear; // keep first entry's z_clear

            // Merge list pointers: prefer non-empty pointers
            for (uint32_t lt = 0; lt < NUM_LIST_TYPES; ++lt) {
                if (!entries[j].list_pointers[lt].empty) {
                    acc.list_pointers[lt] = entries[j].list_pointers[lt];
                }
            }
            j++;
        }
        merged.push_back(acc);
        i = j;
    }

    if (render_gl(merged)) {
        render_complete = true;
        return;
    }

    // --- 2. Resize the thread pool (no-op if unchanged) and allocate
    //        one TileContext per worker slot.
    pool_.resize(configured_thread_count_);
    unsigned int ctx_count = std::max(1u, pool_.worker_count());
    std::vector<TileContext> contexts(ctx_count);

    // --- 3. Dispatch the tiles across the pool.
    pool_.parallel_for(merged.size(),
        [this, &contexts, &merged](std::size_t i) {
            render_tile(contexts[g_worker_id], merged[i]);
        });

    render_complete = true;
}

void CORE::wait_render_done()
{
    if( render_thread_.joinable() ) {
        render_thread_.join();
        /* Restore the live register pointer and clear the texture snapshot
         * now that the previous render thread has finished. */
        if( live_regs_backup_ ) {
            regs_ = live_regs_backup_;
            texture_sampler_.set_registers( live_regs_backup_ );
            texture_sampler_.set_snapshot( nullptr, 0, 0 );
            live_regs_backup_ = nullptr;
        }
    }
}

void CORE::take_param_snapshot()
{
    param_snapshot_.clear();
    if( !vram_ || vram_size_ == 0 ) return;

    /* Snapshot the ENTIRE VRAM.
     *
     * Earlier revisions of this function attempted to compute a
     * minimal bounding window covering only the region array, the
     * ISP/TSP parameter area, and the OPB area.  That approach is
     * fragile for several reasons:
     *
     *   1. OPB allocation can run in either direction.  KOS (and
     *      therefore most commercial titles, including Doom 64)
     *      uses decreasing mode where TA_OL_LIMIT < TA_OL_BASE and
     *      OPBs grow downward from TA_OL_BASE toward TA_OL_LIMIT.
     *      The previous lower-bound computation only considered
     *      TA_OL_BASE and missed the entire [TA_OL_LIMIT, TA_OL_BASE)
     *      range that the OPBs actually occupy in this mode.
     *
     *   2. Background polygons are read from PARAM_BASE +
     *      ISP_BACKGND_T (an arbitrary offset), which need not lie
     *      within the heuristic [PARAM_BASE, PARAM_BASE+4MB] window.
     *
     *   3. Textures are read through the bank-interleave
     *      translation, so a logical TCW texture address can map to
     *      almost any physical byte in VRAM.  Restricting the
     *      snapshot leaves textures whose translated bytes fall in
     *      the OPB region exposed to concurrent TA writes for the
     *      next frame.
     *
     * When any of these regions is left outside the snapshot, the
     * render thread races against the TA writing the next frame and
     * reads partially-updated pointers / parameters, causing
     * polygons to disappear or be drawn with the wrong header.  In
     * Doom 64 this manifests as the bump-mapped grey overlay being
     * drawn without its paired textured polygon (or vice versa).
     *
     * An 8 MB memcpy on the host CPU is a few hundred microseconds
     * at most -- a small price for correctness, and dwarfed by the
     * actual software render that follows. */
    param_snapshot_base_ = 0;
    param_snapshot_.resize( vram_size_ );
    std::memcpy( param_snapshot_.data(), vram_, vram_size_ );
}

void CORE::start_render_async( std::function<void()> on_done )
{
    wait_render_done();

    /* Snapshot registers and TA parameter VRAM before launching the
     * render thread.  The register snapshot prevents register-write
     * races; the VRAM snapshot allows the TA to start writing new
     * geometry immediately (no stall in pvr2_ta_init) while the render
     * thread reads from the frozen copy. */
    render_snap_      = *regs_;
    live_regs_backup_ = regs_;
    take_param_snapshot();

    render_thread_ = std::thread( [this, on_done = std::move(on_done)]() {
        regs_ = &render_snap_;
        texture_sampler_.set_registers( &render_snap_ );
        /* Also protect texture reads: physical bytes in the snapshot range
         * are shared with the bank-interleave texture path.  Without this,
         * TA parameter writes for the next frame corrupt texture samples
         * whose translated physical address lands in that range. */
        texture_sampler_.set_snapshot(
            param_snapshot_.data(),
            param_snapshot_base_,
            static_cast<uint32_t>( param_snapshot_.size() ) );
        start_render();
        on_done();
    } );
}

// ============================================================================
// render_tile
// ============================================================================

void CORE::render_tile(TileContext& ctx, const RegionArrayEntry& region) {
    int tile_x = region.tile_x;
    int tile_y = region.tile_y;

    // Record tile coordinates on the context so process_object and the
    // rasterizer can read them without extra plumbing.
    ctx.tile_x = tile_x;
    ctx.tile_y = tile_y;

    // z_clear: 0 = clear Z buffer, 1 = do NOT clear
    if (!region.z_clear) {
        clear_tile(ctx, 0.0f);
    }

    // Draw background plane
    draw_background(ctx, tile_x, tile_y);

    // --- Opaque list ---
    {
        auto& lp = region.list_pointers[static_cast<int>(ListType::Opaque)];
        if (!lp.empty) {
            process_list(ctx, ListType::Opaque, lp.byte_address(), region.last_region);
        }
    }

    // --- Punch Through list ---
    // In HOLLY2 the opaque Modifier Volume list applies to both Opaque and
    // Punch Through polygons, so Punch Through is rendered (writing depth and
    // marking shadow-enabled pixels) before the modifier-volume parity test,
    // ensuring the volume is tested against the combined opaque+PT depth.
    {
        auto& lp = region.list_pointers[static_cast<int>(ListType::PunchThrough)];
        if (!lp.empty) {
            process_list(ctx, ListType::PunchThrough, lp.byte_address(), region.last_region);
        }
    }

    // --- Opaque Modifier Volume list ---
    // Builds the per-pixel area-1 stencil from the volume geometry.
    {
        auto& lp = region.list_pointers[static_cast<int>(ListType::OpaqueModifierVolume)];
        if (!lp.empty) {
            process_list(ctx, ListType::OpaqueModifierVolume, lp.byte_address(), region.last_region);
        }
    }

    // --- Apply opaque modifier-volume (Intensity Volume Mode) shadows ---
    // Darkens area-1 pixels of shadow-enabled Opaque / Punch Through polygons.
    apply_modifier_shadows(ctx);

    // --- Translucent list ---
    {
        auto& lp = region.list_pointers[static_cast<int>(ListType::Translucent)];
        if (!lp.empty) {
            if (!region.pre_sort) {
                // Auto-sort mode: collect all translucent fragments, sort by depth, blend
                ctx.translucent_fragments.clear();
                ctx.auto_sort_active = true;

                process_list(ctx, ListType::Translucent, lp.byte_address(), region.last_region);

                ctx.auto_sort_active = false;

                // Sort by pixel position then by ascending Z (smaller 1/W = farther = draw first).
                // stable_sort preserves submission order for equal-Z fragments: the spec requires
                // that when two pixels share the same Z value, the polygon submitted first is
                // drawn farthest away, which is exactly what stable insertion order gives us.
                std::stable_sort(ctx.translucent_fragments.begin(), ctx.translucent_fragments.end(),
                    [](const TranslucentFragment& a, const TranslucentFragment& b) {
                        if (a.y != b.y) return a.y < b.y;
                        if (a.x != b.x) return a.x < b.x;
                        return a.z < b.z;
                    });

                // Blend fragments back-to-front.  Auto-sort mode does NOT skip the depth
                // test — the spec states Z values are always compared with GreaterOrEqual
                // (the per-polygon ISP depth-compare mode field is ignored).  This is what
                // causes translucent fragments that lie behind opaque geometry to be
                // correctly culled rather than incorrectly composited on top.
                for (const auto& frag : ctx.translucent_fragments) {
                    if (!isp_depth_test(ctx, frag.x, frag.y, frag.z,
                                        DepthCompareMode::GreaterOrEqual,
                                        frag.z_write_disable)) {
                        continue;
                    }
                    alpha_blend(ctx, frag.x, frag.y, frag.color, frag.tsp);
                }
            } else {
                // Pre-sort mode: render in submission order
                process_list(ctx, ListType::Translucent, lp.byte_address(), region.last_region);
            }
        }
    }

    // --- Translucent Modifier Volume list ---
    // The volume geometry is parsed (building the area stencil), but applying
    // the result requires per-layer processing during the auto-sort blend
    // (spec §3.4.5.3); that is not yet wired up, so translucent modifier
    // volumes currently have no visible effect.
    {
        auto& lp = region.list_pointers[static_cast<int>(ListType::TranslucentModifierVolume)];
        if (!lp.empty) {
            process_list(ctx, ListType::TranslucentModifierVolume, lp.byte_address(), region.last_region);
        }
    }

    // flush_accumulate: 0 = copy to FB, 1 = do NOT copy
    if (!region.flush_accumulate) {
        flush_tile_to_framebuffer(ctx, tile_x, tile_y);
    }
}

// ============================================================================
// clear_tile
// ============================================================================

void CORE::clear_tile(TileContext& ctx, float depth_value) {
    for (int y = 0; y < static_cast<int>(TILE_SIZE); y++) {
        for (int x = 0; x < static_cast<int>(TILE_SIZE); x++) {
            ctx.buf.primary[y][x]   = Pixel{0, 0, 0, 0};
            ctx.buf.secondary[y][x] = Pixel{0, 0, 0, 0};
            ctx.buf.depth[y][x]     = depth_value;
            ctx.buf.stencil[y][x]   = 0;
        }
    }
}

// ============================================================================
// draw_background
// ============================================================================

void CORE::draw_background(TileContext& ctx, int tile_x, int tile_y) {
    if (!regs_) return;

    uint32_t tag_offset  = regs_->isp_backgnd_tag_offset();
    uint32_t tag_address = regs_->isp_backgnd_tag_address();
    uint32_t skip        = regs_->isp_backgnd_skip();

    // Background depth from ISP_BACKGND_D (as float)
    float bg_depth = bits_to_float(regs_->read(RegisterName::ISP_BACKGND_D));


    // Byte address of the ISP/TSP parameter block.
    // tag_address is a 32-bit word address relative to PARAM_BASE;
    // the absolute address is PARAM_BASE + tag_address * 4.
    uint32_t param_base = regs_->read(RegisterName::PARAM_BASE);
    uint32_t param_addr = param_base + tag_address * 4;

    // Read the control words
    ISPTSPInstructionWord isp = ISPTSPInstructionWord::parse(vram_read32(param_addr));
    TSPInstructionWord    tsp = TSPInstructionWord::parse(vram_read32(param_addr + 4));
    TextureControlWord    tcw{};

    // TCW is always present even for non-textured polygons (value is unused in that case)
    tcw = TextureControlWord::parse(vram_read32(param_addr + 8));
    if (!isp.texture) tcw = TextureControlWord{};
    uint32_t control_bytes = 12; // ISP + TSP + TCW (always 12 bytes)

    // Vertex data: each vertex = (skip + 3) 32-bit words
    // tag_offset = number of vertices to skip before the 3 background vertices
    uint32_t words_per_vertex = skip + 3;
    uint32_t vertex_start = param_addr + control_bytes + tag_offset * words_per_vertex * 4;

    // Read 3 background vertices
    Vertex verts[3];
    for (int i = 0; i < 3; i++) {
        uint32_t vaddr = vertex_start + i * words_per_vertex * 4;

        verts[i].x = bits_to_float(vram_read32(vaddr + 0));
        verts[i].y = bits_to_float(vram_read32(vaddr + 4));
        verts[i].z = bits_to_float(vram_read32(vaddr + 8));

        verts[i].u = 0.0f;
        verts[i].v = 0.0f;
        verts[i].base_color   = 0xFFFFFFFF;
        verts[i].offset_color = 0x00000000;

        uint32_t extra = 12;
        if (isp.texture) {
            if (isp.uv_16bit) {
                uint32_t uv_packed = vram_read32(vaddr + extra);
                verts[i].u = half_to_float(static_cast<uint16_t>((uv_packed >> 16) & 0xFFFF));
                verts[i].v = half_to_float(static_cast<uint16_t>(uv_packed & 0xFFFF));
                extra += 4;
            } else {
                verts[i].u = bits_to_float(vram_read32(vaddr + extra));
                verts[i].v = bits_to_float(vram_read32(vaddr + extra + 4));
                extra += 8;
            }
        }

        verts[i].base_color = vram_read32(vaddr + extra);
        extra += 4;

        if (isp.offset) {
            verts[i].offset_color = vram_read32(vaddr + extra);
        }
    }

    // For background rendering, override the depth compare to Always and force z_write
    // because the background fills behind everything.
    ISPTSPInstructionWord bg_isp = isp;
    bg_isp.depth_compare_mode = DepthCompareMode::Always;
    bg_isp.z_write_disable = false;

    // Override vertex Z to the background depth value. The drawing-time
    // Z is always ISP_BACKGND_D, regardless of what the 3 (or 4)
    // vertices have in their Z slots - the per-vertex Z values are
    // only there to parameterise attribute interpolation, not depth.
    for (int i = 0; i < 3; i++) {
        verts[i].z = bg_depth;
    }

    // The background is a *quad*, not a triangle: the three supplied
    // vertices are three corners of a parallelogram, and the hardware
    // synthesises the fourth corner so the background fills the
    // entire screen. KOS relies on this - it only supplies three
    // screen-space vertices (typically (0,H), (0,0), (W,H)) and
    // expects the missing (W,0) corner to be filled in so the quad
    // covers the full viewport. Rasterising only the 3-vertex
    // triangle leaves half the screen unpainted, which is why we were
    // seeing a stray diagonal "ghost" triangle.
    //
    // Synthesise vertex 4 as v1 + v2 - v0: the opposite corner of
    // the parallelogram with v0 as the pivot vertex. The same formula
    // is used for every per-vertex attribute
    // (XY, UV, base/offset color) because they all lie on the same
    // plane parameterised by v0/v1/v2. (Vertex 4's Z is already
    // overridden to bg_depth below, matching the other three.)
    // The three supplied vertices are three corners of a
    // parallelogram with v0 as the "pivot" corner and v1 / v2 as
    // the two corners adjacent to it; the missing opposite corner
    // is therefore v1 + v2 - v0.
    //
    // For the typical KOS background {v0=(0,H), v1=(0,0), v2=(W,H)}
    // this gives v3 = (W, 0) - the upper-right corner - so the
    // four vertices together cover the entire viewport.
    Vertex v3c;
    v3c.x            = verts[1].x + verts[2].x - verts[0].x;
    v3c.y            = verts[1].y + verts[2].y - verts[0].y;
    v3c.z            = bg_depth;
    v3c.u            = verts[1].u + verts[2].u - verts[0].u;
    v3c.v            = verts[1].v + verts[2].v - verts[0].v;
    auto lerp_packed = [](uint32_t a, uint32_t b, uint32_t c) -> uint32_t {
        // result = b + c - a, per channel, with saturation to 0..255.
        // Mirrors the XY formula: the 4th corner's attributes are
        // the parallelogram extrapolation of v1 and v2 relative to
        // the pivot v0.
        uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            int ai = (a >> shift) & 0xFF;
            int bi = (b >> shift) & 0xFF;
            int ci = (c >> shift) & 0xFF;
            int oi = bi + ci - ai;
            if (oi < 0)   oi = 0;
            if (oi > 255) oi = 255;
            out |= (static_cast<uint32_t>(oi) & 0xFFu) << shift;
        }
        return out;
    };
    v3c.base_color   = lerp_packed(verts[0].base_color,
                                    verts[1].base_color,
                                    verts[2].base_color);
    v3c.offset_color = lerp_packed(verts[0].offset_color,
                                    verts[1].offset_color,
                                    verts[2].offset_color);

    // Rasterise the quad as two triangles. v0 is the corner opposite
    // the synthesised v3, so the two triangles that together tile
    // the parallelogram without overlap are:
    //   (v0, v1, v3)  and  (v0, v3, v2)
    // Together they cover the full parallelogram / viewport. Both use
    // depth_compare = Always with z_write enabled, so every pixel of
    // the tile that falls inside the quad gets bg_depth written to
    // the depth buffer and the BG color written to primary - filling
    // any area not covered by the scene's display list.
    bool bg_shadow = regs_ && regs_->isp_backgnd_shadow();
    rasterize_triangle(ctx, ListType::Opaque, bg_isp, tsp, tcw,
                       verts[0], verts[1], v3c,
                       tile_x, tile_y, bg_shadow);
    rasterize_triangle(ctx, ListType::Opaque, bg_isp, tsp, tcw,
                       verts[0], v3c, verts[2],
                       tile_x, tile_y, bg_shadow);
}

// ============================================================================
// process_list - Walk object pointer blocks
// ============================================================================

void CORE::process_list(TileContext& ctx, ListType list_type,
                        uint32_t obj_list_addr, bool /*is_last_region*/) {
    uint32_t addr = obj_list_addr;

    // Hard cap: even a maximally-stuffed scene shouldn't push more than a
    // few thousand objects through a single tile. Anything beyond this is
    // almost certainly a corrupted OPB chain (e.g. a self-referential
    // PointerBlockLink) and would otherwise spin forever.
    constexpr unsigned int MAX_OBJECTS_PER_TILE = 65536;
    constexpr unsigned int MAX_LINK_FOLLOWS    = 4096;
    unsigned int processed = 0;
    unsigned int links     = 0;
    thread_local std::vector<uint32_t> visited;
    visited.clear();
    visited.push_back(obj_list_addr);

    for (;;) {
        uint32_t word = vram_read32(addr);

        // An all-zero slot means the OPB was never populated (no finalize_list
        // marker was written). Treat it as end-of-list to avoid reading past
        // the valid portion of the object list.
        if (word == 0) break;

        ObjectPointer op = ObjectPointer::parse(word);

        if (op.type == ObjectPointer::Type::PointerBlockLink) {
            if (op.end_of_list) break;
            uint32_t next = op.next_pointer * 4;
            // A block already walked means the chain loops (A -> B -> A
            // too, not just a self link): every lap would draw the same
            // objects again, which looks like a hang
            bool again = next == addr || ++links > MAX_LINK_FOLLOWS;
            for (std::size_t i = 0; !again && i < visited.size(); ++i) {
                again = visited[i] == next;
            }
            if (again) {
                static std::atomic<unsigned int> reported{0};
                if (reported.fetch_add(1) < 20) {
                    std::fprintf(stderr, "pvr: tile %d,%d list %d: OPB chain loops at 0x%06x after %u links, %u objects\n",
                                 ctx.tile_x, ctx.tile_y, static_cast<int>(list_type), next, links, processed);
                }
                isp_out_of_cache = true;
                break;
            }
            visited.push_back(next);
            addr = next;
            continue;
        }

        // Process the object (triangle strip, triangle array, or quad array)
        process_object(ctx, list_type, op);
        addr += 4;

        if (++processed > MAX_OBJECTS_PER_TILE) {
            // More objects in a single tile than any real scene produces;
            // we're almost certainly walking data that isn't an object list.
            static std::atomic<unsigned int> reported{0};
            if (reported.fetch_add(1) < 20) {
                std::fprintf(stderr, "pvr: tile %d,%d list %d: over %u objects, %u links\n",
                             ctx.tile_x, ctx.tile_y, static_cast<int>(list_type), MAX_OBJECTS_PER_TILE, links);
            }
            isp_out_of_cache = true;
            break;
        }
    }
}

// ============================================================================
// process_object
// ============================================================================

void CORE::process_object(TileContext& ctx, ListType list_type,
                          const ObjectPointer& obj) {
    // Modifier Volume lists do not carry TSP/TCW or colour data; they are
    // handled by a dedicated path that accumulates the area stencil rather
    // than shading pixels.
    if (list_type == ListType::OpaqueModifierVolume ||
        list_type == ListType::TranslucentModifierVolume) {
        process_modifier_object(ctx, obj);
        return;
    }

    // Absolute byte address of the ISP/TSP parameter block.
    // Object Pointer start_address is a 32-bit word address relative to PARAM_BASE.
    uint32_t param_addr = (regs_ ? regs_->read(RegisterName::PARAM_BASE) : 0u) + obj.start_address * 4;

    ISPTSPInstructionWord isp{};
    TSPInstructionWord    tsp{};
    TextureControlWord    tcw{};

    // Tile coordinates for rasterization come from ctx.tile_x/ctx.tile_y,
    // which were set by render_tile() before process_list() dispatched us.


    switch (obj.type) {
    case ObjectPointer::Type::TriangleStrip: {
        // Determine number of triangles from mask (bit 5 = triangle 0,
        // bit 4 = triangle 1, ... bit 0 = triangle 5).
        int max_tri = -1;
        for (int i = 0; i < 6; i++) {
            if (obj.mask & (1 << (5 - i))) {
                max_tri = i;
            }
        }
        if (max_tri < 0) return; // no triangles set in mask

        int num_verts = max_tri + 3;

        read_isp_tsp_params(param_addr, obj.skip, num_verts, isp, tsp, tcw, ctx.vertex_buf);

        // Rasterize each masked triangle from the strip
        for (int i = 0; i <= max_tri; i++) {
            if (!(obj.mask & (1 << (5 - i)))) continue;

            // Alternate winding order for triangle strips
            if ((i & 1) == 0) {
                rasterize_triangle(ctx, list_type, isp, tsp, tcw,
                                   ctx.vertex_buf[i], ctx.vertex_buf[i + 1], ctx.vertex_buf[i + 2],
                                   ctx.tile_x, ctx.tile_y, obj.shadow);
            } else {
                rasterize_triangle(ctx, list_type, isp, tsp, tcw,
                                   ctx.vertex_buf[i + 1], ctx.vertex_buf[i], ctx.vertex_buf[i + 2],
                                   ctx.tile_x, ctx.tile_y, obj.shadow);
            }
        }
        break;
    }

    case ObjectPointer::Type::TriangleArray: {
        int num_triangles = obj.num_primitives + 1;
        // Each primitive in a Triangle Array has its own complete
        // ISP/TSP/TCW parameter block followed by its own vertex data.
        // The block stride is: 12 bytes (ISP+TSP+TCW) + 3*(skip+3)*4
        // bytes (vertices).  This is how real hardware stores stacked
        // Triangle Array OPs: N+1 consecutive complete blocks starting
        // at the start_address, NOT a single header followed by N*3
        // vertices.
        uint32_t block_stride = 12u +
            3u * static_cast<uint32_t>(obj.skip + 3) * 4u;

        for (int i = 0; i < num_triangles; i++) {
            uint32_t block_addr = param_addr +
                static_cast<uint32_t>(i) * block_stride;
            ISPTSPInstructionWord tri_isp{};
            TSPInstructionWord    tri_tsp{};
            TextureControlWord    tri_tcw{};
            read_isp_tsp_params(block_addr, obj.skip, 3,
                                tri_isp, tri_tsp, tri_tcw, ctx.vertex_buf);
            rasterize_triangle(ctx, list_type, tri_isp, tri_tsp, tri_tcw,
                               ctx.vertex_buf[0], ctx.vertex_buf[1], ctx.vertex_buf[2],
                               ctx.tile_x, ctx.tile_y, obj.shadow);
        }
        break;
    }

    case ObjectPointer::Type::QuadArray: {
        int num_quads = obj.num_primitives + 1;
        int num_verts = num_quads * 4;

        read_isp_tsp_params(param_addr, obj.skip, num_verts, isp, tsp, tcw, ctx.vertex_buf);

        for (int i = 0; i < num_quads; i++) {
            rasterize_quad(ctx, list_type, isp, tsp, tcw,
                           ctx.vertex_buf[i * 4], ctx.vertex_buf[i * 4 + 1],
                           ctx.vertex_buf[i * 4 + 2], ctx.vertex_buf[i * 4 + 3],
                           ctx.tile_x, ctx.tile_y, obj.shadow);
        }
        break;
    }

    default:
        break;
    }
}

// ============================================================================
// read_isp_tsp_params - Read control words and vertex data from VRAM
// ============================================================================

void CORE::read_isp_tsp_params(uint32_t addr, int skip, int vertex_count,
                                ISPTSPInstructionWord& isp,
                                TSPInstructionWord& tsp,
                                TextureControlWord& tcw,
                                std::vector<Vertex>& vertices) const {
    // Read control words
    isp = ISPTSPInstructionWord::parse(vram_read32(addr));
    tsp = TSPInstructionWord::parse(vram_read32(addr + 4));

    // TCW is always present even for non-textured polygons (value is unused in that case)
    tcw = TextureControlWord::parse(vram_read32(addr + 8));
    if (!isp.texture) tcw = TextureControlWord{};
    uint32_t control_bytes = 12; // ISP + TSP + TCW (always 12 bytes)

    // Read vertex data
    uint32_t words_per_vertex = static_cast<uint32_t>(skip + 3);
    vertices.resize(vertex_count);

    for (int i = 0; i < vertex_count; i++) {
        uint32_t vaddr = addr + control_bytes + i * words_per_vertex * 4;
        Vertex& vtx = vertices[i];

        vtx.x = bits_to_float(vram_read32(vaddr + 0));
        vtx.y = bits_to_float(vram_read32(vaddr + 4));
        vtx.z = bits_to_float(vram_read32(vaddr + 8));
        vtx.u = 0.0f;
        vtx.v = 0.0f;
        vtx.base_color   = 0xFFFFFFFF;
        vtx.offset_color = 0x00000000;

        uint32_t extra = 12;

        if (isp.texture) {
            if (isp.uv_16bit) {
                uint32_t uv_packed = vram_read32(vaddr + extra);
                vtx.u = half_to_float(static_cast<uint16_t>((uv_packed >> 16) & 0xFFFF));
                vtx.v = half_to_float(static_cast<uint16_t>(uv_packed & 0xFFFF));
                extra += 4;
            } else {
                vtx.u = bits_to_float(vram_read32(vaddr + extra));
                vtx.v = bits_to_float(vram_read32(vaddr + extra + 4));
                extra += 8;
            }
        }

        vtx.base_color = vram_read32(vaddr + extra);
        extra += 4;

        if (isp.offset) {
            vtx.offset_color = vram_read32(vaddr + extra);
        }
    }

}

// ============================================================================
// isp_depth_test
// ============================================================================

bool CORE::isp_depth_test(TileContext& ctx, int x, int y, float z,
                          DepthCompareMode mode, bool z_write_disable) {
    float current_z = ctx.buf.depth[y][x];
    bool pass = false;

    switch (mode) {
    case DepthCompareMode::Never:          pass = false;              break;
    case DepthCompareMode::Less:           pass = (z < current_z);   break;
    case DepthCompareMode::Equal:          pass = (z == current_z);  break;
    case DepthCompareMode::LessOrEqual:    pass = (z <= current_z);  break;
    case DepthCompareMode::Greater:        pass = (z > current_z);   break;
    case DepthCompareMode::NotEqual:       pass = (z != current_z);  break;
    case DepthCompareMode::GreaterOrEqual: pass = (z >= current_z);  break;
    case DepthCompareMode::Always:         pass = true;              break;
    }

    if (pass && !z_write_disable) {
        ctx.buf.depth[y][x] = z;
    }

    return pass;
}

// ============================================================================
// tsp_shade_pixel
// ============================================================================

Pixel CORE::tsp_shade_pixel(const ISPTSPInstructionWord& isp,
                             const TSPInstructionWord& tsp,
                             const TextureControlWord& tcw,
                             float u, float v,
                             uint32_t base_color,
                             uint32_t offset_color,
                             int /*pixel_x*/, int /*pixel_y*/,
                             float lod,
                             bool texel_half_offset) const {
    Pixel col = Pixel::from_packed(base_color);
    Pixel off = Pixel::from_packed(offset_color);

    // For bump-mapped polygons the offset_color slot holds K1K2K3Q, not a
    // real colour offset.  Zero it out here so it doesn't bleed into the
    // tex-shading path below; it will be consumed by the bump algorithm.
    if (isp.texture && tcw.pixel_format == TexturePixelFormat::BumpMap) {
        off = Pixel{0, 0, 0, 0};
    }

    if (!tsp.use_alpha) {
        col.a = 255;
    }

    if (!isp.texture) {
        // Non-textured: just return the base color
        return col;
    }

    // Sample texture
    Pixel tex = texture_sampler_.sample(tsp, tcw, u, v, lod, texel_half_offset);

    // -----------------------------------------------------------------------
    // PVR bump mapping
    //
    // The texture sampler stores the raw 8-bit S angle in tex.r and the
    // raw 8-bit R angle in tex.g (see texture.cpp BumpMap case).
    //
    // The polygon's offset_color field (passed in as `offset_color`) carries
    // the per-polygon light-source parameters packed as:
    //   bits 31-24 : K1  (1 - strength),            0x00 = 0.0, 0xFF = 1.0
    //   bits 23-16 : K2  (strength * sin(t')),       0x00 = 0.0, 0xFF = 1.0
    //   bits 15-8  : K3  (strength * cos(t')),       0x00 = 0.0, 0xFF = 1.0
    //   bits  7-0  : Q   (light azimuth angle),      0x00 = 0°,  0xFF ≈ 360°
    //
    // The brightness for each texel is:
    //   s' = (π/2) * S / 256
    //   r' = 2π   * R / 256
    //   q' = 2π   * Q / 256
    //   α  = K1 + K2*sin(s') + K3*cos(s')*cos(r' - q')   (clamped to [0,1])
    //
    // The final texel output is opaque white (R=G=B=0xFF) with the computed
    // brightness packed into the alpha channel.  The tex-shading instruction
    // (normally Decal Alpha) then blends this against the Base Color.
    // -----------------------------------------------------------------------
    if (tcw.pixel_format == TexturePixelFormat::BumpMap) {
        // K1, K2, K3, Q from the per-polygon offset_color word
        const uint8_t k1_byte = static_cast<uint8_t>((offset_color >> 24) & 0xFF);
        const uint8_t k2_byte = static_cast<uint8_t>((offset_color >> 16) & 0xFF);
        const uint8_t k3_byte = static_cast<uint8_t>((offset_color >>  8) & 0xFF);
        const uint8_t q_byte  = static_cast<uint8_t>( offset_color        & 0xFF);

        // S (elevation) and R (azimuth) from the sampled texel
        const uint8_t s_byte = tex.r;
        const uint8_t r_byte = tex.g;

        // Convert to radians
        constexpr float PI       = 3.14159265358979323846f;
        constexpr float HALF_PI  = PI * 0.5f;
        constexpr float TWO_PI   = PI * 2.0f;
        const float s_rad = static_cast<float>(s_byte) * (HALF_PI / 256.0f);
        const float r_rad = static_cast<float>(r_byte) * (TWO_PI  / 256.0f);
        const float q_rad = static_cast<float>(q_byte) * (TWO_PI  / 256.0f);

        // Normalise K1/K2/K3 to [0, 1]
        const float k1 = static_cast<float>(k1_byte) / 255.0f;
        const float k2 = static_cast<float>(k2_byte) / 255.0f;
        const float k3 = static_cast<float>(k3_byte) / 255.0f;

        // Bump mapping brightness equation
        float brightness = k1
                         + k2 * std::sin(s_rad)
                         + k3 * std::cos(s_rad) * std::cos(r_rad - q_rad);
        brightness = std::max(0.0f, std::min(1.0f, brightness));

        // Output: white texel whose alpha encodes the brightness
        tex.r = 0xFF;
        tex.g = 0xFF;
        tex.b = 0xFF;
        tex.a = static_cast<uint8_t>(brightness * 255.0f + 0.5f);

        // ignore_tex_alpha does not apply to bump maps (it would destroy the
        // carefully computed brightness value), so skip that path entirely.
    } else {
        if (tsp.ignore_tex_alpha) {
            tex.a = 255;
        }
    }

    Pixel result{};

    switch (tsp.tex_shading_instr) {
    case TexShadingInstr::Decal:
        // PIX_RGB = TEX_RGB + OFFSET_RGB, PIX_A = TEX_A
        result.r = tex.r;
        result.g = tex.g;
        result.b = tex.b;
        result.a = tex.a;
        result = result.add_sat(Pixel{0, off.r, off.g, off.b});
        break;

    case TexShadingInstr::Modulate:
        // PIX_RGB = COL_RGB * TEX_RGB + OFFSET_RGB, PIX_A = TEX_A
        result = col.modulate(tex);
        result.a = tex.a;
        result = result.add_sat(Pixel{0, off.r, off.g, off.b});
        break;

    case TexShadingInstr::DecalAlpha:
        // PIX_RGB = TEX_RGB * TEX_A + COL_RGB * (1-TEX_A) + OFFSET_RGB
        // PIX_A = COL_A
        {
            uint8_t ta = tex.a;
            uint8_t ita = 255 - ta;
            result.r = static_cast<uint8_t>((tex.r * ta + col.r * ita + 127) / 255);
            result.g = static_cast<uint8_t>((tex.g * ta + col.g * ita + 127) / 255);
            result.b = static_cast<uint8_t>((tex.b * ta + col.b * ita + 127) / 255);
            result.a = col.a;
            result = result.add_sat(Pixel{0, off.r, off.g, off.b});
        }
        break;

    case TexShadingInstr::ModulateAlpha:
        // PIX_RGB = COL_RGB * TEX_RGB + OFFSET_RGB, PIX_A = COL_A * TEX_A
        result = col.modulate(tex);
        result.a = static_cast<uint8_t>((static_cast<uint16_t>(col.a) * tex.a + 127) / 255);
        result = result.add_sat(Pixel{0, off.r, off.g, off.b});
        break;
    }

    return result;
}

// ============================================================================
// alpha_blend
// ============================================================================

void CORE::alpha_blend(TileContext& ctx, int x, int y,
                       const Pixel& src, const TSPInstructionWord& tsp) {
    // When SRC Select = 1 the secondary accumulation buffer replaces the
    // shaded pixel as the source.  When DST Select = 1 the secondary
    // accumulation buffer is both the blend destination and the write target;
    // otherwise the primary accumulation buffer serves both roles.
    Pixel src_pixel = tsp.src_select ? ctx.buf.secondary[y][x] : src;
    Pixel dst_pixel = tsp.dst_select ? ctx.buf.secondary[y][x] : ctx.buf.primary[y][x];

    // Compute blend factors
    Pixel src_factor = compute_blend_factor(tsp.src_alpha_instr, src_pixel, dst_pixel, true);
    Pixel dst_factor = compute_blend_factor(tsp.dst_alpha_instr, src_pixel, dst_pixel, false);

    // result = src * src_factor + dst * dst_factor  (with saturation)
    Pixel result = src_pixel.modulate(src_factor).add_sat(dst_pixel.modulate(dst_factor));

    // Write back to whichever buffer was selected as the destination.
    if (tsp.dst_select) {
        ctx.buf.secondary[y][x] = result;
    } else {
        ctx.buf.primary[y][x] = result;
    }
}

// ============================================================================
// apply_fog
// ============================================================================

Pixel CORE::apply_fog(const Pixel& pixel_color, const TSPInstructionWord& tsp,
                       float z_value, uint32_t offset_color) const {
    switch (tsp.fog_control) {
    case FogControl::LookUpTable: {
        float fog_alpha = fog_table_lookup(z_value);
        uint32_t fog_col_raw = regs_->read(RegisterName::FOG_COL_RAM);
        Pixel fog_col = Pixel::from_packed(fog_col_raw | 0xFF000000u);
        uint8_t fog_a = static_cast<uint8_t>(std::min(fog_alpha * 255.0f, 255.0f));
        // fogged = pixel * (1 - fog_alpha) + fog_col * fog_alpha, the colour
        // only: the alpha stays (punch-through's test and the blend take it)
        Pixel fogged = pixel_color.lerp(fog_col, fog_a);
        fogged.a = pixel_color.a;
        return fogged;
    }
    case FogControl::PerVertex: {
        uint8_t fog_a = static_cast<uint8_t>((offset_color >> 24) & 0xFF);
        uint32_t fog_col_raw = regs_->read(RegisterName::FOG_COL_VERT);
        Pixel fog_col = Pixel::from_packed(fog_col_raw | 0xFF000000u);
        Pixel fogged = pixel_color.lerp(fog_col, fog_a);
        fogged.a = pixel_color.a;
        return fogged;
    }
    case FogControl::NoFog:
        return pixel_color;
    case FogControl::LookUpTableMode2: {
        // Replace alpha with fog_alpha, RGB with fog color
        float fog_alpha = fog_table_lookup(z_value);
        uint32_t fog_col_raw = regs_->read(RegisterName::FOG_COL_RAM);
        Pixel result = pixel_color;
        result.a = static_cast<uint8_t>(std::min(fog_alpha * 255.0f, 255.0f));
        result.r = static_cast<uint8_t>((fog_col_raw >> 16) & 0xFF);
        result.g = static_cast<uint8_t>((fog_col_raw >> 8) & 0xFF);
        result.b = static_cast<uint8_t>(fog_col_raw & 0xFF);
        return result;
    }
    default:
        return pixel_color;
    }
}

// ============================================================================
// apply_color_clamp
// ============================================================================

Pixel CORE::apply_color_clamp(const Pixel& color) const {
    uint32_t clamp_min_raw = regs_->read(RegisterName::FOG_CLAMP_MIN);
    uint32_t clamp_max_raw = regs_->read(RegisterName::FOG_CLAMP_MAX);
    Pixel lo = Pixel::from_packed(clamp_min_raw);
    Pixel hi = Pixel::from_packed(clamp_max_raw);
    return color.clamped(lo, hi);
}

// ============================================================================
// fog_table_lookup
// ============================================================================

float CORE::fog_table_lookup(float z_value) const {
    // FOG_DENSITY: a 16 bit float, bits 14-8 the mantissa's top 7 bits
    // (1.mmmmmmm), 7-0 the exponent (two's complement); KOS sets bit 15
    // (pvr_fog_far_depth writes the depth negated) and it means nothing here
    uint32_t dreg = regs_->read(RegisterName::FOG_DENSITY);
    float fog_density = std::ldexp(1.0f + ((dreg >> 8) & 0x7f) / 128.0f, static_cast<int8_t>(dreg & 0xff));

    // Compute lookup value
    float val = std::abs(z_value) * fog_density;

    // Clamp to the representable fog table range
    val = std::max(1.0f, std::min(val, 255.9999f));

    // Extract table index from the IEEE 754 bit representation
    uint32_t val_bits;
    std::memcpy(&val_bits, &val, sizeof(float));

    int exp  = static_cast<int>(((val_bits >> 23) & 0xFF)) - 127; // unbiased exponent
    int mant_top4 = static_cast<int>((val_bits >> 19) & 0xF);     // top 4 mantissa bits

    // 7-bit table index: [exp bits 2:0][mant bits 3:0]
    int table_index = ((exp & 0x7) << 4) | mant_top4;
    table_index = std::max(0, std::min(127, table_index));

    // Read the fog table entry (each 32-bit register holds two 8-bit coefficients)
    uint32_t entry = regs_->fog_table(table_index);
    uint8_t coeff_this = static_cast<uint8_t>((entry >> 8) & 0xFF);
    uint8_t coeff_next = static_cast<uint8_t>(entry & 0xFF);

    // Interpolation fraction from the next 8 mantissa bits
    float frac = static_cast<float>((val_bits >> 11) & 0xFF) / 255.0f;

    float fog_alpha = (coeff_this * (1.0f - frac) + coeff_next * frac) / 255.0f;
    return std::max(0.0f, std::min(1.0f, fog_alpha));
}

// ============================================================================
// compute_mip_lod
//
// Compute a per-triangle mip LOD from the triangle's affine UV derivatives.
//
// Algorithm (Dcalc Ctrl = 0 approximation, simplified):
//   The affine UV gradient is constant across the triangle and equals the
//   perspective-correct gradient evaluated at infinite depth (no foreshortening
//   correction).  For LOD selection this is adequate: the LOD thresholds are
//   powers of two and the per-pixel variation within a single 32×32 tile is
//   small relative to those thresholds.
//
// The barycentric weight gradients are derived from the triangle's edge
// functions:
//   dw0/dx = (v1.y − v2.y) / area,  dw0/dy = (v2.x − v1.x) / area
//   dw1/dx = (v2.y − v0.y) / area,  dw1/dy = (v0.x − v2.x) / area
//   dw2/dx = (v0.y − v1.y) / area,  dw2/dy = (v1.x − v0.x) / area
//
// LOD mapping (texels-per-pixel):
//   D  = max( sqrt(dudx²+dvdx²), sqrt(dudy²+dvdy²) )   [texel space]
//   lod = floor( log2( max(D, 1.0) ) )
//   → D in [1, 2)  → lod 0  (≤1 texel per pixel, use full res)
//   → D in [2, 4)  → lod 1  (≥2 texels per pixel, use half-res mip)
//   → D in [4, 8)  → lod 2  etc.
//
// The MIP-MAP D adjust field (TSP bits 11-8, 2.2 unsigned fixed-point) is
// applied as a multiplier on D before the log2.  A raw value of 0x0 is
// illegal; it is treated as 1.0.
// ============================================================================

float CORE::compute_mip_lod(const TSPInstructionWord& tsp,
                             const TextureControlWord& tcw,
                             const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    // Non-twiddled (stride) textures cannot be mipmapped.
    if (!tcw.mip_mapped || tcw.scan_order) return 0.0f;

    // Signed 2× screen-space area of the triangle.
    float area = edge_function(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (std::abs(area) < 1e-6f) return 0.0f;
    const float inv_area = 1.0f / area;

    // Barycentric weight gradients in screen-pixel space.
    const float dw0dx = (v1.y - v2.y) * inv_area;
    const float dw1dx = (v2.y - v0.y) * inv_area;
    const float dw2dx = (v0.y - v1.y) * inv_area;

    const float dw0dy = (v2.x - v1.x) * inv_area;
    const float dw1dy = (v0.x - v2.x) * inv_area;
    const float dw2dy = (v1.x - v0.x) * inv_area;

    // Affine UV gradients in [0,1] UV space, scaled to texel units.
    const float uf = static_cast<float>(tsp.texture_u_pixels());
    const float vf = static_cast<float>(tsp.texture_v_pixels());

    const float dudx = (dw0dx * v0.u + dw1dx * v1.u + dw2dx * v2.u) * uf;
    const float dvdx = (dw0dx * v0.v + dw1dx * v1.v + dw2dx * v2.v) * vf;
    const float dudy = (dw0dy * v0.u + dw1dy * v1.u + dw2dy * v2.u) * uf;
    const float dvdy = (dw0dy * v0.v + dw1dy * v1.v + dw2dy * v2.v) * vf;

    // D = max(|dUV/dx|, |dUV/dy|) measured in texels per screen pixel.
    const float dx = std::sqrt(dudx * dudx + dvdx * dvdx);
    const float dy = std::sqrt(dudy * dudy + dvdy * dvdy);
    float D = std::max(dx, dy);

    // Apply MIP-MAP D adjust (TSP bits 11-8, 2.2 unsigned fixed-point).
    // A raw value of 0x0 is illegal; treat as 1.0 (no scaling).
    const uint8_t d_adj = tsp.mipmap_d_adjust;
    if (d_adj != 0) {
        D *= static_cast<float>(d_adj) * 0.25f;  // 2.2 fixed-point → float
    }

    // lod = floor( log2( max(D, 1.0) ) )
    //   D < 1 → lod 0 (texture appears larger than native, use full res)
    //   D ∈ [1,2)  → lod 0
    //   D ∈ [2,4)  → lod 1
    //   D ∈ [4,8)  → lod 2  …
    const float lod_f = std::max(0.0f, std::floor(std::log2f(std::max(D, 1.0f))));

    // Clamp to [0, max_lod] for this texture's base size.
    int max_lod = 0;
    for (int s = static_cast<int>(std::max(tsp.texture_u_pixels(),
                                           tsp.texture_v_pixels())); s > 1; s >>= 1) {
        max_lod++;
    }
    return std::min(lod_f, static_cast<float>(max_lod));
}

// ============================================================================
// rasterize_triangle
// ============================================================================

void CORE::rasterize_triangle(TileContext& ctx, ListType list_type,
                               const ISPTSPInstructionWord& isp,
                               const TSPInstructionWord& tsp,
                               const TextureControlWord& tcw,
                               const Vertex& v0, const Vertex& v1, const Vertex& v2,
                               int tile_x, int tile_y, bool shadowed) {
    // Tile pixel origin
    float tile_px = static_cast<float>(tile_x * static_cast<int>(TILE_SIZE));
    float tile_py = static_cast<float>(tile_y * static_cast<int>(TILE_SIZE));

    // Triangle bounding box in screen space
    float min_x = std::min({v0.x, v1.x, v2.x});
    float max_x = std::max({v0.x, v1.x, v2.x});
    float min_y = std::min({v0.y, v1.y, v2.y});
    float max_y = std::max({v0.y, v1.y, v2.y});

    // Clip bounding box to tile
    int ix_min = std::max(0, static_cast<int>(std::floor(min_x - tile_px)));
    int iy_min = std::max(0, static_cast<int>(std::floor(min_y - tile_py)));
    int ix_max = std::min(static_cast<int>(TILE_SIZE) - 1, static_cast<int>(std::floor(max_x - tile_px)));
    int iy_max = std::min(static_cast<int>(TILE_SIZE) - 1, static_cast<int>(std::floor(max_y - tile_py)));

    if (ix_min > ix_max || iy_min > iy_max) return;

    // Area of the triangle (2x signed area via edge function). This
    // is the "det" value used by the ISP's culling stage: a positive
    // area corresponds to CCW winding in screen space, a negative
    // area to CW. Modifier volumes never cull (they always
    // use NoCulling regardless of what the ISP word says).
    float area = edge_function(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (std::abs(area) < 1e-6f) {
        return; // degenerate triangle
    }

    // ISP back-face / small-triangle culling:
    //   0 = No Culling
    //   1 = Cull if Small     : cull if |det| < fpu_cull_val
    //   2 = Cull if Negative  : cull if det <  0 OR |det| < fpu_cull_val
    //   3 = Cull if Positive  : cull if det >  0 OR |det| < fpu_cull_val
    // fpu_cull_val comes from the FPU_CULL_VAL register (IEEE float).
    //
    // This applies to Opaque / Translucent / PunchThrough triangles
    // but NOT to modifier volumes (their ISP word uses the same bits
    // differently; the rasterizer is called for non-MV lists only in
    // practice, but we still gate by list_type to be safe).
    if (list_type != ListType::OpaqueModifierVolume &&
        list_type != ListType::TranslucentModifierVolume) {
        float fpu_cull_val = 0.0f;
        if (regs_) {
            fpu_cull_val = bits_to_float(
                regs_->read(RegisterName::FPU_CULL_VAL));
        }
        const float abs_area = std::fabs(area);
        switch (isp.culling_mode) {
            case CullingMode::NoCulling:
                break;
            case CullingMode::CullIfSmall:
                if (abs_area < fpu_cull_val) return;
                break;
            case CullingMode::CullIfNegative:
                // Cull if det < 0 (back-facing) OR |det| < fpu_cull_val (too small).
                if (area < 0.0f || abs_area < fpu_cull_val) return;
                break;
            case CullingMode::CullIfPositive:
                if (area > 0.0f || abs_area < fpu_cull_val) return;
                break;
        }
    }

    float inv_area = 1.0f / area;

    // Top-left fill rule. A pixel sample that lands exactly on a
    // triangle edge is ambiguous: both triangles sharing that edge
    // would otherwise claim it, which produces a visible seam along
    // the shared diagonal of triangle-strip quads -- especially when
    // the quad is being alpha-blended in or out, because the seam
    // pixel ends up with its alpha applied twice.
    //
    // The standard fix (used by D3D / GL rasterizers) is: a pixel on
    // an edge is considered inside only if that edge is a "top" or
    // "left" edge of the triangle (in screen space). Top edges are
    // perfectly horizontal with the triangle below them; left edges
    // go "downward" from the triangle's perspective. With a positive
    // 2x-area (CCW), an edge (a->b) is:
    //   * top  if dy == 0 and dx < 0
    //   * left if dy  > 0
    // For negative-area (CW) triangles we flip the test so the same
    // winding rule applies.
    //
    // We pre-compute one bias per edge: -1 for non-fill (strict >),
    // 0 for fill (>=). Because the barycentric weights w0/w1/w2 are
    // already normalised by inv_area (so the sign of area doesn't
    // affect whether they are positive inside), we fold the rule in
    // by nudging each weight down by a tiny epsilon when the edge
    // is NOT a fill edge. Using inv_area scaled epsilon keeps the
    // bias in the same units as the normalised weights.
    auto edge_is_fill = [](float ax, float ay, float bx, float by,
                           float signed_area) -> bool {
        float dx = bx - ax;
        float dy = by - ay;
        // Flip for CW triangles so the rule is winding-independent.
        if (signed_area < 0.0f) { dx = -dx; dy = -dy; }
        // Top edge: horizontal and going left.
        if (dy == 0.0f && dx < 0.0f) return true;
        // Left edge: going upward in screen space (decreasing y,
        // since positive y points down in screen coordinates).
        return dy < 0.0f;
    };
    const bool fill_e0 = edge_is_fill(v1.x, v1.y, v2.x, v2.y, area);
    const bool fill_e1 = edge_is_fill(v2.x, v2.y, v0.x, v0.y, area);
    const bool fill_e2 = edge_is_fill(v0.x, v0.y, v1.x, v1.y, area);
    // Epsilon small enough to not visibly move edges but large enough
    // to reliably exclude the losing triangle at shared samples.
    const float edge_eps = 1.0e-5f;
    const float bias0 = fill_e0 ? 0.0f : -edge_eps;
    const float bias1 = fill_e1 ? 0.0f : -edge_eps;
    const float bias2 = fill_e2 ? 0.0f : -edge_eps;

    // ---- HALF_OFFSET register (0x005F8080) ----------------------------------
    // Bit 2: TSP texel sampling position  (1 = (0.5,0.5) centre bias, default).
    // Bit 1: TSP pixel sampling position  (1 = pixel centre +0.5 offset, default).
    // Read once per triangle; defaults to all-enabled (0x7) when regs_ is null.
    const uint32_t half_off_reg = regs_ ? regs_->read(RegisterName::HALF_OFFSET) : 0x7u;
    const bool texel_half  = (half_off_reg >> 2) & 1u;
    const float pixel_off  = ((half_off_reg >> 1) & 1u) ? 0.5f : 0.0f;

    // ---- Mip LOD (constant across the triangle) -----------------------------
    // Only meaningful for mipmapped twiddled textures; 0.0 otherwise.
    // PVR_NOMIP=1: always the full size level, to see what mip choice does
    static const bool no_mip = std::getenv("PVR_NOMIP") != nullptr;
    const float mip_lod = (isp.texture && tcw.mip_mapped && !tcw.scan_order && !no_mip)
                          ? compute_mip_lod(tsp, tcw, v0, v1, v2)
                          : 0.0f;

    // Punch-through alpha reference
    uint32_t pt_alpha_ref = 0;
    if (list_type == ListType::PunchThrough && regs_) {
        pt_alpha_ref = regs_->read(RegisterName::PT_ALPHA_REF) & 0xFF;
    }

    // Hoist per-triangle invariants out of the pixel loop.
    const Pixel c0 = Pixel::from_packed(v0.base_color);
    const Pixel c1 = Pixel::from_packed(v1.base_color);
    const Pixel c2 = Pixel::from_packed(v2.base_color);
    const Pixel o0 = isp.offset ? Pixel::from_packed(v0.offset_color) : Pixel{};
    const Pixel o1 = isp.offset ? Pixel::from_packed(v1.offset_color) : Pixel{};
    const Pixel o2 = isp.offset ? Pixel::from_packed(v2.offset_color) : Pixel{};
    // Flat shading uses the third vertex's color (no per-pixel work needed).
    const uint32_t flat_base_col = v2.base_color;
    const uint32_t flat_off_col  = isp.offset ? v2.offset_color : 0u;

    for (int ly = iy_min; ly <= iy_max; ly++) {
        for (int lx = ix_min; lx <= ix_max; lx++) {
            // Pixel sample point in screen coordinates.
            // The HALF_OFFSET register (bit 1) controls whether the sample is
            // at the pixel centre (+0.5) or the pixel corner (+0.0).
            float px = tile_px + static_cast<float>(lx) + pixel_off;
            float py = tile_py + static_cast<float>(ly) + pixel_off;

            // Barycentric coordinates via edge functions
            float w0 = edge_function(v1.x, v1.y, v2.x, v2.y, px, py) * inv_area;
            float w1 = edge_function(v2.x, v2.y, v0.x, v0.y, px, py) * inv_area;
            float w2 = edge_function(v0.x, v0.y, v1.x, v1.y, px, py) * inv_area;

            // Inside test with top-left fill rule applied via per-edge
            // bias: fill edges use >= 0, non-fill edges use > 0 (by
            // subtracting a tiny epsilon before the sign test). The
            // weights are already normalised by inv_area, so they are
            // positive inside regardless of the triangle winding.
            if (w0 + bias0 < 0.0f || w1 + bias1 < 0.0f || w2 + bias2 < 0.0f) continue;
            // Interpolate Z (1/W) linearly in screen space
            float z = interpolate_bary(v0.z, v1.z, v2.z, w0, w1, w2);

            // --- For translucent auto-sort: collect fragment, skip depth test ---
            if (ctx.auto_sort_active) {
                // Interpolate attributes
                float u = 0.0f, v_coord = 0.0f;
                if (isp.texture) {
                    const float wz0 = w0 * v0.z, wz1 = w1 * v1.z, wz2 = w2 * v2.z;
                    const float den = wz0 + wz1 + wz2;
                    const float inv_den = (std::abs(den) > 1e-12f) ? (1.0f / den) : 0.0f;
                    u       = (v0.u * wz0 + v1.u * wz1 + v2.u * wz2) * inv_den;
                    v_coord = (v0.v * wz0 + v1.v * wz1 + v2.v * wz2) * inv_den;
                }

                // Interpolate colors
                uint32_t base_col, off_col;
                if (isp.gouraud) {
                    Pixel ci{
                        lerp8(c0.a, c1.a, c2.a, w0, w1, w2),
                        lerp8(c0.r, c1.r, c2.r, w0, w1, w2),
                        lerp8(c0.g, c1.g, c2.g, w0, w1, w2),
                        lerp8(c0.b, c1.b, c2.b, w0, w1, w2)
                    };
                    base_col = ci.to_packed();
                } else {
                    base_col = flat_base_col; // flat shading uses last vertex
                }

                if (isp.offset && isp.gouraud) {
                    Pixel oi{
                        lerp8(o0.a, o1.a, o2.a, w0, w1, w2),
                        lerp8(o0.r, o1.r, o2.r, w0, w1, w2),
                        lerp8(o0.g, o1.g, o2.g, w0, w1, w2),
                        lerp8(o0.b, o1.b, o2.b, w0, w1, w2)
                    };
                    off_col = oi.to_packed();
                } else {
                    off_col = flat_off_col;
                }

                int screen_x = tile_x * static_cast<int>(TILE_SIZE) + lx;
                int screen_y = tile_y * static_cast<int>(TILE_SIZE) + ly;
                Pixel shaded = tsp_shade_pixel(isp, tsp, tcw, u, v_coord, base_col, off_col, screen_x, screen_y, mip_lod, texel_half);

                // Apply fog
                if (tsp.fog_control != FogControl::NoFog) {
                    shaded = apply_fog(shaded, tsp, z, off_col);
                }

                // Apply color clamping
                if (tsp.color_clamp) {
                    shaded = apply_color_clamp(shaded);
                }

                TranslucentFragment frag;
                frag.z = z;
                frag.color = shaded;
                frag.x = lx;
                frag.y = ly;
                frag.tsp = tsp;
                frag.z_write_disable = isp.z_write_disable;
                ctx.translucent_fragments.push_back(frag);
                continue;
            }

            // --- ISP depth test ---
            bool depth_pass;
            if (list_type == ListType::PunchThrough) {
                // Punch-through: need to shade first to get alpha, then test
                // Use GreaterOrEqual depth test; only write depth if alpha passes
                depth_pass = isp_depth_test(ctx, lx, ly, z, isp.depth_compare_mode, true); // don't write depth yet
            } else {
                depth_pass = isp_depth_test(ctx, lx, ly, z, isp.depth_compare_mode, isp.z_write_disable);
            }

            if (!depth_pass) {
                continue;
            }

            // --- Interpolate attributes ---
            float u = 0.0f, v_coord = 0.0f;
            if (isp.texture) {
                const float wz0 = w0 * v0.z, wz1 = w1 * v1.z, wz2 = w2 * v2.z;
                const float den = wz0 + wz1 + wz2;
                const float inv_den = (std::abs(den) > 1e-12f) ? (1.0f / den) : 0.0f;
                u       = (v0.u * wz0 + v1.u * wz1 + v2.u * wz2) * inv_den;
                v_coord = (v0.v * wz0 + v1.v * wz1 + v2.v * wz2) * inv_den;
            }

            // Interpolate base color (Gouraud or flat)
            uint32_t base_col, off_col;
            if (isp.gouraud) {
                Pixel ci{
                    lerp8(c0.a, c1.a, c2.a, w0, w1, w2),
                    lerp8(c0.r, c1.r, c2.r, w0, w1, w2),
                    lerp8(c0.g, c1.g, c2.g, w0, w1, w2),
                    lerp8(c0.b, c1.b, c2.b, w0, w1, w2)
                };
                base_col = ci.to_packed();
            } else {
                base_col = flat_base_col; // flat shading: last vertex color
            }

            if (isp.offset && isp.gouraud) {
                Pixel oi{
                    lerp8(o0.a, o1.a, o2.a, w0, w1, w2),
                    lerp8(o0.r, o1.r, o2.r, w0, w1, w2),
                    lerp8(o0.g, o1.g, o2.g, w0, w1, w2),
                    lerp8(o0.b, o1.b, o2.b, w0, w1, w2)
                };
                off_col = oi.to_packed();
            } else {
                off_col = flat_off_col;
            }

            // --- TSP shading ---
            int screen_x = tile_x * static_cast<int>(TILE_SIZE) + lx;
            int screen_y = tile_y * static_cast<int>(TILE_SIZE) + ly;
            Pixel shaded = tsp_shade_pixel(isp, tsp, tcw, u, v_coord, base_col, off_col, screen_x, screen_y, mip_lod, texel_half);

            // --- Fog ---
            if (tsp.fog_control != FogControl::NoFog) {
                shaded = apply_fog(shaded, tsp, z, off_col);
            }

            // --- Color clamping ---
            if (tsp.color_clamp) {
                shaded = apply_color_clamp(shaded);
            }

            // --- Punch-through alpha test ---
            if (list_type == ListType::PunchThrough) {
                if (shaded.a < pt_alpha_ref) continue;
                // Alpha passed: now write depth
                if (!isp.z_write_disable) {
                    ctx.buf.depth[ly][lx] = z;
                }
            }

            // --- Write to accumulation buffer ---
            if (list_type == ListType::Opaque || list_type == ListType::PunchThrough) {
                ctx.buf.primary[ly][lx] = shaded;
                // Track whether the winning Opaque/PT pixel is shadow-enabled
                // so the modifier-volume pass knows which pixels to darken.
                if (shadowed) ctx.buf.stencil[ly][lx] |=  STENCIL_SHADOW;
                else          ctx.buf.stencil[ly][lx] &= ~STENCIL_SHADOW;
            } else if (list_type == ListType::Translucent) {
                // Pre-sort mode: blend directly
                alpha_blend(ctx, lx, ly, shaded, tsp);
            } else {
                ctx.buf.primary[ly][lx] = shaded;
            }
        }
    }
}

// ============================================================================
// rasterize_quad
// ============================================================================

void CORE::rasterize_quad(TileContext& ctx, ListType list_type,
                           const ISPTSPInstructionWord& isp,
                           const TSPInstructionWord& tsp,
                           const TextureControlWord& tcw,
                           const Vertex& v0, const Vertex& v1,
                           const Vertex& v2, const Vertex& v3,
                           int tile_x, int tile_y, bool shadowed) {

    // The Z value for the fourth vertex of a Quad polygon does not need
    // to be specified because it is generated in the CORE."
    //
    // v3 is assumed to lie on the plane defined by v0, v1, v2. We compute
    // v3.z (1/W) from the plane equation using the (x,y) position of v3.
    //
    // The plane is parameterized as p = v0 + a*(v1-v0) + b*(v2-v0), so:
    //   v3.x - v0.x = a*(v1.x - v0.x) + b*(v2.x - v0.x)
    //   v3.y - v0.y = a*(v1.y - v0.y) + b*(v2.y - v0.y)
    // Solve the 2x2 system for (a,b) and then:
    //   v3.z = v0.z + a*(v1.z - v0.z) + b*(v2.z - v0.z)
    Vertex v3c = v3;
    {
        float e1x = v1.x - v0.x;
        float e1y = v1.y - v0.y;
        float e2x = v2.x - v0.x;
        float e2y = v2.y - v0.y;
        float det = e1x * e2y - e1y * e2x;
        if (std::abs(det) > 1e-12f) {
            float dx = v3.x - v0.x;
            float dy = v3.y - v0.y;
            float a = (dx * e2y - dy * e2x) / det;
            float b = (e1x * dy - e1y * dx) / det;
            v3c.z = v0.z + a * (v1.z - v0.z) + b * (v2.z - v0.z);
        }
        // If the first three vertices are colinear, keep the incoming v3.z.
    }

    // Split quad into two triangles: (v0,v1,v2) and (v0,v2,v3c)
    rasterize_triangle(ctx, list_type, isp, tsp, tcw, v0, v1, v2,  tile_x, tile_y, shadowed);
    rasterize_triangle(ctx, list_type, isp, tsp, tcw, v0, v2, v3c, tile_x, tile_y, shadowed);
}

// ============================================================================
// process_modifier_object
//
// A Modifier Volume object holds one or more triangles (the TA emits a single
// triangle per object). The ISP/TSP Instruction Word reinterprets its top
// bits as a Volume Instruction:
//   0  Normal Polygon        -- a triangle that is not the last in the volume
//   1  Inside Last Polygon   -- last polygon of an inclusion volume
//   2  Outside Last Polygon  -- last polygon of an exclusion volume
//
// Modifier Volume parameter blocks carry only the ISP word (no TSP/TCW) and
// X/Y/Z-only vertices, so they cannot share process_object's read_isp_tsp_params
// path. Each triangle toggles the per-pixel parity (STENCIL_VOLPAR) wherever
// the volume surface lies in front of the stored opaque depth; on the volume's
// last polygon the accumulated parity is folded into the persistent area flag
// (STENCIL_AREA1) via combine_modifier_volume().
// ============================================================================

void CORE::process_modifier_object(TileContext& ctx, const ObjectPointer& obj) {
    uint32_t param_addr = (regs_ ? regs_->read(RegisterName::PARAM_BASE) : 0u) +
                          obj.start_address * 4;

    // The Volume Instruction lives in the ISP word (bits 31-29). Modifier
    // Volume objects carry no TSP or TCW word, so vertex data follows
    // immediately after this single control word.
    ISPTSPInstructionWord isp = ISPTSPInstructionWord::parse(vram_read32(param_addr));
    VolumeInstruction vinstr = isp.volume_instruction;

    uint32_t words_per_vertex = static_cast<uint32_t>(obj.skip) + 3u;

    // Read a single Modifier Volume vertex (X, Y, Z only) from |base|.
    auto read_mv_vertex = [&](uint32_t base, uint32_t idx) -> Vertex {
        Vertex v{};
        uint32_t a = base + idx * words_per_vertex * 4u;
        v.x = bits_to_float(vram_read32(a + 0));
        v.y = bits_to_float(vram_read32(a + 4));
        v.z = bits_to_float(vram_read32(a + 8));
        return v;
    };

    switch (obj.type) {
    case ObjectPointer::Type::TriangleArray: {
        // The TA does not stack Modifier Volume Triangle Array OPs, so in
        // practice num_primitives is 0. Still, walk all of them defensively;
        // each stacked block is ISP(4) + 3 vertices (no TSP/TCW).
        int num_triangles = obj.num_primitives + 1;
        uint32_t block_stride = 4u + 3u * words_per_vertex * 4u;
        for (int i = 0; i < num_triangles; i++) {
            uint32_t block = param_addr + static_cast<uint32_t>(i) * block_stride;
            uint32_t vbase = block + 4u;
            rasterize_modifier_triangle(ctx,
                                        read_mv_vertex(vbase, 0),
                                        read_mv_vertex(vbase, 1),
                                        read_mv_vertex(vbase, 2),
                                        ctx.tile_x, ctx.tile_y);
        }
        break;
    }
    case ObjectPointer::Type::TriangleStrip: {
        int max_tri = -1;
        for (int i = 0; i < 6; i++) {
            if (obj.mask & (1 << (5 - i))) max_tri = i;
        }
        if (max_tri < 0) break;
        uint32_t vbase = param_addr + 4u;
        for (int i = 0; i <= max_tri; i++) {
            if (!(obj.mask & (1 << (5 - i)))) continue;
            rasterize_modifier_triangle(ctx,
                                        read_mv_vertex(vbase, i),
                                        read_mv_vertex(vbase, i + 1),
                                        read_mv_vertex(vbase, i + 2),
                                        ctx.tile_x, ctx.tile_y);
        }
        break;
    }
    default:
        // Quad arrays / block links are not valid Modifier Volume objects.
        return;
    }

    // Fold the accumulated parity into the area flag once the volume closes.
    if (vinstr == VolumeInstruction::InsideLastPolygon) {
        combine_modifier_volume(ctx, /*inclusion=*/true);
    } else if (vinstr == VolumeInstruction::OutsideLastPolygon) {
        combine_modifier_volume(ctx, /*inclusion=*/false);
    }
}

// ============================================================================
// rasterize_modifier_triangle
// ============================================================================

void CORE::rasterize_modifier_triangle(TileContext& ctx,
                                       const Vertex& v0, const Vertex& v1,
                                       const Vertex& v2,
                                       int tile_x, int tile_y) {
    float tile_px = static_cast<float>(tile_x * static_cast<int>(TILE_SIZE));
    float tile_py = static_cast<float>(tile_y * static_cast<int>(TILE_SIZE));

    float min_x = std::min({v0.x, v1.x, v2.x});
    float max_x = std::max({v0.x, v1.x, v2.x});
    float min_y = std::min({v0.y, v1.y, v2.y});
    float max_y = std::max({v0.y, v1.y, v2.y});

    int ix_min = std::max(0, static_cast<int>(std::floor(min_x - tile_px)));
    int iy_min = std::max(0, static_cast<int>(std::floor(min_y - tile_py)));
    int ix_max = std::min(static_cast<int>(TILE_SIZE) - 1, static_cast<int>(std::floor(max_x - tile_px)));
    int iy_max = std::min(static_cast<int>(TILE_SIZE) - 1, static_cast<int>(std::floor(max_y - tile_py)));

    if (ix_min > ix_max || iy_min > iy_max) return;

    float area = edge_function(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (std::abs(area) < 1e-6f) return; // degenerate
    float inv_area = 1.0f / area;

    // Modifier volumes are rasterized by the same ISP unit as ordinary
    // polygons, so they must use the identical sample point and coverage
    // rule -- otherwise a volume's silhouette would not line up with the
    // opaque geometry it is being depth-tested against. Mirror the main
    // rasterizer's HALF_OFFSET handling and top-left fill rule. (Modifier
    // volumes never cull: the parity count needs both front and back faces.)
    auto edge_is_fill = [](float ax, float ay, float bx, float by,
                           float signed_area) -> bool {
        float dx = bx - ax;
        float dy = by - ay;
        if (signed_area < 0.0f) { dx = -dx; dy = -dy; }
        if (dy == 0.0f && dx < 0.0f) return true; // top edge
        return dy < 0.0f;                          // left edge
    };
    const float edge_eps = 1.0e-5f;
    const float bias0 = edge_is_fill(v1.x, v1.y, v2.x, v2.y, area) ? 0.0f : -edge_eps;
    const float bias1 = edge_is_fill(v2.x, v2.y, v0.x, v0.y, area) ? 0.0f : -edge_eps;
    const float bias2 = edge_is_fill(v0.x, v0.y, v1.x, v1.y, area) ? 0.0f : -edge_eps;

    const uint32_t half_off_reg = regs_ ? regs_->read(RegisterName::HALF_OFFSET) : 0x7u;
    const float pixel_off = ((half_off_reg >> 1) & 1u) ? 0.5f : 0.0f;

    for (int ly = iy_min; ly <= iy_max; ly++) {
        for (int lx = ix_min; lx <= ix_max; lx++) {
            float px = tile_px + static_cast<float>(lx) + pixel_off;
            float py = tile_py + static_cast<float>(ly) + pixel_off;

            float w0 = edge_function(v1.x, v1.y, v2.x, v2.y, px, py) * inv_area;
            float w1 = edge_function(v2.x, v2.y, v0.x, v0.y, px, py) * inv_area;
            float w2 = edge_function(v0.x, v0.y, v1.x, v1.y, px, py) * inv_area;

            if (w0 + bias0 < 0.0f || w1 + bias1 < 0.0f || w2 + bias2 < 0.0f) continue;

            float z = interpolate_bary(v0.z, v1.z, v2.z, w0, w1, w2);

            // Z is 1/W: a larger value is nearer the viewer. The volume face
            // contributes to the parity wherever it lies in front of (or on)
            // the stored opaque/PT surface. Modifier volumes never write depth.
            if (z >= ctx.buf.depth[ly][lx]) {
                ctx.buf.stencil[ly][lx] ^= STENCIL_VOLPAR;
            }
        }
    }
}

// ============================================================================
// combine_modifier_volume
// ============================================================================

void CORE::combine_modifier_volume(TileContext& ctx, bool inclusion) {
    // Per spec §3.4.5.1:
    //   inclusion volume: new_area = current_area | volume_result
    //   exclusion volume: new_area = current_area & volume_result
    // The per-volume parity bit is the volume_result; clear it afterwards so
    // the next volume starts from zero.
    for (int y = 0; y < static_cast<int>(TILE_SIZE); y++) {
        for (int x = 0; x < static_cast<int>(TILE_SIZE); x++) {
            uint8_t& s = ctx.buf.stencil[y][x];
            bool area = (s & STENCIL_AREA1) != 0;
            bool vol  = (s & STENCIL_VOLPAR) != 0;
            bool na   = inclusion ? (area || vol) : (area && vol);
            s &= ~(STENCIL_AREA1 | STENCIL_VOLPAR);
            if (na) s |= STENCIL_AREA1;
        }
    }
}

// ============================================================================
// apply_modifier_shadows
//
// Intensity Volume Mode (FPU_SHAD_SCALE bit 8): every pixel that is both
// shadow-enabled and in area 1 has its Base/Offset colour scaled by
// FPU_SHAD_SCALE[7:0]/256. Because libpvr shades Opaque/PT pixels as they are
// rasterized (before the modifier-volume list), the scaling is applied here to
// the already-composited primary colour. For the common non-textured and
// Modulate shading paths this is exactly equivalent to scaling Base/Offset.
//
// Parameter Selection Volume Mode (bit 8 = 0) would re-shade area 1 with a
// second parameter set; that "with Two Volumes" format is not implemented, so
// the area stencil is still built but no colour change is applied.
// ============================================================================

void CORE::apply_modifier_shadows(TileContext& ctx) {
    if (!regs_) return;

    uint32_t shad = regs_->read(RegisterName::FPU_SHAD_SCALE);
    bool intensity_mode = (shad >> 8) & 0x1;
    if (!intensity_mode) return; // Parameter Selection mode: nothing to apply

    uint32_t scale = shad & 0xFF; // multiplier is scale/256
    const uint8_t mask = STENCIL_AREA1 | STENCIL_SHADOW;

    for (int y = 0; y < static_cast<int>(TILE_SIZE); y++) {
        for (int x = 0; x < static_cast<int>(TILE_SIZE); x++) {
            if ((ctx.buf.stencil[y][x] & mask) != mask) continue;
            Pixel& p = ctx.buf.primary[y][x];
            p.r = static_cast<uint8_t>((static_cast<uint32_t>(p.r) * scale) >> 8);
            p.g = static_cast<uint8_t>((static_cast<uint32_t>(p.g) * scale) >> 8);
            p.b = static_cast<uint8_t>((static_cast<uint32_t>(p.b) * scale) >> 8);
        }
    }
}

// ============================================================================
// flush_tile_to_framebuffer
// ============================================================================

void CORE::flush_tile_to_framebuffer(TileContext& ctx, int tile_x, int tile_y) {
    if (!regs_ || !vram_) return;

    // Select the write target:
    //   - FB_W_SOF1 points into the 32-bit framebuffer area (normal TV
    //     output). Writes here go through the 32-bit access path.
    //   - FB_W_SOF2 points into the 64-bit texture-memory area and is
    //     used for render-to-texture. Writes there go through the
    //     64-bit access path.
    // The hardware selects which SOF register to use based on the
    // destination: if FB_W_SOF2 is configured into the 64-bit region,
    // that is treated as a render-to-texture target.
    uint32_t fb_w_sof1 = regs_->read(RegisterName::FB_W_SOF1);
    uint32_t fb_w_sof2 = regs_->read(RegisterName::FB_W_SOF2);
    bool rtt = fb_w_sof2_is_texture_region(fb_w_sof2);
    uint32_t fb_w_sof = rtt ? fb_w_sof2 : fb_w_sof1;
    uint32_t packmode  = regs_->fb_packmode();

    // Line stride in bytes: FB_W_LINESTRIDE is in 64-bit (8-byte) units
    uint32_t linestride_val = regs_->read(RegisterName::FB_W_LINESTRIDE) & 0x1FFu;
    uint32_t line_stride_bytes = linestride_val * 8;

    // Bytes per pixel for each pack mode
    int bpp;
    switch (packmode) {
    case 0: case 1: case 2: case 3: bpp = 2; break;
    case 4:                         bpp = 3; break;
    case 5: case 6:                 bpp = 4; break;
    default:                        bpp = 2; break;
    }

    // Pixel clipping registers
    uint32_t x_clip_min = regs_->fb_x_clip_min();
    uint32_t x_clip_max = regs_->fb_x_clip_max();
    uint32_t y_clip_min = regs_->fb_y_clip_min();
    uint32_t y_clip_max = regs_->fb_y_clip_max();

    for (int ly = 0; ly < static_cast<int>(TILE_SIZE); ly++) {
        for (int lx = 0; lx < static_cast<int>(TILE_SIZE); lx++) {
            uint32_t screen_x = static_cast<uint32_t>(tile_x) * TILE_SIZE + static_cast<uint32_t>(lx);
            uint32_t screen_y = static_cast<uint32_t>(tile_y) * TILE_SIZE + static_cast<uint32_t>(ly);

            // Pixel clipping
            if (screen_x < x_clip_min || screen_x > x_clip_max) continue;
            if (screen_y < y_clip_min || screen_y > y_clip_max) continue;

            uint32_t fb_addr = fb_w_sof + screen_y * line_stride_bytes + screen_x * static_cast<uint32_t>(bpp);
            // Encode the access path in the top bit of the address
            // passed to write_fb_pixel. This is stripped before the
            // actual VRAM write is performed and is only used to
            // dispatch to the correct path helper.
            uint32_t tagged = fb_addr | (rtt ? 0x80000000u : 0u);
            write_fb_pixel(tagged, ctx.buf.primary[ly][lx]);
        }
    }
}

// ============================================================================
// write_fb_pixel
// ============================================================================

void CORE::write_fb_pixel(uint32_t fb_addr, const Pixel& pixel) {
    uint32_t packmode = regs_->fb_packmode();
    bool dither = regs_->fb_dither();

    // The top bit of fb_addr is a tag set by flush_tile_to_framebuffer
    // to pick between the 32-bit (FB_W_SOF1) and 64-bit (FB_W_SOF2 /
    // render-to-texture) access paths.
    bool use_64bit_path = (fb_addr & 0x80000000u) != 0;
    fb_addr &= 0x7FFFFFFFu;

    auto fb_write16 = [&](uint32_t a, uint16_t v) {
        if (use_64bit_path) vram_write16_64(vram_, vram_size_, a, v);
        else                vram_write16_32(vram_, vram_size_, a, v);
    };
    auto fb_write32 = [&](uint32_t a, uint32_t v) {
        if (use_64bit_path) vram_write32_64(vram_, vram_size_, a, v);
        else                vram_write32_32(vram_, vram_size_, a, v);
    };
    auto fb_write8 = [&](uint32_t a, uint8_t v) {
        if (use_64bit_path) vram_write8_64(vram_, vram_size_, a, v);
        else                vram_write8_32(vram_, vram_size_, a, v);
    };

    uint8_t r = pixel.r;
    uint8_t g = pixel.g;
    uint8_t b = pixel.b;
    uint8_t a = pixel.a;

    // Simple ordered dithering (4x4 Bayer matrix)
    if (dither) {
        static const int bayer[4][4] = {
            { -4,  0, -3,  1 },
            {  2, -2,  3, -1 },
            { -3,  1, -4,  0 },
            {  3, -1,  2, -2 }
        };
        // Extract pixel position from address (approximate)
        int dx = static_cast<int>((fb_addr >> 1) & 3);
        int dy = static_cast<int>((fb_addr >> 10) & 3);
        int d = bayer[dy & 3][dx & 3];
        r = static_cast<uint8_t>(std::max(0, std::min(255, r + d)));
        g = static_cast<uint8_t>(std::max(0, std::min(255, g + d)));
        b = static_cast<uint8_t>(std::max(0, std::min(255, b + d)));
    }

    switch (packmode) {
    case 0: { // 0555 KRGB (16-bit)
        uint8_t k = static_cast<uint8_t>(regs_->fb_kval() ? 1 : 0);
        uint16_t val = static_cast<uint16_t>(
            (k << 15) |
            ((r >> 3) << 10) |
            ((g >> 3) << 5) |
            (b >> 3));
        fb_write16(fb_addr, val);
        break;
    }
    case 1: { // 565 RGB (16-bit)
        uint16_t val = static_cast<uint16_t>(
            ((r >> 3) << 11) |
            ((g >> 2) << 5) |
            (b >> 3));
        fb_write16(fb_addr, val);
        break;
    }
    case 2: { // 4444 ARGB (16-bit)
        uint16_t val = static_cast<uint16_t>(
            ((a >> 4) << 12) |
            ((r >> 4) << 8) |
            ((g >> 4) << 4) |
            (b >> 4));
        fb_write16(fb_addr, val);
        break;
    }
    case 3: { // 1555 ARGB (16-bit)
        uint8_t threshold = static_cast<uint8_t>(regs_->fb_alpha_threshold());
        uint16_t a_bit = (a >= threshold) ? 1u : 0u;
        uint16_t val = static_cast<uint16_t>(
            (a_bit << 15) |
            ((r >> 3) << 10) |
            ((g >> 3) << 5) |
            (b >> 3));
        fb_write16(fb_addr, val);
        break;
    }
    case 4: { // 888 RGB (24-bit packed)
        fb_write8(fb_addr + 0, r);
        fb_write8(fb_addr + 1, g);
        fb_write8(fb_addr + 2, b);
        break;
    }
    case 5: { // 0888 KRGB (32-bit)
        uint8_t k = static_cast<uint8_t>(regs_->fb_kval());
        uint32_t val = (static_cast<uint32_t>(k) << 24) |
                       (static_cast<uint32_t>(r) << 16) |
                       (static_cast<uint32_t>(g) << 8) |
                       static_cast<uint32_t>(b);
        fb_write32(fb_addr, val);
        break;
    }
    case 6: { // 8888 ARGB (32-bit)
        uint32_t val = (static_cast<uint32_t>(a) << 24) |
                       (static_cast<uint32_t>(r) << 16) |
                       (static_cast<uint32_t>(g) << 8) |
                       static_cast<uint32_t>(b);
        fb_write32(fb_addr, val);
        break;
    }
    default:
        break;
    }
}

// ============================================================================
// read_framebuffer
// ============================================================================

void CORE::read_framebuffer(uint8_t* out, int width, int height) const {
    if (!regs_ || !vram_ || !out) return;

    // The display video output stage reads the framebuffer through the
    // 32-bit access path. In interlaced mode each field comes from a
    // separate start-of-field register (FB_R_SOF1 / FB_R_SOF2); when
    // neither is configured for interlace the hardware simply reads
    // FB_R_SOF1 for every line. We honour FB_R_SOF2 here by alternating
    // between the two SOFs on every line, which matches how an
    // interlaced field pair is stitched back together for display.
    uint32_t fb_sof1 = regs_->read(RegisterName::FB_R_SOF1);
    uint32_t fb_sof2 = regs_->read(RegisterName::FB_R_SOF2);
    bool interlaced = (fb_sof2 != 0) && (fb_sof2 != fb_sof1);

    uint32_t fb_depth_fmt = regs_->fb_depth();
    bool concat = regs_->fb_concat();

    // fb_depth: 0=0555, 1=565, 2=888, 3=0888
    int bpp;
    switch (fb_depth_fmt) {
    case 0: case 1: bpp = 2; break;
    case 2:         bpp = 3; break;
    case 3:         bpp = 4; break;
    default:        bpp = 2; break;
    }

    // Compute read line stride from FB_R_SIZE
    uint32_t fb_x_size = regs_->fb_x_size() + 1; // pixels per line
    uint32_t fb_modulus = regs_->fb_modulus();
    uint32_t line_stride = fb_x_size * static_cast<uint32_t>(bpp) + fb_modulus * 4;

    uint32_t* out32 = reinterpret_cast<uint32_t*>(out);

    for (int y = 0; y < height; y++) {
        uint32_t sof;
        int field_line;
        if (interlaced) {
            sof = (y & 1) ? fb_sof2 : fb_sof1;
            field_line = y >> 1;
        } else {
            sof = fb_sof1;
            field_line = y;
        }

        for (int x = 0; x < width; x++) {
            uint32_t addr = sof + static_cast<uint32_t>(field_line) * line_stride +
                            static_cast<uint32_t>(x) * static_cast<uint32_t>(bpp);

            uint8_t r = 0, g = 0, b_ch = 0, a_ch = 255;

            switch (fb_depth_fmt) {
            case 0: { // 0555 KRGB
                uint16_t val = vram_read16_32(vram_, vram_size_, addr);
                r    = static_cast<uint8_t>(((val >> 10) & 0x1F) << 3);
                g    = static_cast<uint8_t>(((val >> 5) & 0x1F) << 3);
                b_ch = static_cast<uint8_t>((val & 0x1F) << 3);
                if (concat) { r |= r >> 5; g |= g >> 5; b_ch |= b_ch >> 5; }
                a_ch = 255;
                break;
            }
            case 1: { // 565 RGB
                uint16_t val = vram_read16_32(vram_, vram_size_, addr);
                r    = static_cast<uint8_t>(((val >> 11) & 0x1F) << 3);
                g    = static_cast<uint8_t>(((val >> 5) & 0x3F) << 2);
                b_ch = static_cast<uint8_t>((val & 0x1F) << 3);
                if (concat) { r |= r >> 5; g |= g >> 6; b_ch |= b_ch >> 5; }
                a_ch = 255;
                break;
            }
            case 2: { // 888 RGB (24-bit)
                r    = vram_read8_32(vram_, vram_size_, addr + 0);
                g    = vram_read8_32(vram_, vram_size_, addr + 1);
                b_ch = vram_read8_32(vram_, vram_size_, addr + 2);
                a_ch = 255;
                break;
            }
            case 3: { // 0888 ARGB (32-bit)
                uint32_t val = vram_read32_32(vram_, vram_size_, addr);
                a_ch = static_cast<uint8_t>((val >> 24) & 0xFF);
                r    = static_cast<uint8_t>((val >> 16) & 0xFF);
                g    = static_cast<uint8_t>((val >> 8) & 0xFF);
                b_ch = static_cast<uint8_t>(val & 0xFF);
                break;
            }
            }

            out32[y * width + x] = (static_cast<uint32_t>(a_ch) << 24) |
                                   (static_cast<uint32_t>(r) << 16) |
                                   (static_cast<uint32_t>(g) << 8) |
                                   static_cast<uint32_t>(b_ch);
        }
    }
}

// ============================================================================
// ThreadPool implementation
// ============================================================================

CORE::ThreadPool::ThreadPool() = default;

CORE::ThreadPool::~ThreadPool() {
    if (!workers_.empty()) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            shutdown_ = true;
            ++batch_id_;
        }
        cv_work_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }
}

void CORE::ThreadPool::resize(unsigned int count) {
    if (count == worker_count_) return;

    // Tear down any existing workers.
    if (!workers_.empty()) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            shutdown_ = true;
            ++batch_id_;
        }
        cv_work_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
        shutdown_ = false;
        batch_id_ = 0;
        current_task_ = nullptr;
        batch_total_ = 0;
        next_index_.store(0);
        completed_.store(0);
    }

    worker_count_ = count;
    if (count <= 1) {
        // 0 or 1 workers: parallel_for runs inline on the caller.
        return;
    }

    workers_.reserve(count);
    for (unsigned int i = 0; i < count; ++i) {
        workers_.emplace_back([this, id = i]() {
            g_worker_id = id;
            worker_loop();
        });
    }
}

void CORE::ThreadPool::parallel_for(std::size_t n,
                                    const std::function<void(std::size_t)>& task) {
    if (n == 0) return;

    if (worker_count_ <= 1) {
        // Inline fallback: run on the caller's thread (worker slot 0).
        g_worker_id = 0;
        for (std::size_t i = 0; i < n; ++i) task(i);
        return;
    }

    // Publish the batch to the workers.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_task_ = &task;
        batch_total_ = n;
        next_index_.store(0);
        completed_.store(0);
        ++batch_id_;
    }
    cv_work_.notify_all();

    // Wait for all tasks in this batch to complete.
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_done_.wait(lock, [this] {
            return completed_.load() >= batch_total_ && active_ == 0;
        });
        current_task_ = nullptr;
    }
}

void CORE::ThreadPool::worker_loop() {
    std::uint64_t seen_batch = 0;
    for (;;) {
        const std::function<void(std::size_t)>* task = nullptr;
        std::size_t total = 0;

        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_work_.wait(lock, [this, &seen_batch] {
                return shutdown_ || batch_id_ != seen_batch;
            });
            if (shutdown_) return;
            seen_batch = batch_id_;
            task = current_task_;
            total = batch_total_;
            if (!task || total == 0) continue;
            ++active_;
        }

        while (true) {
            std::size_t i = next_index_.fetch_add(1);
            if (i >= total) break;
            (*task)(i);
            completed_.fetch_add(1);
        }

        // Out of this batch: the last one out wakes the dispatcher.
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (--active_ == 0) cv_done_.notify_all();
        }
    }
}

} // namespace pvr
