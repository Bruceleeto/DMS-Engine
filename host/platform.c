#include <kos.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <errno.h>
#include "pvr_host.h"

static maple_device_t keyboard = {0};
static cont_state_t state;
static unsigned frame_limit, frames;
static bool quit, headless;

maple_device_t* maple_enum_type(int port, int function) {
    return port == 0 && function == MAPLE_FUNC_CONTROLLER ? &keyboard : NULL;
}

void* maple_dev_status(maple_device_t* device) {
    (void)device;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) quit = true;
    }
    const bool* keys = SDL_GetKeyboardState(NULL);
    memset(&state, 0, sizeof(state));
    if (quit || keys[SDL_SCANCODE_ESCAPE] || (frame_limit && ++frames >= frame_limit))
        state.buttons |= CONT_START;
    if (keys[SDL_SCANCODE_W]) state.buttons |= CONT_DPAD_UP;
    if (keys[SDL_SCANCODE_S]) state.buttons |= CONT_DPAD_DOWN;
    if (keys[SDL_SCANCODE_A]) state.buttons |= CONT_DPAD_LEFT;
    if (keys[SDL_SCANCODE_D]) state.buttons |= CONT_DPAD_RIGHT;
    if (keys[SDL_SCANCODE_SPACE]) state.buttons |= CONT_A;
    if (keys[SDL_SCANCODE_R]) state.buttons |= CONT_Y;
    state.joyx = 100 * (keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT]);
    state.joyy = 100 * (keys[SDL_SCANCODE_DOWN] - keys[SDL_SCANCODE_UP]);
    state.rtrig = keys[SDL_SCANCODE_LSHIFT] ? 255 : 0;
    if (!headless) SDL_Delay(1);
    return &state;
}

int dms_example_main(int argc, char** argv);

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            char* end;
            errno = 0;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (errno || *end || !n || n > 1000000) {
                fprintf(stderr, "Invalid frame count\n"); return 2;
            }
            frame_limit = n;
        } else {
            fprintf(stderr, "Usage: %s [--headless] [--frames N]\n", argv[0]);
            return 2;
        }
    }
    if (headless && !frame_limit) {
        fprintf(stderr, "--headless requires --frames N\n"); return 2;
    }
    if (!SDL_Init(SDL_INIT_EVENTS | (headless ? 0 : SDL_INIT_VIDEO))) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
    }
    if (!headless && pvr_host_open("DMS — desktop preview", 1)) { SDL_Quit(); return 1; }
    puts("DMS preview: WASD move, arrows look, Space jump, Shift sprint, R reset, Escape exit");
    int result = dms_example_main(argc, argv);
    pvr_host_close();
    SDL_Quit();
    return result;
}
