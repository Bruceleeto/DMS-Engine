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
#include "dms/dc_particles.h"
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  12.0f

#define FLOOR_Y     0.0f
#define MOST        600         /* alive at once, as in the PowerVR demo */
#define BURST       200         /* the A button */

/* ---- App state ---- */
static DCCamera camera;
static DMSModel* floor_model = NULL;
static DCParticles* fire = NULL;

static float distance = 26.0f;

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 512 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    camera.yaw = 0.5f;
    camera.pitch = 0.35f;

    floor_model = dc_model_load(ASSETS "world/floor.dms");

    /* A fountain of fire: thrown up from a small spot on the floor, pulled
     * back down, bouncing when it lands. Grey-pink as it is born, red at its
     * height, going out as it dies. */
    fire = dc_particles_create(MOST, &(DCParticleOpts){
        .pos          = shz_vec3_init(0.0f, FLOOR_Y + 0.5f, 0.0f),
        .spread       = shz_vec3_init(0.5f, 0.0f, 0.5f),
        .speed        = shz_vec3_init(0.0f, 14.0f, 0.0f),
        .speed_spread = shz_vec3_init(2.5f, 6.0f, 2.5f),
        .gravity      = 9.8f,
        .life         = 4.0f, .life_spread = 0.5f,
        .size         = 1.2f, .size_spread = 0.8f,
        .start        = 0x998080,
        .middle       = 0xFF2000,
        .end          = 0x000000,
        .rate         = (float)MOST / 4.0f,     /* enough to keep MOST alive */
        .floor_y      = FLOOR_Y,
        .bounce       = true,
    });

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    shz_vec3_t origin = shz_vec3_init(0.0f, 0.0f, 0.0f);
    shz_vec3_t middle = shz_vec3_init(0.0f, 6.0f, 0.0f);

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Stick turns the camera around the fountain, triggers zoom, A puffs */
        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 10.0f) distance = 10.0f;
            if (distance > 45.0f) distance = 45.0f;
            if (dc_input_pressed(inp, CONT_A)) dc_particles_burst(fire, BURST);
        }
        dc_camera_orbit(&camera, middle, distance, inp, LOOK_SPEED, dt);
        if (camera.pitch < 0.02f) camera.pitch = 0.02f;   /* stay above the floor */
        dc_camera_update(&camera);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_draw(floor_model, origin);
        dc_particles_draw(fire);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_particles_free(fire);
    dc_model_free(floor_model);
    dc_shutdown();

    return 0;
}
