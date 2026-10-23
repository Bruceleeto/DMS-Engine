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
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* Animation indices, in the order they are in dragonfly.glb */
#define ANIM_FIRE 0
#define ANIM_IDLE 1

#define LOOK_SPEED 1.5f
#define ZOOM_SPEED 150.0f

/* The trail: how much of last frame's picture stays (0 to 1), and its size.
 * 256x256 takes 256KB of VRAM; 512 looks a little sharper but costs 4x to draw. */
#define TRAIL_KEEP 0.92f
#define TRAIL_SIZE 256

/* Shots, as in the Katana demo: X plays Fire once, three shots leave during
 * it. A shot is the flash flying off ahead (-z) and sinking; it is the trail
 * that draws it out into a ray. */
#define FIRE_LENGTH  1.667f     /* seconds, the Fire animation */
#define SHOT_SPEED   480.0f     /* units a second */
#define SHOT_SINK    60.0f
#define SHOT_LIFE    3.3f       /* seconds */
#define SHOT_MAX     16
static const float shot_times[3] = { 0.267f, 0.467f, 0.667f };   /* into Fire */

typedef struct {
    bool  active;
    float age;
    shz_vec3_t pos;     /* how far it has gone from where the flash sits in the model */
} Shot;

/* ---- App state ---- */
static DCCamera camera;
static DMSModel* dragonfly = NULL;
static DMSModel* flash = NULL;
static DCImage*  background = NULL;
static DCTarget* trail = NULL;
static Shot  shots[SHOT_MAX];
static float fire_time = -1.0f;     /* seconds into Fire, under 0 when not firing */

static void shot_create(void) {
    for (int i = 0; i < SHOT_MAX; i++) {
        if (shots[i].active) continue;
        shots[i].active = true;
        shots[i].age = 0.0f;
        shots[i].pos = shz_vec3_init(0.0f, 0.0f, 0.0f);
        return;
    }
}

static void shots_update(float dt) {
    for (int i = 0; i < SHOT_MAX; i++) {
        if (!shots[i].active) continue;
        shots[i].age += dt;
        shots[i].pos.z -= SHOT_SPEED * dt;
        shots[i].pos.y -= SHOT_SINK * dt;
        if (shots[i].age > SHOT_LIFE) shots[i].active = false;
    }
}

/* The flash is a flat picture, so it is turned to face the camera, about its
 * own middle (it does not sit at the model's origin) */
static void shots_draw(void) {
    if (!flash || !flash->mesh_count) return;
    shz_sincos_t y = shz_sincosf(camera.yaw), p = shz_sincosf(camera.pitch);
    float rot[9] = {
        y.cos,         0.0f,   -y.sin,              /* the flash's x: the camera's right */
        y.sin * p.sin, p.cos,  y.cos * p.sin,       /* its y: the camera's up */
        y.sin * p.cos, -p.sin, y.cos * p.cos,       /* its z: the way the camera looks */
    };
    const DMSMesh* m = &flash->meshes[0];
    float cx = m->bound_cx, cy = m->bound_cy, cz = m->bound_cz;
    shz_vec3_t turned = shz_vec3_init(rot[0] * cx + rot[3] * cy + rot[6] * cz,
                                      rot[1] * cx + rot[4] * cy + rot[7] * cz,
                                      rot[2] * cx + rot[5] * cy + rot[8] * cz);

    for (int i = 0; i < SHOT_MAX; i++) {
        if (!shots[i].active) continue;
        shz_vec3_t pos = shz_vec3_init(shots[i].pos.x + cx - turned.x,
                                       shots[i].pos.y + cy - turned.y,
                                       shots[i].pos.z + cz - turned.z);
        dc_draw_ex(flash, &(DCDrawOpts){ .pos = pos, .rot = rot, .add = true });
    }
}

/* The middle of the dragonfly (it is about 200 wide and 215 long) */
static const shz_vec3_t centre = { .x = 0.0f, .y = 0.0f, .z = 60.0f };
static float distance = 330.0f;

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    /* Vertex buffer: this scene uses under 200KB of it (drawn twice), and KOS
     * keeps two, so a small one leaves the VRAM for the trail */
    dc_init((DCInitParams){ .vram_size = 512 * 1024 });
    dc_draw2d_init();
    dc_debug_init();

    dc_camera_init(&camera);
    camera.yaw = F_PI * 0.75f;
    camera.pitch = 0.3f;

    dragonfly = dc_model_load(ASSETS "dragonfly/dragonfly.dms");
    flash = dc_model_load(ASSETS "flash/flash.dms");
    background = dc_image_load(ASSETS "background/background.dt");
    trail = dc_target_create(TRAIL_SIZE, TRAIL_SIZE);

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Stick turns the camera around the dragonfly, triggers zoom, X fires */
        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 150.0f) distance = 150.0f;
            if (distance > 800.0f) distance = 800.0f;
        }
        dc_camera_orbit(&camera, centre, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        if (fire_time < 0.0f && inp && dc_input_pressed(inp, CONT_X))
            fire_time = 0.0f;
        if (fire_time >= 0.0f) {
            float before = fire_time;
            fire_time += dt;
            for (int i = 0; i < 3; i++)
                if (before < shot_times[i] && fire_time >= shot_times[i]) shot_create();
            if (fire_time >= FIRE_LENGTH) fire_time = -1.0f;
        }
        shots_update(dt);

        if (dragonfly) {
            int anim = fire_time >= 0.0f ? ANIM_FIRE : ANIM_IDLE;
            if (dc_model_get_anim(dragonfly) != anim)
                dc_model_set_anim(dragonfly, anim);
            dc_model_animate(dragonfly, dt);
        }

        /* ---- Rendering ---- */
        shz_vec3_t origin = shz_vec3_init(0.0f, 0.0f, 0.0f);

        /* The trail: the dragonfly on black, then last frame's trail over it,
         * a little see-through, so what moves leaves a smear that fades */
        dc_set_target(trail);
        dc_set_camera(&camera);
        dc_draw(dragonfly, origin);
        shots_draw();
        dc_draw_target_ex(trail, &(DCTargetOpts){ .alpha = TRAIL_KEEP });

        /* The screen: the dragonfly, sharp, and the trail added over it */
        dc_set_target(NULL);
        dc_set_camera(&camera);
        dc_draw_background(background);
        dc_draw(dragonfly, origin);
        shots_draw();
        dc_draw_target_ex(trail, &(DCTargetOpts){ .add = true });
        dc_debug_stats();
        dc_frame_end();
    }

    dc_target_free(trail);
    dc_model_free(flash);
    dc_model_free(dragonfly);
    dc_image_free(background);
    dc_shutdown();

    return 0;
}
