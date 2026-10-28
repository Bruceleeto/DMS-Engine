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
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* scenes/main.json places the floor, the knot and where the camera starts; the
 * code turns the knot and draws it with its shadow, and moves the light. */

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  10.0f
#define LIGHT_SPEED 1.2f        /* radians a second */
#define TURN_SPEED  0.6f

#define LIGHT_REACH  9.0f       /* how far the light is from the knot */

/* ---- App state ---- */
static DCCamera camera;
static DCScene*  scene = NULL;
static DCSceneObject* knot = NULL;

static float distance = 22.0f;
static float light_around = 0.6f;   /* the light goes around the knot... */
static float light_up = 1.0f;       /* ...and up and down (radians above level) */

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 512 * 1024 });
    dc_draw2d_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);
    knot = dc_scene_find(scene, "Knot");
    DCSceneObject* floor_obj = dc_scene_find(scene, "Floor");
    if (!knot || !knot->m || !floor_obj) { printf("SHADY: the scene needs a Knot and a Floor\n"); return 1; }
    /* The knot needs a shadow, which dc_scene_draw doesn't give: the code
     * draws it, so the scene doesn't (the editor still shows it). */
    knot->hidden = true;
    const float start_turn = knot->rot.y;

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    shz_vec3_t knot_pos = knot->pos;
    float knot_yaw = 0.0f;

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Stick turns the camera around the knot, triggers zoom, D-pad moves the light */
        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 8.0f)  distance = 8.0f;
            if (distance > 35.0f) distance = 35.0f;
            if (dc_input_held(inp, CONT_DPAD_LEFT))  light_around -= LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_RIGHT)) light_around += LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_UP))    light_up += LIGHT_SPEED * dt;
            if (dc_input_held(inp, CONT_DPAD_DOWN))  light_up -= LIGHT_SPEED * dt;
            if (light_up < 0.7f) light_up = 0.7f;
            if (light_up > 1.5f) light_up = 1.5f;
        }
        dc_camera_orbit(&camera, knot_pos, distance, inp, LOOK_SPEED, dt);
        if (camera.pitch < 0.05f) camera.pitch = 0.05f;     /* stay above the floor */
        dc_camera_update(&camera);

        knot_yaw += TURN_SPEED * dt;

        shz_sincos_t around = shz_sincosf(light_around), up = shz_sincosf(light_up);
        DCShadow shadow = {
            .light = shz_vec3_init(knot_pos.x + LIGHT_REACH * up.cos * around.sin,
                                   knot_pos.y + LIGHT_REACH * up.sin,
                                   knot_pos.z + LIGHT_REACH * up.cos * around.cos),
            .floor_y = floor_obj->pos.y,
        };

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        float rot[9];
        knot->rot.y = start_turn - SHZ_RAD_TO_DEG(knot_yaw);
        dc_scene_object_rot(knot, rot);
        dc_draw_ex(knot->m, &(DCDrawOpts){ .pos = knot_pos, .scale = knot->scale.x, .rot = rot,
                                           .shadow = &shadow });
        dc_frame_end();
    }

    dc_scene_free(scene);
    dc_shutdown();

    return 0;
}
