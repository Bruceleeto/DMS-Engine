#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

#include "pvr_types.h"
#include "registers.h"
#include "texture.h"

namespace pvr {

class CORE {
public:
    CORE();
    ~CORE();

    // Disable copy/move (we hold a live thread pool).
    CORE(const CORE&) = delete;
    CORE& operator=(const CORE&) = delete;

    void set_vram(uint8_t* vram, size_t vram_size);
    void set_registers(RegisterBank* regs);

    // Software reset
    void reset();

    // Start rendering (called when STARTRENDER register is written).
    // This processes the entire display list and renders all tiles,
    // distributing tiles across the worker thread pool.
    // Blocks until all tiles are complete.
    void start_render();

    // Block until any in-flight async render is complete and the live
    // register pointer has been restored.  Safe to call from any thread;
    // calling it when no render is in flight is a no-op.
    void wait_render_done();

    // Async variant: returns immediately and calls on_done() on the render
    // thread when all tiles have finished.  A previous in-flight render is
    // joined before the new one is dispatched, so it is safe to call this
    // every frame without explicit external synchronisation.
    void start_render_async(std::function<void()> on_done);

    // Configure the number of worker threads used to render tiles.
    // Must be called before start_render(). Default is 4.
    // If set to 0 or 1, tiles are rendered serially on the calling thread.
    void set_thread_count(unsigned int count);

    // Read back the framebuffer to an external buffer (ARGB8888 format)
    void read_framebuffer(uint8_t* out, int width, int height) const;

    // Rendering complete flag.
    // Written by the render thread, read from the emulator main thread,
    // so must be atomic.
    std::atomic<bool> render_complete{false};

    // SB_ISTERR bit 0, "RENDER: ISP out of Cache (Buffer over flow)".
    //
    // Set when the CORE hits one of its structural safety caps while
    // walking the region array or a tile's object pointer chain - a
    // self-referential Pointer Block Link, an unterminated list, or more
    // objects/links in one tile than any real scene produces. Those caps
    // already existed to stop us spinning forever; previously they bailed
    // out silently, so a display list corrupted by an earlier TA fault
    // rendered as a merely-odd-looking frame with no indication anything
    // had gone wrong.
    //
    // The frame still completes (render_complete is still set) - this
    // reports the fault rather than emulating a stalled CORE, since the
    // spec doesn't describe what the hardware does past this point.
    //
    // Written from worker threads during parallel_for, so must be atomic.
    std::atomic<bool> isp_out_of_cache{false};

private:
    RegisterBank* regs_ = nullptr;
    uint8_t* vram_ = nullptr;
    size_t vram_size_ = 0;
    TextureSampler texture_sampler_;

    // ------------------------------------------------------------------
    // Per-tile rendering context
    //
    // All state that is mutated while rendering a single tile lives here
    // so that multiple worker threads can render different tiles in
    // parallel without contending on CORE members.
    // ------------------------------------------------------------------

    struct TileBuffer {
        Pixel primary[TILE_SIZE][TILE_SIZE];    // Primary accumulation buffer
        Pixel secondary[TILE_SIZE][TILE_SIZE];  // Secondary accumulation buffer
        float depth[TILE_SIZE][TILE_SIZE];      // Z buffer (1/w values)
        // Modifier-volume flag bits (see core.cpp):
        //   bit 0  STENCIL_AREA1   -- pixel is in modifier-volume "area 1"
        //   bit 1  STENCIL_VOLPAR  -- intermediate parity for the volume
        //                             currently being accumulated
        //   bit 2  STENCIL_SHADOW  -- owning opaque/PT pixel has the shadow
        //                             bit set (responds to intensity shadows)
        uint8_t stencil[TILE_SIZE][TILE_SIZE];
    };

    struct TranslucentFragment {
        float z;
        Pixel color;
        int x, y;
        TSPInstructionWord tsp;    // Needed for per-fragment blending during auto-sort
        bool z_write_disable;      // From ISP; honoured during the depth test at blend time
    };

    struct TileContext {
        TileBuffer buf{};
        int tile_x = 0;
        int tile_y = 0;

        std::vector<TranslucentFragment> translucent_fragments;
        bool auto_sort_active = false;

        // Reusable vertex scratch buffer (avoids per-draw allocation).
        std::vector<Vertex> vertex_buf;
    };

    // --- Thread pool -------------------------------------------------

    class ThreadPool {
    public:
        ThreadPool();
        ~ThreadPool();

        // Start/resize the pool. Joins and relaunches workers as needed.
        // count == 0 or 1 means "no pool" -- submit() runs inline.
        void resize(unsigned int count);

        // Number of active workers (0 means inline execution).
        unsigned int worker_count() const { return worker_count_; }

        // Dispatch |n| indexed tasks across the pool and wait for them
        // all to finish. Each task receives its 0-based index.
        // If worker_count_ <= 1, runs the tasks serially on the caller.
        void parallel_for(std::size_t n,
                          const std::function<void(std::size_t)>& task);

    private:
        void worker_loop();

        std::vector<std::thread> workers_;
        unsigned int worker_count_ = 0;

        // Current batch state
        std::mutex mtx_;
        std::condition_variable cv_work_;
        std::condition_variable cv_done_;
        const std::function<void(std::size_t)>* current_task_ = nullptr;
        std::size_t batch_total_ = 0;
        std::atomic<std::size_t> next_index_{0};
        std::atomic<std::size_t> completed_{0};
        std::uint64_t batch_id_ = 0;    // incremented per parallel_for call
        // Workers still inside the current batch (guarded by mtx_). The
        // next batch waits for 0, so no worker can carry a stale task or
        // total into it and take its indices.
        unsigned int active_ = 0;
        bool shutdown_ = false;
    };

    ThreadPool pool_;
    unsigned int configured_thread_count_ = 8;

    // Background render thread.  Joined at the start of the next
    // start_render_async() call and in the destructor.
    std::thread render_thread_;

    // Frozen copy of the register bank taken on the main thread at
    // RENDER_START time.  The render thread reads from this snapshot
    // rather than the live regs_ so that the SH4 can update registers
    // for the next frame without corrupting the in-flight render.
    RegisterBank render_snap_;

    // Pointer to the live register bank, saved before each async render
    // so it can be restored after the render thread is joined.
    RegisterBank* live_regs_backup_ = nullptr;

    // Snapshot of the TA parameter area (region array + ISP/TSP params +
    // OPBs) taken synchronously at RENDER_START time.  vram_read32()
    // redirects reads that fall within this range to the snapshot so
    // that the render thread sees stable geometry data even after the TA
    // starts overwriting live VRAM for the next frame.
    std::vector<uint8_t> param_snapshot_;
    uint32_t             param_snapshot_base_ = 0;  // physical VRAM byte offset

    // --- Rendering pipeline ------------------------------------------

    // Capture a copy of the TA parameter area from live VRAM into
    // param_snapshot_.  Must be called on the main thread after
    // render_snap_ has been populated.
    void take_param_snapshot();

    // The whole render on the GPU (core_gl.cpp); false: the tiles do it
    bool render_gl(const std::vector<RegionArrayEntry>& tiles);

    // Process a single region array entry (render one tile) using |ctx|.
    void render_tile(TileContext& ctx, const RegionArrayEntry& region);

    // Clear the tile buffers in |ctx|.
    void clear_tile(TileContext& ctx, float depth_value);

    // Process a polygon list for the current tile.
    void process_list(TileContext& ctx, ListType list_type,
                      uint32_t obj_list_addr, bool is_last_region);

    // Process a single object from the object list
    void process_object(TileContext& ctx, ListType list_type,
                        const ObjectPointer& obj);

    // Process a single Modifier Volume object (one or more triangles that
    // accumulate the per-pixel volume parity and, on the volume's last
    // polygon, fold that parity into the persistent area-1 flag).
    void process_modifier_object(TileContext& ctx, const ObjectPointer& obj);

    // Rasterize one Modifier Volume triangle: toggle STENCIL_VOLPAR for every
    // covered pixel whose volume surface lies in front of the stored depth.
    void rasterize_modifier_triangle(TileContext& ctx,
                                     const Vertex& v0, const Vertex& v1,
                                     const Vertex& v2,
                                     int tile_x, int tile_y);

    // Fold the accumulated per-volume parity (STENCIL_VOLPAR) into the
    // persistent area flag (STENCIL_AREA1) for the whole tile, then clear the
    // parity bit. |inclusion| selects OR (inclusion volume) vs AND (exclusion).
    void combine_modifier_volume(TileContext& ctx, bool inclusion);

    // Apply Intensity Volume Mode shadows: scale the primary colour of every
    // pixel that is both shadowed and in area 1 by FPU_SHAD_SCALE/256.
    void apply_modifier_shadows(TileContext& ctx);

    // ISP: Perform depth test and optionally write depth
    bool isp_depth_test(TileContext& ctx, int x, int y, float z,
                        DepthCompareMode mode, bool z_write_disable);

    // TSP: Perform texture/shading for a visible pixel (no mutable state).
    //
    // lod              – mip LOD for texture sampling (0 = base level).
    //                    Pre-computed per-triangle from screen-space UV derivatives.
    // texel_half_offset – whether to apply the -0.5 texel bias in bilinear
    //                    filtering (true = HALF_OFFSET bit 2 = 1, the default).
    Pixel tsp_shade_pixel(const ISPTSPInstructionWord& isp,
                          const TSPInstructionWord& tsp,
                          const TextureControlWord& tcw,
                          float u, float v,
                          uint32_t base_color,
                          uint32_t offset_color,
                          int pixel_x, int pixel_y,
                          float lod = 0.0f,
                          bool texel_half_offset = true) const;

    // Alpha blending (operates on |ctx|'s buffers).
    void alpha_blend(TileContext& ctx, int x, int y,
                     const Pixel& src, const TSPInstructionWord& tsp);

    // Fog processing
    Pixel apply_fog(const Pixel& pixel_color, const TSPInstructionWord& tsp,
                    float z_value, uint32_t offset_color) const;

    // Color clamp processing
    Pixel apply_color_clamp(const Pixel& color) const;

    // Rasterize a triangle into the tile. |shadowed| marks the written pixels
    // as belonging to a shadow-enabled polygon (Opaque / Punch Through only).
    void rasterize_triangle(TileContext& ctx, ListType list_type,
                            const ISPTSPInstructionWord& isp,
                            const TSPInstructionWord& tsp,
                            const TextureControlWord& tcw,
                            const Vertex& v0, const Vertex& v1, const Vertex& v2,
                            int tile_x, int tile_y, bool shadowed = false);

    // Rasterize a quad into the tile (splits into two triangles)
    void rasterize_quad(TileContext& ctx, ListType list_type,
                        const ISPTSPInstructionWord& isp,
                        const TSPInstructionWord& tsp,
                        const TextureControlWord& tcw,
                        const Vertex& v0, const Vertex& v1,
                        const Vertex& v2, const Vertex& v3,
                        int tile_x, int tile_y, bool shadowed = false);

    // Draw the background for a tile
    void draw_background(TileContext& ctx, int tile_x, int tile_y);

    // Flush tile to framebuffer
    void flush_tile_to_framebuffer(TileContext& ctx, int tile_x, int tile_y);

    // Write a pixel to the framebuffer in the configured format.
    // Tiles write to disjoint pixel regions, so no synchronisation is
    // required between worker threads here.
    void write_fb_pixel(uint32_t fb_addr, const Pixel& pixel);

    // Read ISP/TSP parameters from VRAM (pure VRAM read -> thread-safe).
    void read_isp_tsp_params(uint32_t addr, int skip, int vertex_count,
                             ISPTSPInstructionWord& isp,
                             TSPInstructionWord& tsp,
                             TextureControlWord& tcw,
                             std::vector<Vertex>& vertices) const;

    // Compute the mip LOD level for a triangle from its screen-space UV derivatives.
    //
    // Uses the affine gradient of the perspective-correct UV field evaluated over
    // the triangle.  This is an approximation to the hardware's per-pixel D value
    // that is good enough for LOD selection; the per-pixel variation within a
    // single triangle is small relative to the LOD thresholds.
    //
    // Returns a non-negative float; the caller floors it to pick an integer level.
    // Only meaningful for mipmapped twiddled textures (tcw.mip_mapped &&
    // !tcw.scan_order); returns 0.0f otherwise.
    static float compute_mip_lod(const TSPInstructionWord& tsp,
                                  const TextureControlWord& tcw,
                                  const Vertex& v0, const Vertex& v1, const Vertex& v2);

    // Edge function for rasterization (returns positive if point is on the left side)
    static float edge_function(float ax, float ay, float bx, float by, float cx, float cy);

    // Barycentric interpolation
    static float interpolate_bary(float v0, float v1, float v2, float w0, float w1, float w2);

    // Perspective-correct interpolation
    static float interpolate_persp(float v0, float v1, float v2,
                                   float z0, float z1, float z2,
                                   float w0, float w1, float w2);

    // Helper to read/write VRAM
    uint32_t vram_read32(uint32_t byte_addr) const;
    void vram_write32(uint32_t byte_addr, uint32_t value);
    void vram_write16(uint32_t byte_addr, uint16_t value);

    // Fog table lookup
    float fog_table_lookup(float z_value) const;

    // Compute blend factor from alpha instruction
    static Pixel compute_blend_factor(AlphaInstr instr, const Pixel& src, const Pixel& dst, bool is_src_factor);
};

} // namespace pvr
