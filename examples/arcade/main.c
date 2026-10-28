#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdlib.h>
#include <string.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_audio.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* An arcade cabinet with a game playing on it. The screen is a flipbook: its
 * material wears a sheet of 64 frames, 8 by 8, and dc_model_flipbook() shows
 * the next one 12 times a second. The frames are 5.3 seconds of a video, and
 * the same 5.3 seconds of its sound loop with them.
 *
 * The cabinet and the camera are scenes/main.json; the camera turns about a
 * point 11 up the cabinet, from where the scene's camera is. */

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  30.0f
#define FRAMES_ACROSS 8
#define FRAMES_DOWN   8
#define FRAME_COUNT   64
#define FRAME_RATE    12.0f

static DCCamera  camera;
static DCScene*  scene;
static float     distance;

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    DCSceneObject* cabinet = dc_scene_find(scene, "Cabinet");
    if (!cabinet) { printf("ARCADE: the scene has no object called Cabinet\n"); return 1; }
    dc_model_flipbook(cabinet->m, "screen", FRAMES_ACROSS, FRAMES_DOWN, FRAME_COUNT, FRAME_RATE);

    if (dc_audio_init()) dc_music_play(ASSETS "music/mario.dca", true);

    /* The cabinet is 23 tall, its screen facing +z */
    shz_vec3_t look = shz_vec3_add(cabinet->pos, shz_vec3_init(0.0f, 11.0f, 0.0f));
    dc_scene_camera(scene, &camera);
    float vx = camera.pos.x - look.x, vy = camera.pos.y - look.y, vz = camera.pos.z - look.z;
    distance = sqrtf(vx * vx + vy * vy + vz * vz);
    camera.pitch = asinf(vy / distance);
    camera.yaw   = atan2f(-vx, -vz);

    printf("ARCADE: stick turns, triggers zoom, Start exits\n");

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 12.0f) distance = 12.0f;
            if (distance > 80.0f) distance = 80.0f;
        }
        dc_camera_orbit(&camera, look, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        dc_set_camera(&camera);
        dc_scene_draw(scene);

        dc_frame_end();
    }

    dc_music_stop();
    dc_audio_shutdown();
    dc_scene_free(scene);
    dc_shutdown();
    return 0;
}
