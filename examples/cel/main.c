#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* A light going around a cel shaded model. .bands on the light is all it
 * takes.
 *
 * The model, the camera's start and the light (its colour, reach, ambient,
 * bands, and where it starts: the code takes it around the model at that
 * height and that far out) are scenes/main.json. */

#define LOOK_SPEED   1.5f
#define ZOOM_SPEED   40.0f
#define ZOOM_MIN     30.0f
#define ZOOM_MAX     150.0f
#define LOOK_Y       23.0f   /* half the model's height */

#define LIGHT_SPEED  0.8f    /* radians a second */

static const int bands_steps[] = { 3, 4, 0, 2 };   /* A steps through them. 0 is smooth */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();

    DCScene* scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    const DCSceneObject* model = dc_scene_find(scene, "Model");
    if (!model || !scene->light_count) { printf("CEL: the scene needs an object called Model and a light\n"); return 1; }

    printf("CEL: A bands, B stops the light, triggers zoom, stick turns, Start exits\n");

    /* The camera turns about the model from where the scene's is */
    DCCamera camera;
    dc_scene_camera(scene, &camera);
    shz_vec3_t look = shz_vec3_add(model->pos, shz_vec3_init(0.0f, LOOK_Y, 0.0f));
    float vx = camera.pos.x - look.x, vy = camera.pos.y - look.y, vz = camera.pos.z - look.z;
    float distance = sqrtf(vx * vx + vy * vy + vz * vz);
    camera.pitch = asinf(vy / distance);
    camera.yaw   = atan2f(-vx, -vz);

    /* The light goes around the model's middle at its height and distance */
    DCLight light = scene->lights[0].light;
    float lx = light.pos.x - model->pos.x, lz = light.pos.z - model->pos.z;
    float reach = sqrtf(lx * lx + lz * lz);
    float around = atan2f(lx, lz);
    bool moving = true;
    int step = 0;
    while (step < 3 && bands_steps[step] != light.bands) step++;

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
        light.pos   = shz_vec3_init(model->pos.x + reach * sc.sin, light.pos.y, model->pos.z + reach * sc.cos);
        light.bands = bands;
        dc_set_light(&light);

        dc_set_camera(&camera);
        dc_scene_draw(scene);

        char line[32];
        if (bands) snprintf(line, sizeof(line), "%d bands", bands);
        else       snprintf(line, sizeof(line), "smooth");
        dc_draw_text(line, 16, 440, 16, DC_COLOR_WHITE);

        dc_frame_end();
    }

    dc_set_light(NULL);
    dc_scene_free(scene);
    dc_shutdown();
    return 0;
}
