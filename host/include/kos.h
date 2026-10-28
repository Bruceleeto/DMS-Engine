#pragma once
/* Only the KOS interfaces used by the DMS desktop preview. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <arch/cache.h>
#include <arch/timer.h>

#define CONT_B (1u << 1)
#define CONT_A (1u << 2)
#define CONT_START (1u << 3)
#define CONT_DPAD_UP (1u << 4)
#define CONT_DPAD_DOWN (1u << 5)
#define CONT_DPAD_LEFT (1u << 6)
#define CONT_DPAD_RIGHT (1u << 7)
#define CONT_Y (1u << 9)
#define CONT_X (1u << 10)
#define MAPLE_FUNC_CONTROLLER 1
typedef struct { int port; } maple_device_t;
typedef struct {
    uint32_t buttons;
    int joyx, joyy, joy2x, joy2y;
    unsigned ltrig, rtrig;
} cont_state_t;
maple_device_t* maple_enum_type(int port, int function);
void* maple_dev_status(maple_device_t* device);
