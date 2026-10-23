#include "game.h"
#include "scene.h"
#include "sound.h"
#include "save.h"
#include <stdio.h>

/* BotBoy: a 3D platformer, ported from the N64 to DMS. Boots into the
 * splash screens, then the logo, then the menu. */

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    save_init();                        /* the volumes come from the VMU */
    sound_init();
    dc_draw2d_init();
    dc_set_clear_color(0xFF000000);

    printf("BOTBOY: A, B or Start skips a splash\n");

    scene_start(SCENE_SPLASH);

    float since = 0.0f;   /* TEMP: fps to the console every 5 s */
    for (;;) {
        dc_frame_begin();
        scene_update(dc_delta_time());
        scene_draw();
        dc_frame_end();
        since += dc_delta_time();
        if (since >= 5.0f) {
            since = 0.0f;
            printf("fps %.1f  frame %.2f ms\n", dc_fps(), dc_frame_stats()->frame_ms);
        }
    }
    return 0;
}
