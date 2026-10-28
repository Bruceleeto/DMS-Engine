#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pvr_types.h"
#include "registers.h"
#include "ta.h"
#include "core.h"

namespace pvr {

// 32-byte TA command
struct TACommand {
    uint8_t bytes[32];
};

// Main PVR (PowerVR CLX2 / HOLLY2) interface
// This class owns the register bank and connects the TA and CORE to VRAM.
class PVR {
public:
    // Construct with pointer to external VRAM buffer.
    // vram_size is typically 8MB (0x800000) for the Dreamcast.
    PVR(uint8_t* vram, size_t vram_size);

    // --- Register interface ---

    // Write to a PVR register (offset from 0x005F8000).
    // Some writes trigger hardware actions (STARTRENDER, TA_LIST_INIT, etc.)
    void write_register(uint32_t offset, uint32_t value);

    // Read a PVR register (offset from 0x005F8000).
    uint32_t read_register(uint32_t offset) const;

    // Convenience: write/read by RegisterName enum
    void write_register(RegisterName name, uint32_t value);
    uint32_t read_register(RegisterName name) const;

    // --- TA interface ---

    // Send a 32-byte command to the Tile Accelerator.
    // This is the primary way polygon data is submitted.
    void send_ta_command(const TACommand& command);

    // Send raw 32-byte data to the TA (same as above but from raw pointer)
    void send_ta_data(const uint8_t* data, size_t len);

    // --- Save / load state ---

    // Save/restore the TA's internal state to/from a FILE*.
    // VRAM and the register bank are NOT included - callers are expected
    // to save/restore those separately.
    // Returns true on success.
    bool save_ta_state(std::FILE* f) const { return ta_.save_state(f); }
    bool load_ta_state(std::FILE* f)       { return ta_.load_state(f); }

    // --- Rendering ---

    // Trigger rendering (same as writing to STARTRENDER register).
    // Blocks until the render is complete.
    void start_render();

    // Block until any in-flight async render completes.  No-op if no render
    // is running.
    void wait_render_done();

    // Async variant: returns immediately and calls on_done() on the render
    // thread when the frame is complete.
    void start_render_async(std::function<void()> on_done);

    // Check if rendering is complete.
    bool is_render_complete() const;

    // --- Framebuffer readback ---

    // Read the framebuffer contents into an ARGB8888 buffer.
    // width and height should match the configured display resolution.
    void read_framebuffer(uint8_t* framebuffer_out, int width, int height) const;

    // --- Interrupt status ---

    // Check and clear TA list completion interrupts
    bool is_opaque_list_complete() const;
    bool is_opaque_modifier_complete() const;
    bool is_translucent_list_complete() const;
    bool is_translucent_modifier_complete() const;
    bool is_punch_through_complete() const;

    // TA fault latches, mapping to the TA half of SB_ISTERR:
    //   is_opb_overflow()      bit 3, Object List Pointer Overflow
    //   is_isp_overflow()      bit 2, ISP/TSP Parameter Overflow
    //   is_illegal_parameter() bit 4, Illegal Parameter
    bool is_opb_overflow() const;
    bool is_isp_overflow() const;
    bool is_illegal_parameter() const;

    // CORE fault latch: SB_ISTERR bit 0, RENDER: ISP out of Cache.
    bool is_isp_out_of_cache() const;

    // True once an ISP/TSP parameter overflow has left the display list
    // unusable for drawing. Unlike the latches above this is NOT cleared by
    // clear_ta_interrupts() - per §3.7.4 only starting over from list
    // initialization (TA_LIST_INIT) recovers.
    bool is_display_list_invalid() const;

    void clear_ta_interrupts();

    // --- Direct access (for advanced use) ---
    RegisterBank& registers() { return regs_; }
    const RegisterBank& registers() const { return regs_; }
    TA& tile_accelerator() { return ta_; }
    CORE& core() { return core_; }

private:
    uint8_t* vram_;
    size_t vram_size_;

    RegisterBank regs_;
    TA ta_;
    CORE core_;
};

} // namespace pvr
