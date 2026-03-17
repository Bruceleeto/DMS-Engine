#include "dc_input.h"
#include <string.h>
#include <math.h>

#define DC_INPUT_PORTS 4
#define DC_DEFAULT_DEADZONE 0.15f

static DCInput g_inputs[DC_INPUT_PORTS];
static uint32_t g_prev_buttons[DC_INPUT_PORTS];
static float g_deadzone = DC_DEFAULT_DEADZONE;

static float apply_deadzone(float val) {
    if (val > -g_deadzone && val < g_deadzone) return 0.0f;
    return val;
}

void dc_input_poll(void) {
    for (int port = 0; port < DC_INPUT_PORTS; port++) {
        DCInput* inp = &g_inputs[port];
        uint32_t prev = g_prev_buttons[port];

        maple_device_t* cont = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
        if (!cont) {
            memset(inp, 0, sizeof(DCInput));
            g_prev_buttons[port] = 0;
            continue;
        }

        cont_state_t* state = (cont_state_t*)maple_dev_status(cont);
        if (!state) {
            memset(inp, 0, sizeof(DCInput));
            g_prev_buttons[port] = 0;
            continue;
        }

        inp->connected = true;

        /* Sticks: joyx/joyy are signed -128..127, normalize to -1..1 */
        inp->stick_x = apply_deadzone((float)state->joyx / 128.0f);
        inp->stick_y = apply_deadzone((float)state->joyy / 128.0f);
        inp->stick2_x = apply_deadzone((float)state->joy2x / 128.0f);
        inp->stick2_y = apply_deadzone((float)state->joy2y / 128.0f);

        /* Triggers: 0-255 → 0.0-1.0 */
        inp->ltrig = (float)state->ltrig / 255.0f;
        inp->rtrig = (float)state->rtrig / 255.0f;

        /* Buttons + pressed/released detection */
        inp->buttons  = state->buttons;
        inp->pressed  = state->buttons & ~prev;    /* newly down */
        inp->released = ~state->buttons & prev;     /* newly up */

        g_prev_buttons[port] = state->buttons;
    }
}

const DCInput* dc_input_get(int port) {
    if (port < 0 || port >= DC_INPUT_PORTS) return NULL;
    return &g_inputs[port];
}

bool dc_input_pressed(const DCInput* inp, uint32_t btn) {
    return inp && (inp->pressed & btn) != 0;
}

bool dc_input_held(const DCInput* inp, uint32_t btn) {
    return inp && (inp->buttons & btn) != 0;
}

void dc_input_set_deadzone(float deadzone) {
    g_deadzone = deadzone;
}
