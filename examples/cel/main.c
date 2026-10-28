#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A light going around a cel shaded model. .bands on the light is all it
 * takes. */

#define LOOK_SPEED   1.5f
#define ZOOM_SPEED   40.0f
#define ZOOM_MIN     30.0f
#define ZOOM_MAX     150.0f
#define LOOK_Y       23.0f   /* half the model's height */

#define LIGHT_SPEED  0.8f    /* radians a second */
#define LIGHT_REACH  40.0f   /* how far out it goes around */
#define LIGHT_HEIGHT 45.0f

static const int bands_steps[] = { 3, 4, 0, 2 };   /* A steps through them. 0 is smooth */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();
    dc_set_clear_color(0xFF202838);

    DCCamera camera;
    dc_camera_init(&camera);
    camera.yaw = 0.3f;
    camera.pitch = 0.15f;

    DMSModel* model = dc_model_load(ASSETS "cel/scene.dms");

    printf("CEL: A bands, B stops the light, triggers zoom, stick turns, Start exits\n");

    shz_vec3_t look = shz_vec3_init(0.0f, LOOK_Y, 0.0f);
    float distance = 75.0f;
    float around = 0.0f;
    bool moving = true;
    int step = 0;

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < ZOOM_MIN) distance = ZOOM_MIN;
            if (distance > ZOOM_MAX) distance = ZOOM_MAX;
            if (dc_input_pressed(inp, CONT_A)) step = (step + 1) % 4;
            if (dc_input_pressed(inp, CONT_B)) moving = !moving;
        }
        if (moving) around += LIGHT_SPEED * dt;
        dc_camera_orbit(&camera, look, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        shz_sincos_t sc = shz_sincosf(around);
        int bands = bands_steps[step];
        dc_set_light(&(DCLight){
            .pos     = shz_vec3_init(LIGHT_REACH * sc.sin, LIGHT_HEIGHT, LIGHT_REACH * sc.cos),
            .range   = 150.0f,
            .ambient = 0.4f,
            .bands   = bands,
        });

        dc_set_camera(&camera);
        dc_draw(model, shz_vec3_init(0.0f, 0.0f, 0.0f));

        char line[32];
        if (bands) snprintf(line, sizeof(line), "%d bands", bands);
        else       snprintf(line, sizeof(line), "smooth");
        dc_draw_text(line, 16, 440, 16, DC_COLOR_WHITE);

        dc_debug_stats();
        dc_frame_end();
    }

    dc_set_light(NULL);
    dc_model_free(model);
    dc_shutdown();
    return 0;
}
