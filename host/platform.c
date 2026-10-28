#include <kos.h>
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pvr_host.h"
#include "png.h"
#include "embed.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void DCSim_Info(int* inuse, int* freebytes);

static maple_device_t ports[2] = { { 0, 0 }, { 1, 0 } };
static cont_state_t state, state2;      /* port 1 (the keyboard and a pad), port 2 (a second pad) */
static bool use_pads;                   /* not in a scripted --headless run */
static SDL_Gamepad* pads[2];
static unsigned frame_limit, frames;
static bool quit, headless;
static const char* shot_path;
static DMSEmbed* embed;     /* run inside the editor's Game tab */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Once a frame, inside the editor: the picture out, the pad in, at 60 a
 * second (no window to wait on vsync), and held while paused. */
static void embed_frame(void) {
    static double next, fps_from;
    static unsigned fps_frames;
    pvr_host_read_front(embed->rgb, 640 * 3);
    int used = 0, free_bytes = 0;
    DCSim_Info(&used, &free_bytes);
    embed->heap_used = used;
    embed->heap_free = free_bytes;
    embed->frame++;
    double t = now_s();
    if (!fps_from) fps_from = t;
    if (++fps_frames >= 30) { embed->fps = fps_frames / (t - fps_from); fps_from = t; fps_frames = 0; }
    if (next > t) usleep((useconds_t)((next - t) * 1e6));
    next = (next > t - 0.1 ? next : t) + 1.0 / 60.0;
    while (embed->paused && !embed->step && !embed->quit) { usleep(5000); next = now_s(); fps_from = 0; }
    if (embed->step) embed->step--;
    if (embed->quit) exit(0);
    state.buttons = embed->buttons;
    state.joyx = embed->joyx;
    state.joyy = embed->joyy;
    state.ltrig = embed->ltrig;
    state.rtrig = embed->rtrig;
}

static void save_shot(void) {
    static uint8_t rgb[640 * 480 * 3];
    if (!pvr_host_read_front(rgb, 640 * 3)) { fprintf(stderr, "shot: nothing rendered\n"); return; }
    if (write_png(shot_path, rgb, 640, 480)) fprintf(stderr, "shot: can't write %s\n", shot_path);
    else printf("shot: %s\n", shot_path);
}

/* ---- Gamepads: any SDL knows (Xbox, PlayStation, Switch...) as a
 * Dreamcast pad, by where the buttons are: the bottom one is A, the right
 * B, the left X, the top Y; the left stick, the d-pad, the triggers,
 * Start (and Back) Start. The first is port 1 with the keyboard; a second
 * is port 2. */

static void pads_refresh(void) {
    for (int i = 0; i < 2; i++)
        if (pads[i] && !SDL_GamepadConnected(pads[i])) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
    if (!pads[0] && pads[1]) { pads[0] = pads[1]; pads[1] = NULL; }
    int n;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int k = 0; ids && k < n; k++) {
        if ((pads[0] && SDL_GetGamepadID(pads[0]) == ids[k]) || (pads[1] && SDL_GetGamepadID(pads[1]) == ids[k])) continue;
        int slot = !pads[0] ? 0 : !pads[1] ? 1 : -1;
        if (slot < 0) break;
        if ((pads[slot] = SDL_OpenGamepad(ids[k])))
            printf("pad: %s is port %d\n", SDL_GetGamepadName(pads[slot]), slot + 1);
    }
    SDL_free(ids);
}

static int axis8(SDL_Gamepad* g, SDL_GamepadAxis a) {
    int v = SDL_GetGamepadAxis(g, a) / 256;     /* -128 to 127 */
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

/* The pad on top of what the keyboard has: buttons together, the stick
 * whichever is pushed further */
static void pad_read(SDL_Gamepad* g, cont_state_t* s) {
    if (!g) return;
    static const struct { SDL_GamepadButton b; uint32_t cont; } map[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, CONT_A }, { SDL_GAMEPAD_BUTTON_EAST, CONT_B },
        { SDL_GAMEPAD_BUTTON_WEST, CONT_X },  { SDL_GAMEPAD_BUTTON_NORTH, CONT_Y },
        { SDL_GAMEPAD_BUTTON_START, CONT_START }, { SDL_GAMEPAD_BUTTON_BACK, CONT_START },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, CONT_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, CONT_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, CONT_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, CONT_DPAD_RIGHT },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (SDL_GetGamepadButton(g, map[i].b)) s->buttons |= map[i].cont;
    int x = axis8(g, SDL_GAMEPAD_AXIS_LEFTX), y = axis8(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (abs(x) > abs(s->joyx)) s->joyx = x;
    if (abs(y) > abs(s->joyy)) s->joyy = y;
    s->joy2x = axis8(g, SDL_GAMEPAD_AXIS_RIGHTX);
    s->joy2y = axis8(g, SDL_GAMEPAD_AXIS_RIGHTY);
    unsigned lt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 128;
    unsigned rt = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 128;
    if (lt > s->ltrig) s->ltrig = lt > 255 ? 255 : lt;
    if (rt > s->rtrig) s->rtrig = rt > 255 ? 255 : rt;
}

maple_device_t* maple_enum_type(int port, int function) {
    if (function != MAPLE_FUNC_CONTROLLER) return NULL;
    if (port == 0) return &ports[0];
    return port == 1 && pads[1] ? &ports[1] : NULL;
}

static void poll_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) quit = true;
        if (event.type == SDL_EVENT_GAMEPAD_ADDED || event.type == SDL_EVENT_GAMEPAD_REMOVED) pads_refresh();
    }
}

static cont_state_t* port1(void);

void* maple_dev_status(maple_device_t* device) {
    if (device->port == 1) {
        memset(&state2, 0, sizeof(state2));
        pad_read(pads[1], &state2);
        return &state2;
    }
    return port1();
}

static cont_state_t* port1(void) {
    if (embed) {
        embed_frame();
        if (use_pads) { poll_events(); pad_read(pads[0], &state); }
        return &state;
    }
    poll_events();
    const bool* keys = SDL_GetKeyboardState(NULL);
    memset(&state, 0, sizeof(state));
    bool done = frame_limit && ++frames >= frame_limit;
    if (done && shot_path && frames == frame_limit) save_shot();
    /* A game whose Start pauses instead of quitting (botboy): leave anyway */
    if (frame_limit && frames >= frame_limit + 120) exit(0);
    if (quit || keys[SDL_SCANCODE_ESCAPE] || done)
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
    if (use_pads && !done) pad_read(pads[0], &state);
    if (!headless) SDL_Delay(1);
    return &state;
}

int dms_game_main(int argc, char** argv);

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
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot_path = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [--headless] [--frames N [--shot out.png]]\n", argv[0]);
            return 2;
        }
    }
    if ((headless || shot_path) && !frame_limit) {
        fprintf(stderr, "--headless and --shot need --frames N\n"); return 2;
    }
    const char* embed_path = getenv("DMS_EMBED");
    if (embed_path) {
        int fd = open(embed_path, O_RDWR);
        embed = fd < 0 ? NULL : mmap(NULL, sizeof(DMSEmbed), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fd >= 0) close(fd);
        if (embed == MAP_FAILED || !embed || embed->magic != DMS_EMBED_MAGIC) {
            fprintf(stderr, "DMS_EMBED: cannot map %s\n", embed_path); return 1;
        }
        headless = true;
        setvbuf(stdout, NULL, _IOLBF, 0);   /* the editor's console reads it as it comes */
    }
    /* Pads in a window, and in the editor's Game tab (whether or not it has
     * the keyboard); never in a scripted run, where they would change it */
    use_pads = !headless || embed;
    if (use_pads) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_EVENTS | (headless ? 0 : SDL_INIT_VIDEO) | (use_pads ? SDL_INIT_GAMEPAD : 0))) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1;
    }
    if (use_pads) pads_refresh();
    if (!headless && pvr_host_open("DMS — desktop preview", 1)) { SDL_Quit(); return 1; }
    puts("DMS preview: WASD move, arrows look, Space jump, Shift sprint, R reset, Escape exit; or a gamepad");
    int result = dms_game_main(argc, argv);
    pvr_host_close();
    SDL_Quit();
    return result;
}
