#include "ta.h"
#include "vram.h"

#include <cstdio>
#include <cstring>

namespace pvr {

// ============================================================================
// Construction / Setup
// ============================================================================

TA::TA() {
    std::memset(&current_isp_tsp_, 0, sizeof(current_isp_tsp_));
    std::memset(&current_tsp_, 0, sizeof(current_tsp_));
    std::memset(&current_tcw_, 0, sizeof(current_tcw_));
    std::memset(&current_tsp_1_, 0, sizeof(current_tsp_1_));
    std::memset(&current_tcw_1_, 0, sizeof(current_tcw_1_));
    std::memset(&current_pcw_, 0, sizeof(current_pcw_));
    tile_opbs_.fill(TileOPB{});
}

void TA::set_vram(uint8_t* vram, size_t vram_size) {
    vram_ = vram;
    vram_size_ = vram_size;
}

void TA::set_registers(RegisterBank* regs) {
    regs_ = regs;
}

void TA::reset() {
    state_ = State::WaitingForGlobalOrControl;
    current_list_type_ = ListType::Opaque;
    list_type_set_ = false;
    vertex_count_ = 0;
    strip_vertices_.clear();
    strip_len_ = 1;
    mv_open_ = false;
    user_clip_enabled_ = false;
    user_clip_mode_ = 0;
    stack_active_ = false;
    user_clip_x_min_ = 0;
    user_clip_y_min_ = 0;
    user_clip_x_max_ = 0;
    user_clip_y_max_ = 0;
    cmd_buffer_fill_ = 0;
    cmd_expected_size_ = 32;
    std::memset(cmd_buffer_, 0, sizeof(cmd_buffer_));
    isp_write_ptr_ = 0;
    next_opb_ = 0;
    glob_clip_x_ = 0;
    glob_clip_y_ = 0;
    tile_opbs_.fill(TileOPB{});

    face_color_a_ = 0; face_color_r_ = 0; face_color_g_ = 0; face_color_b_ = 0;
    face_offset_a_ = 0; face_offset_r_ = 0; face_offset_g_ = 0; face_offset_b_ = 0;
    sprite_base_color_ = 0;
    sprite_offset_color_ = 0;

    opaque_list_complete = false;
    opaque_modifier_complete = false;
    translucent_list_complete = false;
    translucent_modifier_complete = false;
    punch_through_complete = false;

    opb_overflow = false;
    isp_overflow = false;
    illegal_parameter = false;
    opb_overflow_reported_ = false;
    isp_overflow_reported_ = false;
    illegal_parameter_reported_ = false;
    display_list_invalid = false;
}

// ============================================================================
// Save / load state
// ============================================================================
//
// On-disk format (little-endian host assumed, matching the rest of lxdream's
// save-state handling). Note: the VRAM buffer and the register bank are NOT
// saved here - they are expected to be saved/restored by the caller.
//
//   char     magic[8]          "PVRTA001"
//   uint32_t state
//   uint32_t current_list_type
//   uint32_t list_type_set
//   ISPTSPInstructionWord current_isp_tsp
//   TSPInstructionWord    current_tsp
//   TextureControlWord    current_tcw
//   TSPInstructionWord    current_tsp_1
//   TextureControlWord    current_tcw_1
//   ParameterControlWord  current_pcw
//   float    face_color[4]  (argb)
//   float    face_offset[4] (argb)
//   uint32_t sprite_base_color
//   uint32_t sprite_offset_color
//   int32_t  strip_len
//   int32_t  vertex_count
//   uint32_t num_strip_vertices
//   Vertex   strip_vertices[num_strip_vertices]
//   uint32_t user_clip_enabled
//   int32_t  user_clip_mode
//   int32_t  user_clip_x_min, y_min, x_max, y_max
//   uint32_t isp_write_ptr
//   int32_t  glob_clip_x, glob_clip_y
//   uint32_t next_opb
//   uint32_t opb_overflow_reported
//   uint32_t isp_overflow_reported
//   uint8_t  opaque_list_complete
//   uint8_t  opaque_modifier_complete
//   uint8_t  translucent_list_complete
//   uint8_t  translucent_modifier_complete
//   uint8_t  punch_through_complete
//   TileOPB  tile_opbs[MAX_TILES_X * MAX_TILES_Y * NUM_LIST_TYPES]
//
// The error latches (opb_overflow / isp_overflow / illegal_parameter /
// display_list_invalid) are deliberately NOT serialized, which is why adding
// them did not need a format bump. They are per-frame state cleared by the
// next list_init(), and a save state restored mid-list is approximate anyway;
// starting a restored state with them clear is both correct enough and
// strictly safer than restoring a stale "list is unusable" latch.

static const char TA_SAVE_MAGIC[8] = { 'P','V','R','T','A','0','0','1' };

bool TA::save_state(std::FILE* f) const {
    if (!f) return false;

    auto wr = [&](const void* p, size_t n) {
        return std::fwrite(p, 1, n, f) == n;
    };

    if (!wr(TA_SAVE_MAGIC, sizeof(TA_SAVE_MAGIC))) return false;

    uint32_t u32;
    int32_t  i32;

    u32 = static_cast<uint32_t>(state_);                if (!wr(&u32, sizeof(u32))) return false;
    u32 = static_cast<uint32_t>(current_list_type_);    if (!wr(&u32, sizeof(u32))) return false;
    u32 = list_type_set_ ? 1u : 0u;                     if (!wr(&u32, sizeof(u32))) return false;

    if (!wr(&current_isp_tsp_, sizeof(current_isp_tsp_))) return false;
    if (!wr(&current_tsp_,     sizeof(current_tsp_)))     return false;
    if (!wr(&current_tcw_,     sizeof(current_tcw_)))     return false;
    if (!wr(&current_tsp_1_,   sizeof(current_tsp_1_)))   return false;
    if (!wr(&current_tcw_1_,   sizeof(current_tcw_1_)))   return false;
    if (!wr(&current_pcw_,     sizeof(current_pcw_)))     return false;

    float face_color[4]  = { face_color_a_,  face_color_r_,  face_color_g_,  face_color_b_  };
    float face_offset[4] = { face_offset_a_, face_offset_r_, face_offset_g_, face_offset_b_ };
    if (!wr(face_color,  sizeof(face_color)))  return false;
    if (!wr(face_offset, sizeof(face_offset))) return false;

    if (!wr(&sprite_base_color_,   sizeof(sprite_base_color_)))   return false;
    if (!wr(&sprite_offset_color_, sizeof(sprite_offset_color_))) return false;

    i32 = strip_len_;    if (!wr(&i32, sizeof(i32))) return false;
    i32 = vertex_count_; if (!wr(&i32, sizeof(i32))) return false;

    u32 = static_cast<uint32_t>(strip_vertices_.size());
    if (!wr(&u32, sizeof(u32))) return false;
    if (u32 > 0) {
        if (!wr(strip_vertices_.data(), sizeof(Vertex) * u32)) return false;
    }

    u32 = user_clip_enabled_ ? 1u : 0u; if (!wr(&u32, sizeof(u32))) return false;
    i32 = user_clip_mode_;              if (!wr(&i32, sizeof(i32))) return false;
    i32 = user_clip_x_min_;             if (!wr(&i32, sizeof(i32))) return false;
    i32 = user_clip_y_min_;             if (!wr(&i32, sizeof(i32))) return false;
    i32 = user_clip_x_max_;             if (!wr(&i32, sizeof(i32))) return false;
    i32 = user_clip_y_max_;             if (!wr(&i32, sizeof(i32))) return false;

    if (!wr(&isp_write_ptr_, sizeof(isp_write_ptr_))) return false;

    i32 = glob_clip_x_; if (!wr(&i32, sizeof(i32))) return false;
    i32 = glob_clip_y_; if (!wr(&i32, sizeof(i32))) return false;

    if (!wr(&next_opb_, sizeof(next_opb_))) return false;

    u32 = opb_overflow_reported_ ? 1u : 0u; if (!wr(&u32, sizeof(u32))) return false;
    u32 = isp_overflow_reported_ ? 1u : 0u; if (!wr(&u32, sizeof(u32))) return false;

    uint8_t flags[5] = {
        (uint8_t)(opaque_list_complete        ? 1 : 0),
        (uint8_t)(opaque_modifier_complete    ? 1 : 0),
        (uint8_t)(translucent_list_complete   ? 1 : 0),
        (uint8_t)(translucent_modifier_complete ? 1 : 0),
        (uint8_t)(punch_through_complete      ? 1 : 0),
    };
    if (!wr(flags, sizeof(flags))) return false;

    if (!wr(tile_opbs_.data(), sizeof(TileOPB) * tile_opbs_.size())) return false;

    return true;
}

bool TA::load_state(std::FILE* f) {
    if (!f) return false;

    auto rd = [&](void* p, size_t n) {
        return std::fread(p, 1, n, f) == n;
    };

    char magic[8];
    if (!rd(magic, sizeof(magic))) return false;
    if (std::memcmp(magic, TA_SAVE_MAGIC, sizeof(magic)) != 0) return false;

    uint32_t u32;
    int32_t  i32;

    if (!rd(&u32, sizeof(u32))) return false;
    state_ = static_cast<State>(u32);
    if (!rd(&u32, sizeof(u32))) return false;
    current_list_type_ = static_cast<ListType>(u32);
    if (!rd(&u32, sizeof(u32))) return false;
    list_type_set_ = (u32 != 0);

    if (!rd(&current_isp_tsp_, sizeof(current_isp_tsp_))) return false;
    if (!rd(&current_tsp_,     sizeof(current_tsp_)))     return false;
    if (!rd(&current_tcw_,     sizeof(current_tcw_)))     return false;
    if (!rd(&current_tsp_1_,   sizeof(current_tsp_1_)))   return false;
    if (!rd(&current_tcw_1_,   sizeof(current_tcw_1_)))   return false;
    if (!rd(&current_pcw_,     sizeof(current_pcw_)))     return false;

    float face_color[4];
    float face_offset[4];
    if (!rd(face_color,  sizeof(face_color)))  return false;
    if (!rd(face_offset, sizeof(face_offset))) return false;
    face_color_a_  = face_color[0];  face_color_r_  = face_color[1];
    face_color_g_  = face_color[2];  face_color_b_  = face_color[3];
    face_offset_a_ = face_offset[0]; face_offset_r_ = face_offset[1];
    face_offset_g_ = face_offset[2]; face_offset_b_ = face_offset[3];

    if (!rd(&sprite_base_color_,   sizeof(sprite_base_color_)))   return false;
    if (!rd(&sprite_offset_color_, sizeof(sprite_offset_color_))) return false;

    if (!rd(&i32, sizeof(i32))) return false;
    strip_len_    = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    vertex_count_ = i32;

    if (!rd(&u32, sizeof(u32))) return false;
    // Sanity-limit vertex count to guard against corrupt files
    if (u32 > 65536u) return false;
    strip_vertices_.resize(u32);
    if (u32 > 0) {
        if (!rd(strip_vertices_.data(), sizeof(Vertex) * u32)) return false;
    }

    if (!rd(&u32, sizeof(u32))) return false;
    user_clip_enabled_ = (u32 != 0);
    if (!rd(&i32, sizeof(i32))) return false;
    user_clip_mode_    = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    user_clip_x_min_   = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    user_clip_y_min_   = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    user_clip_x_max_   = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    user_clip_y_max_   = i32;

    if (!rd(&isp_write_ptr_, sizeof(isp_write_ptr_))) return false;

    if (!rd(&i32, sizeof(i32))) return false;
    glob_clip_x_ = i32;
    if (!rd(&i32, sizeof(i32))) return false;
    glob_clip_y_ = i32;

    if (!rd(&next_opb_, sizeof(next_opb_))) return false;

    if (!rd(&u32, sizeof(u32))) return false;
    opb_overflow_reported_ = (u32 != 0);
    if (!rd(&u32, sizeof(u32))) return false;
    isp_overflow_reported_ = (u32 != 0);

    uint8_t flags[5];
    if (!rd(flags, sizeof(flags))) return false;
    opaque_list_complete          = (flags[0] != 0);
    opaque_modifier_complete      = (flags[1] != 0);
    translucent_list_complete     = (flags[2] != 0);
    translucent_modifier_complete = (flags[3] != 0);
    punch_through_complete        = (flags[4] != 0);

    if (!rd(tile_opbs_.data(), sizeof(TileOPB) * tile_opbs_.size())) return false;

    // Reset stacking state — it is not serialised (stacking context is
    // only valid within a single TA pass) so always start fresh.
    stack_active_ = false;

    return true;
}

// ============================================================================
// VRAM access helpers
//
// The Tile Accelerator writes parameter data (region array, OPBs, ISP/TSP
// parameter blocks, vertex data) that the CORE later reads via the 64-bit
// access path. Both sides must therefore agree on the path, so every TA
// VRAM access goes through vram_*_64() from vram.h.
// ============================================================================

void TA::vram_write32(uint32_t byte_addr, uint32_t value) {
    vram_write32_64(vram_, vram_size_, byte_addr, value);
}

uint32_t TA::vram_read32(uint32_t byte_addr) const {
    return vram_read32_64(vram_, vram_size_, byte_addr);
}

float TA::read_float(const uint8_t* data, int offset) {
    float f;
    std::memcpy(&f, data + offset, 4);
    return f;
}

uint32_t TA::read_u32(const uint8_t* data, int offset) {
    uint32_t v;
    std::memcpy(&v, data + offset, 4);
    return v;
}

uint8_t TA::float_to_u8(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

uint32_t TA::convert_intensity_to_packed(float intensity,
                                          float face_a, float face_r,
                                          float face_g, float face_b) {
    uint8_t a = float_to_u8(intensity * face_a);
    uint8_t r = float_to_u8(intensity * face_r);
    uint8_t g = float_to_u8(intensity * face_g);
    uint8_t b = float_to_u8(intensity * face_b);
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) <<  8) |
           (static_cast<uint32_t>(b));
}

// ============================================================================
// Tile OPB helpers
// ============================================================================

TA::TileOPB& TA::get_tile_opb(int tile_x, int tile_y, ListType list) {
    int tiles_x = glob_clip_x_ + 1;
    int idx = (tile_y * tiles_x + tile_x) * NUM_LIST_TYPES +
              static_cast<int>(list);
    return tile_opbs_[idx];
}

bool TA::is_tile_in_clip_area(int tile_x, int tile_y) {
    if (tile_x < 0 || tile_y < 0) return false;
    if (tile_x > glob_clip_x_ || tile_y > glob_clip_y_) return false;

    if (user_clip_mode_ == 2) {
        // Inside mode: tile must be within user clip rectangle
        return tile_x >= user_clip_x_min_ && tile_x <= user_clip_x_max_ &&
               tile_y >= user_clip_y_min_ && tile_y <= user_clip_y_max_;
    } else if (user_clip_mode_ == 3) {
        // Outside mode: tile must be outside user clip rectangle
        return tile_x < user_clip_x_min_ || tile_x > user_clip_x_max_ ||
               tile_y < user_clip_y_min_ || tile_y > user_clip_y_max_;
    }

    return true;
}

void TA::register_object_in_tile(int tile_x, int tile_y, ListType list,
                                  uint32_t object_pointer) {
    if (!is_tile_in_clip_area(tile_x, tile_y)) return;

    TileOPB& opb = get_tile_opb(tile_x, tile_y, list);

    // This list type has no OPB allocated (TA_ALLOC_CTRL size == "No List").
    //
    // §3.7.4, parameter input restrictions: "If 'No List' is specified for
    // the Object Pointer Block size in the TA_ALLOC_CTRL register for a
    // certain type of list, parameters for that type of list may not be
    // input." Doing it anyway is an input violation, and real hardware
    // raises "TA: Illegal Parameter" (SB_ISTERR bit 4). We still drop the
    // object either way, but the guest now gets told - previously this was
    // the one TA fault that failed completely silently, which is exactly
    // the case that looks like "my geometry vanished for no reason".
    if (opb.opb_size == 0) {
        if (!illegal_parameter_reported_) {
            illegal_parameter_reported_ = true;
            if (getenv("PVR_TA_DEBUG")) {
                FILE *f = fopen(getenv("PVR_TA_DEBUG"), "a");
                if (f) { fprintf(f, "TA: object for list %d (no OPB) tile %d,%d op %08x cur_list %d set %d\n",
                        (int)list, tile_x, tile_y, object_pointer, (int)current_list_type_, (int)list_type_set_); fclose(f); }
            }
        }
        illegal_parameter = true;
        return;
    }

    // Triangle Array stacking.
    //
    // When a single-triangle Triangle Array OP (top 3 bits = 100) is
    // registered in a tile that was also covered by the PREVIOUS
    // single-triangle emission, we can increment the "Number of
    // Triangles" field of the existing OPB entry rather than writing
    // a new slot, matching real hardware behaviour and
    // keeps per-tile OPB usage bounded for GL_TRIANGLES workloads.
    //
    // The "tile was in previous triangle's bbox" check is the critical
    // guard: it ensures the previous triangle's ISP/TSP parameter block
    // was written immediately before the current one in VRAM, so the
    // CORE can walk (count+1) contiguous blocks starting at the
    // original start address.
    //
    // stack_active_ is only set when the immediately preceding emission
    // was also a single-triangle Array OP, and is cleared by any state
    // change (global param, list boundary, multi-tri strip, reset).
    if (stack_active_ &&
        (object_pointer & 0x80000000u) != 0 &&          // Array OP (bit 31)
        ((object_pointer >> 29) & 0x7u) != 0x7u &&      // not PointerBlockLink
        tile_x >= stack_tx_min_ && tile_x <= stack_tx_max_ &&
        tile_y >= stack_ty_min_ && tile_y <= stack_ty_max_ &&
        opb.obj_count > 0) {
        uint32_t prev_addr = opb.current_opb_addr +
                             static_cast<uint32_t>(opb.obj_count - 1) * 4;
        uint32_t prev = vram_read32(prev_addr);
        // Header bits: bits 31-29 (para type), bit 24 (shadow),
        // bits 23-21 (skip).  Count field: bits 28-25 (max 0xF = 15
        // extra triangles, giving 16 total per stacked slot).
        constexpr uint32_t HDR_MASK = 0xE1E00000u;
        constexpr uint32_t CNT_MASK = 0x1E000000u;
        constexpr uint32_t CNT_INC  = 0x02000000u;
        constexpr uint32_t CNT_MAX  = 0x1E000000u; // 15 << 25
        if ((prev & HDR_MASK) == (object_pointer & HDR_MASK) &&
            (prev & CNT_MASK) <  CNT_MAX) {
            vram_write32(prev_addr,
                         (prev & ~CNT_MASK) |
                         ((prev & CNT_MASK) + CNT_INC));
            return; // stacked: no new OPB slot consumed
        }
    }

    // If the current OPB is full (need to reserve last slot for link pointer),
    // allocate a new OPB block
    if (opb.obj_count >= opb.opb_size - 1) {
        uint32_t opb_byte_size = static_cast<uint32_t>(opb.opb_size) * 4;

        // OPB allocation direction: TA_ALLOC_CTRL bit 20 (OPB_Mode).
        //   0 = increasing addresses (default, used by our own samples):
        //       next_opb_ points at the bottom of the free area and grows
        //       upward. TA_OL_LIMIT is the upper bound; overflow when
        //       next_opb_ + size > ol_limit. The new OPB is allocated at
        //       next_opb_.
        //   1 = decreasing addresses (used by KOS): next_opb_ starts at
        //       the top of the free area and grows downward. TA_OL_LIMIT
        //       is the *lower* bound; overflow when
        //       next_opb_ - size < ol_limit. The new OPB is allocated at
        //       next_opb_ - size so it occupies [next_opb_ - size, next_opb_).
        const bool     opb_decreasing = regs_ ? regs_->opb_mode() : false;
        const uint32_t ol_limit       = regs_ ? regs_->read(RegisterName::TA_OL_LIMIT) : 0;

        uint32_t new_opb_addr;
        bool     overflow = false;
        if (opb_decreasing) {
            if (next_opb_ < opb_byte_size ||
                (ol_limit != 0 && (next_opb_ - opb_byte_size) < ol_limit)) {
                overflow     = true;
                new_opb_addr = next_opb_;  // only used for the log message
            } else {
                new_opb_addr = next_opb_ - opb_byte_size;
            }
        } else {
            new_opb_addr = next_opb_;
            if (ol_limit != 0 && new_opb_addr + opb_byte_size > ol_limit) {
                overflow = true;
            }
        }

        // Exceeding TA_OL_LIMIT raises "TA: Object List Pointer Overflow"
        // (SB_ISTERR bit 3) on real hardware.
        //
        // §3.7.3.4.3 is specific about what the TA does with the memory:
        // "the TA stores the 'End of List' Object List data at the limit
        // address, and links to this 'End of List' the OPBs for all of the
        // Tiles for which additional OPBs could not be stored. Therefore,
        // the address that is specified in the TA_OL_LIMIT register cannot
        // be used for other data."
        //
        // So the End-of-List marker lives at TA_OL_LIMIT itself - a single
        // shared word - and every starved tile gets a Pointer Block Link to
        // it in the last slot of its current OPB. We previously wrote the
        // marker at the tile's own next free slot instead, which terminated
        // the walk one slot early and, more importantly, meant a guest
        // inspecting VRAM after the fault saw a layout real hardware would
        // never produce.
        //
        // Note the display list stays structurally *valid* here: the same
        // section notes the list "is generated correctly in this case" and
        // only "the resulting image will not appear as expected", so unlike
        // an ISP/TSP overflow this does not set display_list_invalid.
        if (overflow) {
            if (!opb_overflow_reported_) {
                opb_overflow_reported_ = true;
            }
            opb_overflow = true;

            if (ol_limit != 0) {
                // The shared End-of-List at the limit address.
                vram_write32(ol_limit, 0xF0000000u);

                // Link this tile's current OPB to it. Pointer Block Link
                // format: bits 31-29 = 111, bit 28 = 0 (not end-of-list),
                // bits 23-2 = next block's word address.
                uint32_t link_addr = opb.current_opb_addr +
                                     static_cast<uint32_t>(opb.obj_count) * 4;
                vram_write32(link_addr, 0xE0000000u | ((ol_limit >> 2) << 2));
            }

            // Mark this OPB as full so no more objects are appended to it.
            opb.obj_count = opb.opb_size;
            return;
        }

        // Advance next_opb_ for the next allocation.
        if (opb_decreasing) {
            next_opb_ = new_opb_addr;
        } else {
            next_opb_ += opb_byte_size;
        }

        // Zero out the new OPB
        for (int i = 0; i < opb.opb_size; ++i) {
            vram_write32(new_opb_addr + i * 4, 0);
        }

        // Write a Pointer Block Link in the last slot of the current OPB
        // Format: bits 31-29 = 111, bit 28 = 0 (not end), bits 23-2 = next word addr
        uint32_t link_word = 0xE0000000u | ((new_opb_addr >> 2) << 2);
        uint32_t link_addr = opb.current_opb_addr +
                             static_cast<uint32_t>(opb.obj_count) * 4;
        vram_write32(link_addr, link_word);

        // Switch to the new OPB
        opb.current_opb_addr = new_opb_addr;
        opb.obj_count = 0;
    }

    // If this OPB was marked full due to a previous overflow, drop the object.
    if (opb.obj_count >= opb.opb_size) return;

    // Write the object pointer at the current position
    uint32_t write_addr = opb.current_opb_addr +
                          static_cast<uint32_t>(opb.obj_count) * 4;
    vram_write32(write_addr, object_pointer);
    opb.obj_count++;
}

void TA::calc_bounding_box(const Vertex* vertices, int count,
                            int& tile_x_min, int& tile_y_min,
                            int& tile_x_max, int& tile_y_max) {
    float min_x = vertices[0].x;
    float min_y = vertices[0].y;
    float max_x = vertices[0].x;
    float max_y = vertices[0].y;

    for (int i = 1; i < count; ++i) {
        if (vertices[i].x < min_x) min_x = vertices[i].x;
        if (vertices[i].y < min_y) min_y = vertices[i].y;
        if (vertices[i].x > max_x) max_x = vertices[i].x;
        if (vertices[i].y > max_y) max_y = vertices[i].y;
    }

    // Clamp to non-negative
    if (min_x < 0.0f) min_x = 0.0f;
    if (min_y < 0.0f) min_y = 0.0f;

    tile_x_min = static_cast<int>(min_x) / TILE_SIZE;
    tile_y_min = static_cast<int>(min_y) / TILE_SIZE;
    tile_x_max = static_cast<int>(max_x) / TILE_SIZE;
    tile_y_max = static_cast<int>(max_y) / TILE_SIZE;

    // Clamp to global clip area
    if (tile_x_min < 0) tile_x_min = 0;
    if (tile_y_min < 0) tile_y_min = 0;
    if (tile_x_max > glob_clip_x_) tile_x_max = glob_clip_x_;
    if (tile_y_max > glob_clip_y_) tile_y_max = glob_clip_y_;
}

// ============================================================================
// Object pointer construction
// ============================================================================

uint32_t TA::make_strip_object_pointer(uint32_t isp_addr,
                                        uint32_t mask, bool shadow, int skip) {
    // Triangle Strip format:
    //   bit 31 = 0
    //   bits 30-25 = mask (6 bits, which triangles in the strip)
    //   bit 24 = shadow
    //   bits 23-21 = skip
    //   bits 20-0 = start address (word address relative to PARAM_BASE)
    uint32_t word = 0;
    word |= (mask & 0x3Fu) << 25;
    if (shadow) word |= (1u << 24);
    word |= (static_cast<uint32_t>(skip) & 0x7u) << 21;
    word |= (isp_addr & 0x1FFFFFu);
    return word;
}

uint32_t TA::make_array_object_pointer(uint32_t isp_addr, int count,
                                        bool is_quad, bool shadow, int skip) {
    // Triangle/Quad Array format:
    //   bits 31-29 = 100 (triangle) or 101 (quad)
    //   bits 28-25 = num_primitives (0-based)
    //   bit 24 = shadow
    //   bits 23-21 = skip
    //   bits 20-0 = start address
    uint32_t word = 0;
    if (is_quad) {
        word = 0b101u << 29;
    } else {
        word = 0b100u << 29;
    }
    word |= (static_cast<uint32_t>(count - 1) & 0xFu) << 25;
    if (shadow) word |= (1u << 24);
    word |= (static_cast<uint32_t>(skip) & 0x7u) << 21;
    word |= (isp_addr & 0x1FFFFFu);
    return word;
}

int TA::calc_skip_value() const {
    bool tex = current_pcw_.texture;
    bool ofs = current_pcw_.offset;
    bool uv16 = current_pcw_.uv_16bit;

    if (!tex && !ofs) {
        // X, Y, Z, BaseColor = 4 words, skip+3=4, skip=1
        return 1;
    }
    if (tex && !ofs && !uv16) {
        // X, Y, Z, U, V, BaseColor = 6 words, skip+3=6, skip=3
        return 3;
    }
    if (tex && !ofs && uv16) {
        // X, Y, Z, UV_packed, BaseColor = 5 words, skip+3=5, skip=2
        return 2;
    }
    if (tex && ofs && !uv16) {
        // X, Y, Z, U, V, BaseColor, OffsetColor = 7 words, skip+3=7, skip=4
        return 4;
    }
    if (tex && ofs && uv16) {
        // X, Y, Z, UV_packed, BaseColor, OffsetColor = 6 words, skip+3=6, skip=3
        return 3;
    }
    if (!tex && ofs) {
        // X, Y, Z, BaseColor, OffsetColor = 5 words, skip+3=5, skip=2
        return 2;
    }

    return 1;
}

// ============================================================================
// List initialization
// ============================================================================

void TA::list_init() {
    if (!regs_) return;

    // Read configuration from registers
    uint32_t ol_base = regs_->read(RegisterName::TA_OL_BASE);
    uint32_t isp_base = regs_->read(RegisterName::TA_ISP_BASE);
    uint32_t next_opb_init = regs_->read(RegisterName::TA_NEXT_OPB_INIT);

    // The TA writes OPB entries to VRAM and the CORE reads them back.
    // Both sides must agree on tile dimensions for the VRAM layout to
    // be consistent. The BIOS sets TA_GLOB_TILE_CLIP to 64×15 tiles,
    // which exceeds the documented 40×15 hardware max but is within the
    // register's encoding capacity.
    glob_clip_x_ = static_cast<int>(regs_->tile_x_num());
    glob_clip_y_ = static_cast<int>(regs_->tile_y_num());

    int tiles_x = glob_clip_x_ + 1;
    int tiles_y = glob_clip_y_ + 1;

    // Read OPB sizes for each list type
    uint32_t opb_sizes[NUM_LIST_TYPES];
    opb_sizes[static_cast<int>(ListType::Opaque)]                    = regs_->opaque_opb_size();
    opb_sizes[static_cast<int>(ListType::OpaqueModifierVolume)]      = regs_->opaque_modifier_opb_size();
    opb_sizes[static_cast<int>(ListType::Translucent)]               = regs_->translucent_opb_size();
    opb_sizes[static_cast<int>(ListType::TranslucentModifierVolume)] = regs_->translucent_modifier_opb_size();
    opb_sizes[static_cast<int>(ListType::PunchThrough)]              = regs_->punch_through_opb_size();

    // Initialize ISP/TSP write pointer
    isp_write_ptr_ = isp_base;
    isp_base_      = isp_base;

    // Initialize next OPB allocation pointer
    next_opb_ = next_opb_init;

    // Reset stacking state - list init crosses a VRAM boundary
    stack_active_ = false;

    // Reset overflow reporting flags for this frame
    opb_overflow_reported_ = false;
    isp_overflow_reported_ = false;
    illegal_parameter_reported_ = false;

    // TA_LIST_INIT is precisely the "start over from list initialization"
    // §3.7.4 requires after an ISP/TSP parameter overflow, so this is the
    // one place the display list becomes usable again.
    display_list_invalid = false;

    // Clear state
    state_ = State::WaitingForGlobalOrControl;
    list_type_set_ = false;
    vertex_count_ = 0;
    strip_vertices_.clear();
    strip_len_ = 1;
    mv_open_ = false;
    user_clip_enabled_ = false;
    user_clip_mode_ = 0;

    // Reset 64-byte command accumulator so that any partial command left
    // over from a previous list doesn't leak into the new one.
    cmd_buffer_fill_ = 0;
    cmd_expected_size_ = 32;

    face_color_a_ = 0; face_color_r_ = 0; face_color_g_ = 0; face_color_b_ = 0;
    face_offset_a_ = 0; face_offset_r_ = 0; face_offset_g_ = 0; face_offset_b_ = 0;
    sprite_base_color_ = 0;
    sprite_offset_color_ = 0;

    opaque_list_complete = false;
    opaque_modifier_complete = false;
    translucent_list_complete = false;
    translucent_modifier_complete = false;
    punch_through_complete = false;

    // Reset tile OPBs
    tile_opbs_.fill(TileOPB{});

    // Zero out the OL area from ol_base to next_opb_init
    if (next_opb_init > ol_base) {
        uint32_t clear_size = next_opb_init - ol_base;
        for (uint32_t off = 0; off < clear_size; off += 4) {
            vram_write32(ol_base + off, 0);
        }
    }

    // Layout: list-type major, then Y-major, then X-major. Each tile
    // gets one initial OPB per enabled list type, starting at ol_base
    // and laid out with addresses increasing. This matches both the
    // PVR region-array formula and how KOS pre-computes its own
    // Region Array OPB pointers (opb_type[j] + (y*tw + x) * opb_size[j]
    // * 4) when it builds the Tile Matrix itself.
    //
    // The OPB_Mode bit in TA_ALLOC_CTRL only controls how *overflow*
    // OPBs are allocated when a tile's initial OPB fills up (see
    // register_object_in_tile); the initial per-tile OPB layout is
    // always increasing from TA_OL_BASE.

    // Calculate per-tile OPB block size (sum of all enabled list OPB sizes in bytes)
    uint32_t per_tile_bytes = 0;
    for (uint32_t lt = 0; lt < NUM_LIST_TYPES; ++lt) {
        per_tile_bytes += opb_sizes[lt] * 4;
    }

    uint32_t addr = ol_base;
    for (uint32_t lt = 0; lt < NUM_LIST_TYPES; ++lt) {
        for (int ty = 0; ty < tiles_y; ++ty) {
            for (int tx = 0; tx < tiles_x; ++tx) {
                int idx = (ty * tiles_x + tx) * NUM_LIST_TYPES + lt;
                if (opb_sizes[lt] > 0) {
                    tile_opbs_[idx].first_opb_addr = addr;
                    tile_opbs_[idx].current_opb_addr = addr;
                    tile_opbs_[idx].obj_count = 0;
                    tile_opbs_[idx].opb_size = static_cast<int>(opb_sizes[lt]);
                    addr += opb_sizes[lt] * 4;
                } else {
                    tile_opbs_[idx].first_opb_addr = 0;
                    tile_opbs_[idx].current_opb_addr = 0;
                    tile_opbs_[idx].obj_count = 0;
                    tile_opbs_[idx].opb_size = 0;
                }
            }
        }
    }

    // Seed next_opb_ for overflow allocations.
    //
    // In increasing (OPB_Mode=0) mode, overflow OPBs are allocated
    // starting at TA_NEXT_OPB_INIT, growing up toward TA_OL_LIMIT.
    // The initial per-tile OPBs live below TA_NEXT_OPB_INIT, so we
    // only need to bump next_opb_ if our initial layout happens to
    // extend past it (normally it shouldn't).
    //
    // In decreasing (OPB_Mode=1) mode, initial OPBs live ABOVE
    // TA_NEXT_OPB_INIT and overflow allocations grow DOWN from
    // TA_NEXT_OPB_INIT toward TA_OL_LIMIT. next_opb_ therefore
    // stays exactly at TA_NEXT_OPB_INIT and must NOT be bumped by
    // the initial layout's upward walk.
    const bool opb_decreasing = regs_ ? regs_->opb_mode() : false;
    if (!opb_decreasing && addr > next_opb_) {
        next_opb_ = addr;
    }

    // Write TA_NEXT_OPB register
    regs_->write(RegisterName::TA_NEXT_OPB, next_opb_);
}

void TA::list_cont() {
    if (!regs_) return;

    // List continuation: reinitialize tile OPBs for the new list
    // configuration but keep isp_write_ptr_ and next_opb_ as-is.
    // Stacking state is invalidated: list-cont can change TA_ALLOC_CTRL
    // and the ISP/TSP state, so any previous triangle's params are no
    // longer a valid stacking target.
    stack_active_ = false;

    uint32_t ol_base = regs_->read(RegisterName::TA_OL_BASE);

    // Use the same tile dimensions as list_init() - must match for
    // TA/CORE VRAM layout agreement.
    glob_clip_x_ = static_cast<int>(regs_->tile_x_num());
    glob_clip_y_ = static_cast<int>(regs_->tile_y_num());

    int tiles_x = glob_clip_x_ + 1;
    int tiles_y = glob_clip_y_ + 1;

    uint32_t opb_sizes[NUM_LIST_TYPES];
    opb_sizes[static_cast<int>(ListType::Opaque)]                    = regs_->opaque_opb_size();
    opb_sizes[static_cast<int>(ListType::OpaqueModifierVolume)]      = regs_->opaque_modifier_opb_size();
    opb_sizes[static_cast<int>(ListType::Translucent)]               = regs_->translucent_opb_size();
    opb_sizes[static_cast<int>(ListType::TranslucentModifierVolume)] = regs_->translucent_modifier_opb_size();
    opb_sizes[static_cast<int>(ListType::PunchThrough)]              = regs_->punch_through_opb_size();

    // Reset state machine but keep ISP write pointer
    state_ = State::WaitingForGlobalOrControl;
    list_type_set_ = false;
    vertex_count_ = 0;
    strip_vertices_.clear();
    strip_len_ = 1;
    mv_open_ = false;
    user_clip_mode_ = 0;

    // Reinitialize tile OPBs with the new OL base
    tile_opbs_.fill(TileOPB{});

    uint32_t addr = ol_base;
    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            for (uint32_t lt = 0; lt < NUM_LIST_TYPES; ++lt) {
                int idx = (ty * tiles_x + tx) * NUM_LIST_TYPES + lt;
                if (opb_sizes[lt] > 0) {
                    tile_opbs_[idx].first_opb_addr = addr;
                    tile_opbs_[idx].current_opb_addr = addr;
                    tile_opbs_[idx].obj_count = 0;
                    tile_opbs_[idx].opb_size = static_cast<int>(opb_sizes[lt]);
                    addr += opb_sizes[lt] * 4;
                } else {
                    tile_opbs_[idx] = TileOPB{};
                }
            }
        }
    }

    // Seed next_opb_ for overflow allocations. Mirrors the logic in
    // list_init(): only bump next_opb_ past the initial layout when in
    // increasing OPB mode. In decreasing mode (KOS), initial OPBs live
    // above TA_NEXT_OPB_INIT and overflow grows downward from it, so
    // next_opb_ must stay at its current value.
    const bool opb_decreasing = regs_->opb_mode();
    if (!opb_decreasing && addr > next_opb_) {
        next_opb_ = addr;
    }

    regs_->write(RegisterName::TA_NEXT_OPB, next_opb_);
}

// ============================================================================
// Command dispatch
// ============================================================================

// Determine the total size (32 or 64 bytes) of a TA command, given its
// first 32-byte chunk. Several command types span two store-queue blocks
// on real hardware (e.g. Floating Color vertices, Intensity Mode 2 +
// offset globals, modifier volume vertices). The host hands us data in
// 32-byte chunks, so we need this to know when to buffer a second half
// before dispatching.
int TA::command_size_bytes(const uint8_t* first_chunk) const {
    uint32_t pcw_raw = read_u32(first_chunk, 0x00);
    ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

    switch (pcw.para_type) {
        case ParaType::EndOfList:
        case ParaType::UserTileClip:
        case ParaType::ObjectListSet:
            // Control parameters are always 32 bytes.
            return 32;

        case ParaType::Sprite:
            // Sprite global parameter is 32 bytes. Textured-sprite vertex
            // UV data is sent as a separate 32-byte command afterwards.
            return 32;

        case ParaType::Polygon: {
            // Polygon / Modifier Volume global parameter.
            bool is_mv = (current_list_type_ == ListType::OpaqueModifierVolume ||
                          current_list_type_ == ListType::TranslucentModifierVolume);
            if (is_mv) return 32;           // Modifier volume header: 32 bytes
            if (pcw.volume) return 32;      // Two Volumes header: 32 bytes

            // Intensity Mode 2 + offset carries the face offset color at
            // 0x20-0x2C, which only exists in the 64-byte form.
            if (pcw.col_type == ColorType::IntensityMode2 && pcw.offset) {
                return 64;
            }
            return 32;
        }

        case ParaType::Vertex: {
            bool is_mv = (current_list_type_ == ListType::OpaqueModifierVolume ||
                          current_list_type_ == ListType::TranslucentModifierVolume);
            if (is_mv) {
                // Modifier volume vertex command packs A/B/C in 64 bytes.
                return 64;
            }

            if (current_pcw_.para_type == ParaType::Sprite) {
                // Sprite vertex is a single 64-byte command containing all
                // four corner positions and, for textured sprites, the three
                // 16-bit UV pairs (A, B, C).  D's UV is derived at parse time.
                return 64;
            }

            // Vertex command sizes per §3.7.5.3:
            //   Non-textured FloatingColor (Type 1): last field at 0x1C → 32 bytes
            //   Textured FloatingColor (Types 5/6): last field at 0x3C → 64 bytes
            //   Two-volume textured (Types 11-14): last field at 0x2C → 64 bytes
            //   All other types: last field at 0x1C or earlier → 32 bytes
            if (current_pcw_.col_type == ColorType::FloatingColor && current_pcw_.texture) {
                return 64;
            }
            if (current_pcw_.volume && current_pcw_.texture) {
                return 64;
            }
            return 32;
        }

        default:
            // Reserved / unknown - assume minimum size.
            return 32;
    }
}

void TA::process_command(const uint8_t* data) {
    // If we're partway through buffering a 64-byte command, append this
    // chunk as the second half and dispatch once complete.
    if (cmd_buffer_fill_ != 0) {
        std::memcpy(cmd_buffer_ + cmd_buffer_fill_, data, 32);
        cmd_buffer_fill_ += 32;

        if (cmd_buffer_fill_ < cmd_expected_size_) {
            // Shouldn't happen (max expected is 64) but be defensive.
            return;
        }

        const uint8_t* full = cmd_buffer_;
        uint32_t pcw_raw = read_u32(full, 0x00);
        ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

        // Reset the accumulator before dispatching (process_* may call back
        // in through state transitions).
        cmd_buffer_fill_ = 0;
        cmd_expected_size_ = 32;

        switch (pcw.para_type) {
            case ParaType::EndOfList:
            case ParaType::UserTileClip:
            case ParaType::ObjectListSet:
                process_control_param(full);
                break;
            case ParaType::Polygon:
            case ParaType::Sprite:
                process_global_param(full);
                break;
            case ParaType::Vertex:
                process_vertex_param(full);
                break;
            default:
                break;
        }
        return;
    }

    // Fresh command. Determine whether it's 32 or 64 bytes.
    int size = command_size_bytes(data);
    if (size > 32) {
        // Buffer the first 32 bytes and wait for the second half.
        std::memcpy(cmd_buffer_, data, 32);
        cmd_buffer_fill_ = 32;
        cmd_expected_size_ = size;
        return;
    }

    // 32-byte command: dispatch directly.
    uint32_t pcw_raw = read_u32(data, 0x00);
    ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

    switch (pcw.para_type) {
        case ParaType::EndOfList:
        case ParaType::UserTileClip:
        case ParaType::ObjectListSet:
            process_control_param(data);
            break;

        case ParaType::Polygon:
        case ParaType::Sprite:
            process_global_param(data);
            break;

        case ParaType::Vertex:
            process_vertex_param(data);
            break;

        default:
            // Reserved or unknown parameter type - ignore
            break;
    }
}

// ============================================================================
// Control parameters
// ============================================================================

void TA::process_control_param(const uint8_t* data) {
    uint32_t pcw_raw = read_u32(data, 0x00);
    ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

    switch (pcw.para_type) {
        case ParaType::EndOfList: {
            // Finalize the current list (write end-of-list markers in every
            // tile's OPB, even if no polygons were submitted for this list).
            finalize_list(current_list_type_);

            // Set the appropriate interrupt flag
            switch (current_list_type_) {
                case ListType::Opaque:
                    opaque_list_complete = true;
                    break;
                case ListType::OpaqueModifierVolume:
                    opaque_modifier_complete = true;
                    break;
                case ListType::Translucent:
                    translucent_list_complete = true;
                    break;
                case ListType::TranslucentModifierVolume:
                    translucent_modifier_complete = true;
                    break;
                case ListType::PunchThrough:
                    punch_through_complete = true;
                    break;
            }

            // Reset for next list
            state_ = State::WaitingForGlobalOrControl;
            list_type_set_ = false;
            vertex_count_ = 0;
            strip_vertices_.clear();
            user_clip_mode_ = 0;
            break;
        }

        case ParaType::UserTileClip: {
            // Read clip coordinates from the command data
            // Offset 0x10: X min (tile coords), 0x14: Y min, 0x18: X max, 0x1C: Y max
            user_clip_x_min_ = static_cast<int>(read_u32(data, 0x10));
            user_clip_y_min_ = static_cast<int>(read_u32(data, 0x14));
            user_clip_x_max_ = static_cast<int>(read_u32(data, 0x18));
            user_clip_y_max_ = static_cast<int>(read_u32(data, 0x1C));
            user_clip_enabled_ = true;
            break;
        }

        case ParaType::ObjectListSet: {
            // Direct object list insertion - read the Object Pointer from offset 0x04
            uint32_t obj_pointer = read_u32(data, 0x04);

            // Read bounding box in tile coordinates from offsets 0x10-0x1C
            int bbox_tile_x_min = static_cast<int>(read_u32(data, 0x10));
            int bbox_tile_y_min = static_cast<int>(read_u32(data, 0x14));
            int bbox_tile_x_max = static_cast<int>(read_u32(data, 0x18));
            int bbox_tile_y_max = static_cast<int>(read_u32(data, 0x1C));

            // Clamp to global clip area
            if (bbox_tile_x_min < 0) bbox_tile_x_min = 0;
            if (bbox_tile_y_min < 0) bbox_tile_y_min = 0;
            if (bbox_tile_x_max > glob_clip_x_) bbox_tile_x_max = glob_clip_x_;
            if (bbox_tile_y_max > glob_clip_y_) bbox_tile_y_max = glob_clip_y_;

            // Register the object pointer in all tiles within the bounding box
            for (int ty = bbox_tile_y_min; ty <= bbox_tile_y_max; ++ty) {
                for (int tx = bbox_tile_x_min; tx <= bbox_tile_x_max; ++tx) {
                    register_object_in_tile(tx, ty, current_list_type_, obj_pointer);
                }
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Global parameters
// ============================================================================

void TA::process_global_param(const uint8_t* data) {
    uint32_t pcw_raw = read_u32(data, 0x00);
    ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

    if (pcw.para_type == ParaType::Sprite) {
        // Sprites always transmit UV as 16-bit packed pairs, and always
        // carry both a Base Color and an Offset Color (KOS's
        // pvr_sprite_compile() unconditionally sets PVR_UVFMT_16BIT and
        // always populates argb/oargb, regardless of whether the game
        // bothered to set the PCW's uv/offset bits). Force these so the
        // skip value and vertex storage below stay correct even when a
        // sprite's PCW leaves them unset - otherwise the offset color
        // silently fails to be stored.
        pcw.uv_16bit = true;
        pcw.offset   = true;
    }

    // A new global parameter changes ISP/TSP/TCW state, so any
    // previous stacking run is invalidated.
    stack_active_ = false;

    // Store the current PCW
    current_pcw_ = pcw;

    // Set list type from PCW if this is the first global param after init or end-of-list
    if (!list_type_set_) {
        current_list_type_ = pcw.list_type;
        list_type_set_ = true;
    }

    // Parse ISP/TSP Instruction Word from offset 0x04
    uint32_t isp_raw = read_u32(data, 0x04);
    current_isp_tsp_ = ISPTSPInstructionWord::parse(isp_raw);

    // Overwrite ISP/TSP Instruction Word bits 25-22 with PCW bits 3-0
    // (texture, offset, gouraud, 16bit_uv)
    current_isp_tsp_.texture  = pcw.texture;
    current_isp_tsp_.offset   = pcw.offset;
    current_isp_tsp_.gouraud  = pcw.gouraud;
    current_isp_tsp_.uv_16bit = pcw.uv_16bit;

    bool is_modifier_volume = (pcw.para_type == ParaType::Polygon) &&
                               (current_list_type_ == ListType::OpaqueModifierVolume ||
                                current_list_type_ == ListType::TranslucentModifierVolume);

    if (pcw.para_type == ParaType::Sprite) {
        // Sprite global parameter
        // Parse TSP and TCW
        current_tsp_ = TSPInstructionWord::parse(read_u32(data, 0x08));
        current_tcw_ = TextureControlWord::parse(read_u32(data, 0x0C));

        // Read sprite base/offset colors
        sprite_base_color_ = read_u32(data, 0x10);
        sprite_offset_color_ = read_u32(data, 0x14);

    } else if (is_modifier_volume) {
        // Modifier Volume: just the ISP/TSP Instruction Word (already parsed)
        // No TSP or TCW needed

    } else {
        // Polygon type - parse TSP and TCW
        current_tsp_ = TSPInstructionWord::parse(read_u32(data, 0x08));
        current_tcw_ = TextureControlWord::parse(read_u32(data, 0x0C));

        // Determine the data format based on col_type, texture, offset, volume bits
        if (pcw.volume && !is_modifier_volume) {
            // Two Volumes: read second TSP from 0x10, second TCW from 0x14
            current_tsp_1_ = TSPInstructionWord::parse(read_u32(data, 0x10));
            current_tcw_1_ = TextureControlWord::parse(read_u32(data, 0x14));

        } else if (pcw.col_type == ColorType::IntensityMode1) {
            // Intensity Mode 1: read Face Color from offsets 0x10-0x1C
            face_color_a_ = read_float(data, 0x10);
            face_color_r_ = read_float(data, 0x14);
            face_color_g_ = read_float(data, 0x18);
            face_color_b_ = read_float(data, 0x1C);

        } else if (pcw.col_type == ColorType::IntensityMode2 && pcw.offset) {
            // Intensity Mode 2 with offset: also read Face Offset Color from 0x20-0x2C
            // Note: this is a 64-byte command, with face offset at second 32 bytes
            face_offset_a_ = read_float(data, 0x20);
            face_offset_r_ = read_float(data, 0x24);
            face_offset_g_ = read_float(data, 0x28);
            face_offset_b_ = read_float(data, 0x2C);
        }
    }

    // Update strip_len from group control if group_en is set
    if (pcw.group_en) {
        // strip_len encoding: 0=1, 1=2, 2=4, 3=6
        switch (pcw.strip_len) {
            case 0: strip_len_ = 1; break;
            case 1: strip_len_ = 2; break;
            case 2: strip_len_ = 4; break;
            case 3: strip_len_ = 6; break;
            default: strip_len_ = 1; break;
        }

        // Update user clip mode from PCW
        user_clip_mode_ = pcw.user_clip;
    }

    // Reset vertex state for the new polygon
    vertex_count_ = 0;
    strip_vertices_.clear();

    // Transition to waiting for vertices
    state_ = State::WaitingForVertex;
}

// ============================================================================
// Vertex parameters
// ============================================================================

void TA::process_vertex_param(const uint8_t* data) {
    if (state_ != State::WaitingForVertex) return;

    uint32_t pcw_raw = read_u32(data, 0x00);
    ParameterControlWord pcw = ParameterControlWord::parse(pcw_raw);

    bool is_modifier_volume = (current_list_type_ == ListType::OpaqueModifierVolume ||
                                current_list_type_ == ListType::TranslucentModifierVolume);
    bool is_sprite = (current_pcw_.para_type == ParaType::Sprite);

    if (is_sprite) {
        // ===== Sprite vertex (single 64-byte command) =====
        //
        // Sprite Type 1 layout (textured):
        //   0x04 AX   0x08 AY   0x0C AZ
        //   0x10 BX   0x14 BY   0x18 BZ
        //   0x1C CX   0x20 CY   0x24 CZ
        //   0x28 DX   0x2C DY   (DZ computed from the A/B/C plane)
        //   0x30 (ignored)
        //   0x34 AU/AV  0x38 BU/BV  0x3C CU/CV  (16-bit half-float pairs)
        //   DU/DV derived: D = B + C - A  (parallelogram fourth corner)
        //
        // Sprite Type 0 (non-textured) has the same layout; 0x34–0x3C ignored.

        Vertex va, vb, vc, vd;

        va.x = read_float(data, 0x04);
        va.y = read_float(data, 0x08);
        va.z = read_float(data, 0x0C);

        vb.x = read_float(data, 0x10);
        vb.y = read_float(data, 0x14);
        vb.z = read_float(data, 0x18);

        vc.x = read_float(data, 0x1C);
        vc.y = read_float(data, 0x20);
        vc.z = read_float(data, 0x24);

        // D has X and Y only; Z is computed from the A/B/C plane so that
        // all four corners are coplanar (matching CORE behaviour).
        vd.x = read_float(data, 0x28);
        vd.y = read_float(data, 0x2C);
        {
            float e1x = vb.x - va.x;
            float e1y = vb.y - va.y;
            float e2x = vc.x - va.x;
            float e2y = vc.y - va.y;
            float det = e1x * e2y - e1y * e2x;
            if (std::fabs(det) > 1e-12f) {
                float dx = vd.x - va.x;
                float dy = vd.y - va.y;
                float a = (dx * e2y - dy * e2x) / det;
                float b = (e1x * dy - e1y * dx) / det;
                vd.z = va.z + a * (vb.z - va.z) + b * (vc.z - va.z);
            } else {
                vd.z = va.z;
            }
        }

        va.base_color = vb.base_color = vc.base_color = vd.base_color = sprite_base_color_;
        va.offset_color = vb.offset_color = vc.offset_color = vd.offset_color = sprite_offset_color_;
        va.u = va.v = vb.u = vb.v = vc.u = vc.v = vd.u = vd.v = 0.0f;

        if (current_pcw_.texture) {
            // 16-bit UV values are the upper 16 bits of a 32-bit IEEE 754
            // single-precision float, with the lower 16 bits discarded.
            // Decode by placing the 16 bits back into the high word.
            auto decode_uv16 = [](uint32_t packed, float& u, float& v) {
                uint32_t ubits = packed & 0xFFFF0000u;
                uint32_t vbits = (packed & 0x0000FFFFu) << 16;
                std::memcpy(&u, &ubits, 4);
                std::memcpy(&v, &vbits, 4);
            };

            decode_uv16(read_u32(data, 0x34), va.u, va.v);
            decode_uv16(read_u32(data, 0x38), vb.u, vb.v);
            decode_uv16(read_u32(data, 0x3C), vc.u, vc.v);

            // D is the fourth corner of the parallelogram.  A and C are the
            // diagonal pair (A=top-left, C=bottom-right), so D = A + C - B.
            vd.u = va.u + vc.u - vb.u;
            vd.v = va.v + vc.v - vb.v;
        }

        strip_vertices_.clear();
        strip_vertices_.push_back(va);
        strip_vertices_.push_back(vb);
        strip_vertices_.push_back(vc);
        strip_vertices_.push_back(vd);

        vertex_count_ = 0;

        // Emit the sprite quad
        if (strip_vertices_.size() >= 4) {
            // Object pointer start_address is resolved at render time as
            // PARAM_BASE + start_address*4; key off TA_ISP_BASE so that
            // the encoding is correct when PARAM_BASE is stale during
            // the TA pass.
            uint32_t param_base = isp_base_;
            uint32_t isp_addr = write_isp_tsp_params(strip_vertices_.data(), 4);
            uint32_t word_addr = (isp_addr - param_base) / 4;

            int skip = calc_skip_value();
            uint32_t obj_ptr = make_array_object_pointer(word_addr, 1, true,
                                                          current_pcw_.shadow, skip);

            int tx_min, ty_min, tx_max, ty_max;
            calc_bounding_box(strip_vertices_.data(), 4, tx_min, ty_min, tx_max, ty_max);

            for (int ty = ty_min; ty <= ty_max; ++ty) {
                for (int tx = tx_min; tx <= tx_max; ++tx) {
                    register_object_in_tile(tx, ty, current_list_type_, obj_ptr);
                }
            }

            strip_vertices_.clear();
        }

        state_ = State::WaitingForVertex;
        return;
    }

    if (is_modifier_volume) {
        // ===== Modifier Volume vertex =====
        // One 64-byte parameter holds a complete triangle: vertices A, B and C
        // (spec §3.7.5.3, "Modifier Volume" vertex format):
        //   AX 0x04  AY 0x08  AZ 0x0C
        //   BX 0x10  BY 0x14  BZ 0x18
        //   CX 0x1C  CY 0x20  CZ 0x24
        // process_command() has already assembled both 32-byte halves here.
        Vertex tri[3];
        for (int i = 0; i < 3; ++i) {
            int base = 0x04 + i * 0x0C;
            tri[i].x = read_float(data, base + 0x00);
            tri[i].y = read_float(data, base + 0x04);
            tri[i].z = read_float(data, base + 0x08);
            tri[i].u = 0; tri[i].v = 0;
            tri[i].base_color = 0;
            tri[i].offset_color = 0;
        }

        // Object pointer start_address is resolved at render time as
        // PARAM_BASE + start_address*4; key off TA_ISP_BASE so that the
        // encoding is correct when PARAM_BASE is stale during the TA pass.
        uint32_t param_base = isp_base_;
        uint32_t isp_addr = write_isp_tsp_params(tri, 3);
        uint32_t word_addr = (isp_addr - param_base) / 4;

        // The TA only supports single-triangle Modifier Volumes (it does not
        // support Modifier Volume strips -- see spec §3.7.7). Emit a
        // one-triangle Triangle Array object pointer. Modifier Volume vertex
        // data is X/Y/Z only (3 words/vertex), so the skip field is 0. These
        // OPs are not stacked (stack_active_ is cleared by the preceding
        // global parameter), so each triangle keeps its own OPB slot.
        uint32_t obj_ptr = make_array_object_pointer(word_addr, 1,
                                                     /*is_quad=*/false,
                                                     current_pcw_.shadow,
                                                     /*skip=*/0);

        int tx_min, ty_min, tx_max, ty_max;
        calc_bounding_box(tri, 3, tx_min, ty_min, tx_max, ty_max);

        // The ISP folds a volume's parity in at its last triangle, so the
        // TA puts that triangle in every tile of the volume's bounding
        // box, not just its own: otherwise a tile the volume touches but
        // its last triangle doesn't keeps a stale parity.
        if (mv_open_) {
            tx_min = std::min(tx_min, mv_tx_min_);
            ty_min = std::min(ty_min, mv_ty_min_);
            tx_max = std::max(tx_max, mv_tx_max_);
            ty_max = std::max(ty_max, mv_ty_max_);
        }
        bool last = current_isp_tsp_.volume_instruction == VolumeInstruction::InsideLastPolygon ||
                    current_isp_tsp_.volume_instruction == VolumeInstruction::OutsideLastPolygon;
        if (last) {
            mv_open_ = false;
            for (int ty = ty_min; ty <= ty_max; ++ty) {
                for (int tx = tx_min; tx <= tx_max; ++tx) {
                    register_object_in_tile(tx, ty, current_list_type_, obj_ptr);
                }
            }
        } else {
            int own_tx_min, own_ty_min, own_tx_max, own_ty_max;
            calc_bounding_box(tri, 3, own_tx_min, own_ty_min, own_tx_max, own_ty_max);
            for (int ty = own_ty_min; ty <= own_ty_max; ++ty) {
                for (int tx = own_tx_min; tx <= own_tx_max; ++tx) {
                    register_object_in_tile(tx, ty, current_list_type_, obj_ptr);
                }
            }
            mv_open_ = true;
            mv_tx_min_ = tx_min;
            mv_ty_min_ = ty_min;
            mv_tx_max_ = tx_max;
            mv_ty_max_ = ty_max;
        }

        vertex_count_ = 0;
        strip_vertices_.clear();
        return;
    }

    // ===== Polygon vertex =====
    Vertex v;
    v.x = read_float(data, 0x04);
    v.y = read_float(data, 0x08);
    v.z = read_float(data, 0x0C);
    v.u = 0;
    v.v = 0;
    v.base_color = 0xFFFFFFFF;
    v.offset_color = 0;

    // Vertex command color offsets per §3.7.5.3 (applies to Packed/Intensity types):
    //   Type 0 (non-tex packed):      Base=0x18
    //   Type 2 (non-tex intensity):   Base=0x18
    //   Types 3/4 (tex packed):       U/V=0x10/0x14  Base=0x18  Offset=0x1C
    //   Types 7/8 (tex intensity):    U/V=0x10/0x14  Base=0x18  Offset=0x1C
    //   FloatingColor handled separately below via fc_base.
    int color_offset = 0x18;
    if (current_pcw_.texture) {
        if (current_pcw_.uv_16bit) {
            // 16-bit packed UV: upper 16 bits = U, lower 16 bits = V.
            // Each 16-bit value is the upper 16 bits of a 32-bit IEEE 754
            // single; decode by shifting back into the high word.
            uint32_t uv_packed = read_u32(data, 0x10);
            uint32_t ubits = uv_packed & 0xFFFF0000u;
            uint32_t vbits = (uv_packed & 0x0000FFFFu) << 16;
            std::memcpy(&v.u, &ubits, 4);
            std::memcpy(&v.v, &vbits, 4);
            // Polygon Type 4: 0x10 = U/V packed,
            // 0x14 = ignored, 0x18 = Base Color, 0x1C = Offset Color
            color_offset = 0x18;
        } else {
            // 32-bit UV at offsets 0x10 (U) and 0x14 (V)
            v.u = read_float(data, 0x10);
            v.v = read_float(data, 0x14);
            color_offset = 0x18; // Color follows 32-bit UV pair
        }
    }

    // Read Base Color
    switch (current_pcw_.col_type) {
        case ColorType::PackedColor: {
            v.base_color = read_u32(data, color_offset);
            break;
        }
        case ColorType::FloatingColor: {
            // Non-textured: A/R/G/B at 0x10/0x14/0x18/0x1C
            // Textured:     A/R/G/B at 0x20/0x24/0x28/0x2C (UV occupies 0x10-0x14)
            int fc_base = current_pcw_.texture ? 0x20 : 0x10;
            float a = read_float(data, fc_base + 0x00);
            float r = read_float(data, fc_base + 0x04);
            float g = read_float(data, fc_base + 0x08);
            float b = read_float(data, fc_base + 0x0C);
            v.base_color = (static_cast<uint32_t>(float_to_u8(a)) << 24) |
                           (static_cast<uint32_t>(float_to_u8(r)) << 16) |
                           (static_cast<uint32_t>(float_to_u8(g)) <<  8) |
                           (static_cast<uint32_t>(float_to_u8(b)));
            break;
        }
        case ColorType::IntensityMode1:
        case ColorType::IntensityMode2: {
            // Read intensity float at color_offset
            float intensity = read_float(data, color_offset);
            v.base_color = convert_intensity_to_packed(intensity,
                                                        face_color_a_, face_color_r_,
                                                        face_color_g_, face_color_b_);
            break;
        }
    }

    // Read Offset Color if offset bit is set
    if (current_pcw_.offset) {
        int offset_color_pos = color_offset + 4;
        switch (current_pcw_.col_type) {
            case ColorType::PackedColor: {
                v.offset_color = read_u32(data, offset_color_pos);
                break;
            }
            case ColorType::FloatingColor: {
                // Offset color starts 0x10 bytes after base color
                // Non-textured: 0x20/0x24/0x28/0x2C; Textured: 0x30/0x34/0x38/0x3C
                int fc_off_base = current_pcw_.texture ? 0x30 : 0x20;
                float a = read_float(data, fc_off_base + 0x00);
                float r = read_float(data, fc_off_base + 0x04);
                float g = read_float(data, fc_off_base + 0x08);
                float b = read_float(data, fc_off_base + 0x0C);
                v.offset_color = (static_cast<uint32_t>(float_to_u8(a)) << 24) |
                                 (static_cast<uint32_t>(float_to_u8(r)) << 16) |
                                 (static_cast<uint32_t>(float_to_u8(g)) <<  8) |
                                 (static_cast<uint32_t>(float_to_u8(b)));
                break;
            }
            case ColorType::IntensityMode1:
            case ColorType::IntensityMode2: {
                float intensity = read_float(data, offset_color_pos);
                v.offset_color = convert_intensity_to_packed(intensity,
                                                              face_offset_a_, face_offset_r_,
                                                              face_offset_g_, face_offset_b_);
                break;
            }
        }
    }

    // Add vertex to the strip
    strip_vertices_.push_back(v);
    vertex_count_++;

    // Check if we should emit the strip
    bool should_emit = pcw.end_of_strip;

    // Also emit if we've hit strip_len triangles worth of vertices
    // (strip_len triangles = strip_len + 2 vertices)
    if (!should_emit && vertex_count_ >= (strip_len_ + 2)) {
        should_emit = true;
    }

    if (should_emit && strip_vertices_.size() >= 3) {
        // Object pointer start_address is resolved at render time as
        // PARAM_BASE + start_address*4; key off TA_ISP_BASE so that the
        // encoding is correct when PARAM_BASE is stale during the TA
        // pass.
        uint32_t param_base = isp_base_;
        int skip = calc_skip_value();

        // Write the strip to ISP/TSP parameters (always with a fresh header).
        uint32_t isp_addr = write_isp_tsp_params(strip_vertices_.data(),
                                                   static_cast<int>(strip_vertices_.size()));
        uint32_t word_addr = (isp_addr - param_base) / 4;

        // Number of triangles in this emission (up to 6 - one Triangle
        // Strip object pointer can cover at most 6 triangles via the
        // 6-bit per-triangle mask).
        int num_triangles = static_cast<int>(strip_vertices_.size()) - 2;
        if (num_triangles > 6) num_triangles = 6;

        // Isolated single-triangle emissions (which is what libgl's
        // GL_TRIANGLES mode produces, V V V_EOL, V V V_EOL, ...) are
        // emitted as Triangle *Array* object pointers rather than
        // single-triangle Triangle Strip OPs. Triangle Array OPs have
        // a 4-bit "Number of Triangles - 1" field and live in a
        // shared OPB slot with adjacent array OPs that have the same
        // header bits (shadow / skip / list bits): the TA stacks
        // consecutive single-triangle Triangle Array entries for the
        // same tile into one OPB slot by bumping the count instead of
        // allocating a new slot.
        //
        // Without this libgl-style scenes register one OPB slot per
        // triangle per covered tile, exhausting the OPB region in
        // dense scenes even though real hardware runs them fine. The
        // stacking check itself is performed in register_object_in_tile.
        //
        // Real DC software that submits long triangle strips (native
        // KOS demos etc) still uses the Triangle Strip path with the
        // per-tile mask, because those emissions contain
        // num_triangles > 1.
        const bool use_tri_array = (num_triangles == 1);

        // Per-tile mask optimisation for multi-triangle strips.
        //
        // A Triangle Strip object pointer carries a 6-bit mask
        // (bits 30-25, T0..T5) selecting which of the up-to-six
        // triangles in the strip are in the registered tile. Real
        // TAs set only the bits for triangles whose individual
        // bounding box includes the tile, so tiles that the strip's
        // overall bbox passes over but no individual triangle covers
        // get no OP entry at all. Matching that saves OPB slots on
        // long / diagonal strips.
        //
        // For Triangle Array OPs (use_tri_array == true) we don't
        // use the 6-bit mask at all; the per-triangle bbox is just
        // the whole strip bbox (1 triangle), and stacking in
        // register_object_in_tile removes the per-tile duplication.
        struct TriBox { int tx_min, ty_min, tx_max, ty_max; };
        TriBox tri_box[6];
        int union_tx_min = 0, union_ty_min = 0;
        int union_tx_max = -1, union_ty_max = -1;
        for (int i = 0; i < num_triangles; ++i) {
            calc_bounding_box(&strip_vertices_[i], 3,
                              tri_box[i].tx_min, tri_box[i].ty_min,
                              tri_box[i].tx_max, tri_box[i].ty_max);
            if (i == 0 || tri_box[i].tx_min < union_tx_min) union_tx_min = tri_box[i].tx_min;
            if (i == 0 || tri_box[i].ty_min < union_ty_min) union_ty_min = tri_box[i].ty_min;
            if (i == 0 || tri_box[i].tx_max > union_tx_max) union_tx_max = tri_box[i].tx_max;
            if (i == 0 || tri_box[i].ty_max > union_ty_max) union_ty_max = tri_box[i].ty_max;
        }

        if (num_triangles > 0 && union_tx_max >= union_tx_min &&
            union_ty_max >= union_ty_min) {
            for (int ty = union_ty_min; ty <= union_ty_max; ++ty) {
                for (int tx = union_tx_min; tx <= union_tx_max; ++tx) {
                    if (use_tri_array) {
                        // Single-triangle emission: only the one
                        // triangle bbox matters; skip tiles it
                        // doesn't touch.
                        const TriBox& b = tri_box[0];
                        if (tx < b.tx_min || tx > b.tx_max ||
                            ty < b.ty_min || ty > b.ty_max) {
                            continue;
                        }
                        uint32_t obj_ptr = make_array_object_pointer(
                            word_addr, 1, /*is_quad=*/false,
                            current_pcw_.shadow, skip);
                        register_object_in_tile(tx, ty, current_list_type_, obj_ptr);
                        // Cache the OP header bits for the stacking check
                        // the NEXT triangle will perform (see
                        // register_object_in_tile).  Saved here so all
                        // tiles get the same header reference.
                        // (stack_hdr_ is only read, never written, inside
                        // the registration loop itself.)
                        stack_hdr_ = obj_ptr & 0xE1E00000u;
                    } else {
                        uint32_t mask = 0;
                        for (int i = 0; i < num_triangles; ++i) {
                            if (tx >= tri_box[i].tx_min && tx <= tri_box[i].tx_max &&
                                ty >= tri_box[i].ty_min && ty <= tri_box[i].ty_max) {
                                // Bit 5 = triangle 0, bit 0 = triangle 5.
                                mask |= (1u << (5 - i));
                            }
                        }
                        if (mask == 0) continue;  // strip doesn't touch this tile

                        uint32_t obj_ptr = make_strip_object_pointer(
                            word_addr, mask,
                            current_pcw_.shadow, skip);
                        register_object_in_tile(tx, ty, current_list_type_, obj_ptr);
                    }
                }
            }
        }

        // Update the Triangle Array stacking state.
        //
        // If this emission was a single-triangle (and therefore used a
        // Triangle Array OP), record its tile bbox so that the NEXT
        // single-triangle emission can stack onto this one's OPB entries
        // for tiles they share.  This mirrors master lxdream's
        // last_triangle_bounds update which happens AFTER registering
        // all tiles, so that registration of the current triangle uses
        // the PREVIOUS triangle's bounds (already in stack_*_) and only
        // then switches the state forward.
        //
        // For multi-triangle strips, or any emission that broke the
        // contiguity guarantee (non-single-triangle, modifier volume,
        // sprite), clear the stacking state so the next emission starts
        // fresh.
        if (num_triangles == 1) {
            // Single-triangle: activate stacking for the next emission.
            stack_active_  = true;
            stack_tx_min_  = tri_box[0].tx_min;
            stack_ty_min_  = tri_box[0].ty_min;
            stack_tx_max_  = tri_box[0].tx_max;
            stack_ty_max_  = tri_box[0].ty_max;
        } else {
            // Multi-triangle strip: VRAM layout is not a simple sequence
            // of Triangle Array blocks, so stacking is not safe here.
            stack_active_ = false;
        }

        // If this was an end_of_strip, clear for the next strip
        if (pcw.end_of_strip) {
            vertex_count_ = 0;
            strip_vertices_.clear();
        } else {
            // For strip_len partitioning: keep the last 2 vertices to continue
            // the strip (they form the starting edge of the next segment)
            Vertex last2[2];
            last2[0] = strip_vertices_[strip_vertices_.size() - 2];
            last2[1] = strip_vertices_[strip_vertices_.size() - 1];
            strip_vertices_.clear();
            strip_vertices_.push_back(last2[0]);
            strip_vertices_.push_back(last2[1]);
            vertex_count_ = 2;
        }
    }
}

// ============================================================================
// Write ISP/TSP parameters to VRAM
// ============================================================================

uint32_t TA::write_isp_tsp_params(const Vertex* vertices, int vertex_count) {
    uint32_t start_addr = isp_write_ptr_;

    // Bounds check: don't allow ISP/TSP parameter writes to overrun
    // TA_ISP_LIMIT (which would corrupt whatever lives above, e.g. textures).
    // We conservatively estimate the worst-case size for this block:
    //   ISP(4) + TSP(4) + TCW(4) + per-vertex (X,Y,Z, UV, base, offset) = 24 bytes
    // = 12 + vertex_count * 24 bytes.
    if (regs_) {
        uint32_t isp_limit = regs_->read(RegisterName::TA_ISP_LIMIT);
        uint32_t worst_case = 12u + static_cast<uint32_t>(vertex_count) * 24u;
        if (isp_limit != 0 && isp_write_ptr_ + worst_case > isp_limit) {
            if (!isp_overflow_reported_) {
                isp_overflow_reported_ = true;
            }
            isp_overflow = true;
            // §3.7.4: "The display list (ISP/TSP Parameters) is not
            // generated correctly in this case, and therefore should not be
            // used for drawing. It is necessary in this case to reconsider
            // the memory allocations and to start over from list
            // initialization."
            //
            // The list is now unusable, not merely wrong - object pointers
            // written from here on reference parameter blocks that were
            // never stored. Latch that separately from the interrupt so it
            // survives the guest acknowledging the interrupt; only
            // list_init() clears it.
            display_list_invalid = true;
            // Return the current pointer without advancing; the caller will
            // still build object pointers referencing this address, but no
            // further objects can actually be rendered safely. In practice
            // register_object_in_tile's own OPB bounds check will also stop
            // dispatching into corrupted regions.
            return start_addr;
        }
    }

    bool is_modifier_volume = (current_list_type_ == ListType::OpaqueModifierVolume ||
                                current_list_type_ == ListType::TranslucentModifierVolume);

    {
        // 1. Reconstruct and write ISP/TSP Instruction Word (with PCW bits merged)
        uint32_t isp_word = 0;
        if (is_modifier_volume) {
            // Modifier volume format
            isp_word |= (static_cast<uint32_t>(current_isp_tsp_.volume_instruction) & 0x7) << 29;
            isp_word |= (static_cast<uint32_t>(current_isp_tsp_.culling_mode) & 0x3) << 27;
        } else {
            isp_word |= (static_cast<uint32_t>(current_isp_tsp_.depth_compare_mode) & 0x7) << 29;
            isp_word |= (static_cast<uint32_t>(current_isp_tsp_.culling_mode) & 0x3) << 27;
            isp_word |= (current_isp_tsp_.z_write_disable ? 1u : 0u) << 26;
            isp_word |= (current_isp_tsp_.texture  ? 1u : 0u) << 25;
            isp_word |= (current_isp_tsp_.offset   ? 1u : 0u) << 24;
            isp_word |= (current_isp_tsp_.gouraud  ? 1u : 0u) << 23;
            isp_word |= (current_isp_tsp_.uv_16bit ? 1u : 0u) << 22;
            isp_word |= (current_isp_tsp_.cache_bypass ? 1u : 0u) << 21;
            isp_word |= (current_isp_tsp_.dcalc_ctrl ? 1u : 0u) << 20;
        }

        vram_write32(isp_write_ptr_, isp_word);
        isp_write_ptr_ += 4;

        // 2. Write TSP Instruction Word (if not modifier volume)
        if (!is_modifier_volume) {
            // Reconstruct TSP word
            uint32_t tsp_word = 0;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.src_alpha_instr) & 0x7) << 29;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.dst_alpha_instr) & 0x7) << 26;
            tsp_word |= (current_tsp_.src_select ? 1u : 0u) << 25;
            tsp_word |= (current_tsp_.dst_select ? 1u : 0u) << 24;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.fog_control) & 0x3) << 22;
            tsp_word |= (current_tsp_.color_clamp ? 1u : 0u) << 21;
            tsp_word |= (current_tsp_.use_alpha ? 1u : 0u) << 20;
            tsp_word |= (current_tsp_.ignore_tex_alpha ? 1u : 0u) << 19;
            tsp_word |= (current_tsp_.flip_u ? 1u : 0u) << 18;
            tsp_word |= (current_tsp_.flip_v ? 1u : 0u) << 17;
            tsp_word |= (current_tsp_.clamp_u ? 1u : 0u) << 16;
            tsp_word |= (current_tsp_.clamp_v ? 1u : 0u) << 15;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.filter_mode) & 0x3) << 13;
            tsp_word |= (current_tsp_.super_sample ? 1u : 0u) << 12;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.mipmap_d_adjust) & 0xF) << 8;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.tex_shading_instr) & 0x3) << 6;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.tex_u_size) & 0x7) << 3;
            tsp_word |= (static_cast<uint32_t>(current_tsp_.tex_v_size) & 0x7);

            vram_write32(isp_write_ptr_, tsp_word);
            isp_write_ptr_ += 4;

            // 3. Write Texture Control Word (ALWAYS present,
            //    even for non-textured polygons; value is unused in that case)
            {
                uint32_t tcw_word = 0;
                if (current_pcw_.texture) {
                    tcw_word |= (current_tcw_.mip_mapped ? 1u : 0u) << 31;
                    tcw_word |= (current_tcw_.vq_compressed ? 1u : 0u) << 30;
                    tcw_word |= (static_cast<uint32_t>(current_tcw_.pixel_format) & 0x7) << 27;
                    if (current_tcw_.is_palette()) {
                        tcw_word |= (static_cast<uint32_t>(current_tcw_.palette_selector) & 0x3F) << 21;
                    } else {
                        tcw_word |= (current_tcw_.scan_order ? 1u : 0u) << 26;
                        tcw_word |= (current_tcw_.stride_select ? 1u : 0u) << 25;
                    }
                    tcw_word |= (current_tcw_.texture_address & 0x1FFFFF);
                }
                vram_write32(isp_write_ptr_, tcw_word);
                isp_write_ptr_ += 4;
            }
        }
    }

    // Write vertex data
    for (int i = 0; i < vertex_count; ++i) {
        const Vertex& vtx = vertices[i];
        uint32_t f;

        // X
        std::memcpy(&f, &vtx.x, 4);
        vram_write32(isp_write_ptr_, f);
        isp_write_ptr_ += 4;

        // Y
        std::memcpy(&f, &vtx.y, 4);
        vram_write32(isp_write_ptr_, f);
        isp_write_ptr_ += 4;

        // Z (1/W)
        std::memcpy(&f, &vtx.z, 4);
        vram_write32(isp_write_ptr_, f);
        isp_write_ptr_ += 4;

        if (!is_modifier_volume) {
            // UV
            if (current_pcw_.texture) {
                if (current_pcw_.uv_16bit) {
                    // 16-bit UV: keep the upper 16 bits of each float's bit
                    // pattern and pack U into the high word, V into the low.
                    uint32_t fu, fv;
                    std::memcpy(&fu, &vtx.u, 4);
                    std::memcpy(&fv, &vtx.v, 4);
                    uint32_t packed_uv = (fu & 0xFFFF0000u) |
                                         (fv >> 16);
                    vram_write32(isp_write_ptr_, packed_uv);
                    isp_write_ptr_ += 4;
                } else {
                    // 32-bit U
                    std::memcpy(&f, &vtx.u, 4);
                    vram_write32(isp_write_ptr_, f);
                    isp_write_ptr_ += 4;
                    // 32-bit V
                    std::memcpy(&f, &vtx.v, 4);
                    vram_write32(isp_write_ptr_, f);
                    isp_write_ptr_ += 4;
                }
            }

            // Base Color
            vram_write32(isp_write_ptr_, vtx.base_color);
            isp_write_ptr_ += 4;

            // Offset Color
            if (current_pcw_.offset) {
                vram_write32(isp_write_ptr_, vtx.offset_color);
                isp_write_ptr_ += 4;
            }
        }
    }

    // Update the TA_ITP_CURRENT register
    if (regs_) {
        regs_->write(RegisterName::TA_ITP_CURRENT, isp_write_ptr_);
    }

    return start_addr;
}

// ============================================================================
// Finalize list
// ============================================================================

void TA::finalize_list(ListType list) {
    int tiles_x = glob_clip_x_ + 1;
    int tiles_y = glob_clip_y_ + 1;

    for (int ty = 0; ty < tiles_y; ++ty) {
        for (int tx = 0; tx < tiles_x; ++tx) {
            TileOPB& opb = get_tile_opb(tx, ty, list);

            if (opb.opb_size == 0) continue;

            // Write end-of-list marker: Pointer Block Link with end_of_list bit set
            // Format: 0xE0000000 | (1 << 28) = 0xF0000000
            // But if there are objects, write it after the last object
            uint32_t marker_addr = opb.current_opb_addr +
                                   static_cast<uint32_t>(opb.obj_count) * 4;
            vram_write32(marker_addr, 0xF0000000u);
        }
    }
}

} // namespace pvr
