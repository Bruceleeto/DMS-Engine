#include "pvr.h"

namespace pvr {

PVR::PVR(uint8_t* vram, size_t vram_size)
    : vram_(vram)
    , vram_size_(vram_size)
{
    // Connect TA and CORE to VRAM and registers
    ta_.set_vram(vram_, vram_size_);
    ta_.set_registers(&regs_);

    core_.set_vram(vram_, vram_size_);
    core_.set_registers(&regs_);
}

// --- Register interface ---

void PVR::write_register(uint32_t offset, uint32_t value) {
    // Handle special register writes that trigger hardware actions
    switch (offset) {
        case static_cast<uint32_t>(RegisterName::SOFTRESET):
            regs_.write(offset, value);
            if (value & 0x1) {
                // TA soft reset
                ta_.reset();
            }
            if (value & 0x2) {
                // CORE soft reset
                core_.reset();
            }
            return;

        case static_cast<uint32_t>(RegisterName::STARTRENDER):
            regs_.write(offset, value);
            core_.start_render();
            return;

        case static_cast<uint32_t>(RegisterName::TA_LIST_INIT):
            regs_.write(offset, value);
            if (value & 0x80000000) {
                ta_.list_init();
            }
            return;

        case static_cast<uint32_t>(RegisterName::TA_LIST_CONT):
            regs_.write(offset, value);
            if (value & 0x80000000) {
                ta_.list_cont();
            }
            return;

        default:
            regs_.write(offset, value);
            return;
    }
}

uint32_t PVR::read_register(uint32_t offset) const {
    return regs_.read(offset);
}

void PVR::write_register(RegisterName name, uint32_t value) {
    write_register(static_cast<uint32_t>(name), value);
}

uint32_t PVR::read_register(RegisterName name) const {
    return regs_.read(name);
}

// --- TA interface ---

void PVR::send_ta_command(const TACommand& command) {
    ta_.process_command(command.bytes);
}

void PVR::send_ta_data(const uint8_t* data, size_t len) {
    // Process in 32-byte chunks
    for (size_t i = 0; i + 32 <= len; i += 32) {
        ta_.process_command(data + i);
    }
}

// --- Rendering ---

void PVR::start_render() {
    write_register(RegisterName::STARTRENDER, 1);
}

void PVR::wait_render_done() {
    core_.wait_render_done();
}

void PVR::start_render_async( std::function<void()> on_done ) {
    /* Write the register (fires TA quiesce / parameter commit side-effects)
     * then hand off to the CORE's async path. */
    regs_.write( static_cast<uint32_t>(RegisterName::STARTRENDER), 1 );
    core_.start_render_async( std::move(on_done) );
}

bool PVR::is_render_complete() const {
    return core_.render_complete;
}

// --- Framebuffer readback ---

void PVR::read_framebuffer(uint8_t* framebuffer_out, int width, int height) const {
    core_.read_framebuffer(framebuffer_out, width, height);
}

// --- Interrupt status ---

bool PVR::is_opaque_list_complete() const {
    return ta_.opaque_list_complete;
}

bool PVR::is_opaque_modifier_complete() const {
    return ta_.opaque_modifier_complete;
}

bool PVR::is_translucent_list_complete() const {
    return ta_.translucent_list_complete;
}

bool PVR::is_translucent_modifier_complete() const {
    return ta_.translucent_modifier_complete;
}

bool PVR::is_punch_through_complete() const {
    return ta_.punch_through_complete;
}

bool PVR::is_opb_overflow() const {
    return ta_.opb_overflow;
}

bool PVR::is_isp_overflow() const {
    return ta_.isp_overflow;
}

bool PVR::is_illegal_parameter() const {
    return ta_.illegal_parameter;
}

bool PVR::is_isp_out_of_cache() const {
    return core_.isp_out_of_cache;
}

bool PVR::is_display_list_invalid() const {
    return ta_.display_list_invalid;
}

void PVR::clear_ta_interrupts() {
    ta_.opaque_list_complete = false;
    ta_.opaque_modifier_complete = false;
    ta_.translucent_list_complete = false;
    ta_.translucent_modifier_complete = false;
    ta_.punch_through_complete = false;
    ta_.opb_overflow = false;
    ta_.isp_overflow = false;
    ta_.illegal_parameter = false;
    core_.isp_out_of_cache = false;
    // ta_.display_list_invalid is deliberately NOT cleared here - see
    // PVR::is_display_list_invalid(). Acknowledging the interrupt does not
    // make a half-written display list usable again; only TA_LIST_INIT does.
}

} // namespace pvr
