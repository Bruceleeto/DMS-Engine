#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <array>

#include "pvr_types.h"
#include "registers.h"

namespace pvr {

class TA {
public:
    TA();

    // Connect to VRAM and register bank
    void set_vram(uint8_t* vram, size_t vram_size);
    void set_registers(RegisterBank* regs);

    // Software reset
    void reset();

    // List initialization (called when TA_LIST_INIT is written)
    void list_init();

    // List continuation processing (called when TA_LIST_CONT is written)
    void list_cont();

    // Process a single 32-byte TA command
    void process_command(const uint8_t* data);

    // --- Save/load state ---

    // Serialize internal TA state to the given FILE*.
    // Returns true on success.
    bool save_state(std::FILE* f) const;

    // Restore internal TA state from the given FILE*. The TA must already
    // have vram/registers connected (they are not serialized).
    // Returns true on success.
    bool load_state(std::FILE* f);

    // Interrupt status flags (polled by the PVR)
    bool opaque_list_complete = false;
    bool opaque_modifier_complete = false;
    bool translucent_list_complete = false;
    bool translucent_modifier_complete = false;
    bool punch_through_complete = false;

    // Error latches, corresponding to the TA half of SB_ISTERR. Unlike the
    // five completion flags above these are level-sticky until explicitly
    // polled and cleared (see PVR::clear_ta_interrupts()), since a fault can
    // happen at any point mid-list rather than exactly at a list boundary.
    //
    //   opb_overflow      SB_ISTERR bit 3, "TA: Object List Pointer
    //                     Overflow". register_object_in_tile() hit
    //                     TA_OL_LIMIT. Per §3.7.3.4.3 the display list is
    //                     still structurally valid after this - only the
    //                     image is wrong - so rendering continues.
    //   isp_overflow      SB_ISTERR bit 2, "TA: ISP/TSP Parameter
    //                     Overflow". write_isp_tsp_params() hit
    //                     TA_ISP_LIMIT. Per §3.7.4 the resulting display
    //                     list "is not generated correctly [...] and
    //                     therefore should not be used for drawing"; see
    //                     display_list_invalid below.
    //   illegal_parameter SB_ISTERR bit 4, "TA: Illegal Parameter". §3.7.4
    //                     lists the input constraints; the one we can
    //                     actually detect here is submitting parameters for
    //                     a list whose TA_ALLOC_CTRL OPB size is "No List".
    bool opb_overflow = false;
    bool isp_overflow = false;
    bool illegal_parameter = false;

    // Set alongside isp_overflow and cleared *only* by list_init()/reset(),
    // not by clear_ta_interrupts(). §3.7.4 on exceeding TA_ISP_LIMIT: "It is
    // necessary in this case to reconsider the memory allocations and to
    // start over from list initialization." So unlike the interrupt latches
    // this survives being polled - acknowledging the interrupt does not make
    // the half-written display list usable again, only TA_LIST_INIT does.
    bool display_list_invalid = false;

private:
    // --- State ---
    RegisterBank* regs_ = nullptr;
    uint8_t* vram_ = nullptr;
    size_t vram_size_ = 0;

    // Current state machine
    enum class State {
        WaitingForGlobalOrControl,
        WaitingForVertex
    };
    State state_ = State::WaitingForGlobalOrControl;

    // Currently active list type
    ListType current_list_type_ = ListType::Opaque;
    bool list_type_set_ = false;

    // Current global parameter state
    ISPTSPInstructionWord current_isp_tsp_;
    TSPInstructionWord current_tsp_;
    TextureControlWord current_tcw_;
    TSPInstructionWord current_tsp_1_;  // For Two Volumes
    TextureControlWord current_tcw_1_;  // For Two Volumes
    ParameterControlWord current_pcw_;

    // Face colors for Intensity mode
    float face_color_a_ = 0, face_color_r_ = 0, face_color_g_ = 0, face_color_b_ = 0;
    float face_offset_a_ = 0, face_offset_r_ = 0, face_offset_g_ = 0, face_offset_b_ = 0;

    // Sprite base/offset color
    uint32_t sprite_base_color_ = 0;
    uint32_t sprite_offset_color_ = 0;

    // Strip state
    int strip_len_ = 1;   // 1, 2, 4, or 6
    // the modifier volume so far's tiles: its last triangle goes in all of
    // them, so that each of them closes it
    bool mv_open_ = false;
    int mv_tx_min_ = 0, mv_ty_min_ = 0, mv_tx_max_ = 0, mv_ty_max_ = 0;
    int vertex_count_ = 0;
    std::vector<Vertex> strip_vertices_;

    // User tile clip state
    bool user_clip_enabled_ = false;
    int user_clip_mode_ = 0; // 0=disabled, 2=inside, 3=outside
    int user_clip_x_min_ = 0, user_clip_y_min_ = 0;
    int user_clip_x_max_ = 0, user_clip_y_max_ = 0;

    // 64-byte command accumulator.
    //
    // Several TA commands span two 32-byte store-queue blocks (e.g.
    // polygon/modifier-volume vertex commands with Floating Color, modifier
    // volume vertex A/B/C triples, polygon global params with Intensity
    // Mode 1 face color / Intensity Mode 2 + offset face color / Two
    // Volumes). The host feeds us data 32 bytes at a time, so we buffer
    // the first half until the second half arrives, then dispatch once.
    uint8_t  cmd_buffer_[64] = {};
    int      cmd_buffer_fill_ = 0;     // bytes currently in cmd_buffer_
    int      cmd_expected_size_ = 32;  // 32 or 64; computed after first half

    // ISP/TSP parameter write pointer (offset into vram)
    uint32_t isp_write_ptr_ = 0;
    uint32_t isp_base_      = 0;  // cached TA_ISP_BASE, set at list_init

    // Triangle Array stacking state.
    //
    // The TA coalesces consecutive single-triangle submissions that
    // cover overlapping tiles into a single OPB slot by incrementing
    // the "Number of Triangles" field of the existing Triangle Array
    // object pointer instead of writing a new entry.
    // This is critical for GL_TRIANGLES-style workloads (one V V V_EOL
    // triple per triangle) where naive emission would allocate one OPB
    // slot per triangle per covered tile, exhausting KOS's overflow OPB
    // budget after a few seconds of gameplay.
    //
    // The rule (matching master lxdream's pre-libpvr TA):
    //   * Both the previous and current emissions are single-triangle
    //     Triangle Array OPs (top 3 bits = 100, bit 31 set).
    //   * The current tile (tx, ty) was also within the previous
    //     triangle's tile bounding box — this guarantees that the
    //     previous triangle's ISP/TSP parameter block was the last one
    //     written for this tile's OPB entry, so the current block is
    //     contiguous with it in VRAM.
    //   * The last OPB entry for this tile has the same header bits
    //     (bits 31-29 para-type, bit-24 shadow, bits 23-21 skip).
    //   * The count field (bits 28-25) has not saturated (max 15 extra).
    //
    // stack_active_ is cleared whenever a non-single-triangle emission,
    // a global parameter, a list boundary, or a reset occurs, so that
    // stacking never crosses an ISP/TSP state change or a VRAM gap.
    bool     stack_active_  = false;
    uint32_t stack_hdr_     = 0; // previous obj_ptr & 0xE1E00000
    int      stack_tx_min_  = 0, stack_ty_min_ = 0;
    int      stack_tx_max_  = -1, stack_ty_max_ = -1;

    // Object list pointer buffer (per tile, per list type)
    // For each tile, for each list type, we track the current OPB write position
    // and the number of objects registered
    struct TileOPB {
        uint32_t first_opb_addr = 0;    // Address of the first OPB for this tile/list
        uint32_t current_opb_addr = 0;  // Address of the current OPB being written
        int obj_count = 0;              // Number of objects in current OPB
        int opb_size = 0;               // OPB size for this list type
    };

    // tiles_x * tiles_y * NUM_LIST_TYPES
    int glob_clip_x_ = 0;
    int glob_clip_y_ = 0;
    std::array<TileOPB, MAX_TILES_X * MAX_TILES_Y * NUM_LIST_TYPES> tile_opbs_;

    // Next OPB address (for allocating additional OPBs)
    uint32_t next_opb_ = 0;

    // Overflow reporting flags (so we only log once per frame / run)
    bool opb_overflow_reported_ = false;
    bool isp_overflow_reported_ = false;
    bool illegal_parameter_reported_ = false;

    // --- Helper methods ---

    // Get the OPB for a specific tile and list
    TileOPB& get_tile_opb(int tile_x, int tile_y, ListType list);

    // Register an object in the object list for a specific tile
    void register_object_in_tile(int tile_x, int tile_y, ListType list, uint32_t object_pointer);

    // Process different parameter types
    void process_control_param(const uint8_t* data);
    void process_global_param(const uint8_t* data);
    void process_vertex_param(const uint8_t* data);

    // Determine total command size (32 or 64 bytes) from the first 32
    // bytes of a TA command, using the currently-active polygon format
    // state (current_pcw_/current_isp_tsp_ etc).
    int command_size_bytes(const uint8_t* first_chunk) const;

    // Write ISP/TSP parameters to VRAM for a polygon
    uint32_t write_isp_tsp_params(const Vertex* vertices, int vertex_count);

    // Calculate bounding box in tile coordinates
    void calc_bounding_box(const Vertex* vertices, int count,
                          int& tile_x_min, int& tile_y_min,
                          int& tile_x_max, int& tile_y_max);

    // Check if a tile is within the effective clipping area
    bool is_tile_in_clip_area(int tile_x, int tile_y);

    // Write end-of-list markers for all tiles in the current list
    void finalize_list(ListType list);

    // Determine Object Pointer value for a polygon
    uint32_t make_strip_object_pointer(uint32_t isp_addr, uint32_t mask, bool shadow, int skip);
    uint32_t make_array_object_pointer(uint32_t isp_addr, int count, bool is_quad, bool shadow, int skip);

    // Calculate skip value from parameter control word
    int calc_skip_value() const;

    // Helper to convert float color to packed 8-bit
    static uint8_t float_to_u8(float v);

    // Convert shading data to packed color
    uint32_t convert_intensity_to_packed(float intensity, float face_a, float face_r, float face_g, float face_b);

    // Write a uint32_t to VRAM at byte offset
    void vram_write32(uint32_t byte_addr, uint32_t value);
    uint32_t vram_read32(uint32_t byte_addr) const;

    // Parse a float from command data
    static float read_float(const uint8_t* data, int offset);
    static uint32_t read_u32(const uint8_t* data, int offset);
};

} // namespace pvr
