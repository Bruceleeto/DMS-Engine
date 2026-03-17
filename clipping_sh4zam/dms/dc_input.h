#ifndef DC_INPUT_H
#define DC_INPUT_H

#include <kos.h>
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Input state (per controller port)
 * ================================================================ */

typedef struct {
    float    stick_x, stick_y;       /* -1.0 to 1.0, deadzone applied */
    float    stick2_x, stick2_y;     /* second stick if available */
    float    ltrig, rtrig;           /* 0.0 to 1.0 */
    uint32_t buttons;                /* raw button state (KOS CONT_* bits) */
    uint32_t pressed;                /* buttons that went down THIS frame */
    uint32_t released;               /* buttons that went up THIS frame */
    bool     connected;
} DCInput;

/* ================================================================
 * API
 * ================================================================ */

/* Poll all 4 ports. Call once per frame (dc_frame_begin does this). */
void dc_input_poll(void);

/* Get input state for a controller port (0-3). Returns NULL if invalid. */
const DCInput* dc_input_get(int port);

/* Convenience: check if button was pressed this frame. */
bool dc_input_pressed(const DCInput* inp, uint32_t btn);

/* Convenience: check if button is currently held. */
bool dc_input_held(const DCInput* inp, uint32_t btn);

/* Set analog deadzone (default 0.15). Applied to both sticks. */
void dc_input_set_deadzone(float deadzone);

#endif /* DC_INPUT_H */
