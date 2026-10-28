#include "game.h"
#include "scene.h"
#include "sound.h"
#include "save.h"
#include <stdio.h>
#include <stdlib.h>

extern int game_level_id;

/* BotBoy: a 3D platformer, ported from the N64 to DMS. Boots into the
 * splash screens, then the logo, then the menu. */

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    save_init();                        /* the volumes come from the VMU */
    sound_init();
    dc_draw2d_init();
    dc_set_clear_color(0xFF000000);

    printf("BOTBOY: A, B or Start skips a splash\n");

    /* Play in the editor names the scene open there: a level's goes
     * straight into it (only on Linux; the Dreamcast has no environment) */
    const char* open = getenv("DMS_SCENE");
    int lv;
    if (open && sscanf(open, "scenes/level%d.json", &lv) == 1 && lv >= 1) {
        game_level_id = lv - 1;
        scene_start(SCENE_GAME);
    } else {
        scene_start(SCENE_SPLASH);
    }

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
