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
#include "dms/dc_scene.h"

/* Where assets are read from: /pc/ over dcload; on a disc the engine finds them at /cd/ */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* scenes/main.json places the scanner model and where the camera starts; the
 * code orbits the camera around the dinosaur and plays the animation. */

#define LOOK_SPEED  1.5f
#define ZOOM_SPEED  80.0f

/* ---- App state ---- */
static DCCamera camera;
static DCScene*  scene = NULL;
static DMSModel* model = NULL;

static float distance = 260.0f;
static bool  playing = true;
static bool  ring_hidden = false;
static bool  vol_on = true;

/* What dc_model_volume() settled on, so Y can put it back without naming a
 * mesh twice. Mesh number plus one. */
static uint32_t vol_shape_was, vol_on_was;

/* How much skin is left over the skeleton inside the window: a quarter, so the
 * body still reads as a body while the bones show through it. */
#define INSIDE_ALPHA  0x40

/* ========== MAIN ========== */

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    if (!inp) return false;
    return (inp->buttons & CONT_START) != 0;
}

int main(int argc, char* argv[]) {
    /* .volumes: dc_model_volume() needs tile bins in the translucent modifier
     * list, and they cost texture RAM, so they are off by default. */
    dc_init((DCInitParams){ .vram_size = 2 * 1024 * 1024, .volumes = true });
    dc_draw2d_init();
    dc_debug_init();

    scene = dc_scene_play(ASSETS, "scenes/main.json");
    if (!scene) return 1;
    dc_scene_apply(scene);
    dc_scene_camera(scene, &camera);

    /* The whole thing is one glb: dinosaur, bones, scanner, stand and the
     * animation that walks the scanner along the body. */
    DCSceneObject* obj = dc_scene_find(scene, "Scanner");
    model = obj ? obj->m : NULL;
    if (!model) { printf("SCANNER: the scene has no Scanner model\n"); return 1; }

    /* Everything the file has, by the name Blender gave it */
    dc_model_materials(model);

    /* Wherever the scanner ring covers the dinosaur's skin, the skin shows the
     * bones picture instead. Both named by their Blender material. */
    dc_model_volume(model, "Scanner", "Dinosaur", "Bones");

    /* 'Scanner' is on two meshes: the ring (5) and a pair of room panels (3).
     * The name picks 3, whose slabs cover the screen. Until the panels get
     * their own material in Blender, say which one. */
    model->vol_shape = 5 + 1;

    vol_shape_was = model->vol_shape;
    vol_on_was    = model->vol_on;

    /* The ring sits inside the dinosaur for most of its turn, so solid it only
     * shows where it sticks out. Everything in scanner.glb exports OPAQUE. */
    dc_model_see_through(model, "Scanner", 128);

    dc_model_volume_inside(model, INSIDE_ALPHA, 0xC8E6FF);

    dc_model_set_anim(model, 0);

    printf("SCANNER: A pause, B ring, Y volume, triggers zoom\n");

    printf("VRAM free after loading: %luKB\n", (unsigned long)(pvr_mem_available() / 1024));

    /* the dinosaur, wherever the scene puts the model */
    shz_vec3_t middle = shz_vec3_add(obj->pos, shz_vec3_init(0.0f, -20.0f, 20.0f));

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        /* Stick turns the camera, triggers zoom, A holds the animation still */
        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < 60.0f) distance = 60.0f;
            if (distance > 500.0f) distance = 500.0f;
            if (dc_input_pressed(inp, CONT_A)) playing = !playing;

            /* The ring's own near wall covers every pixel its volume marks,
             * so hiding it is the only way to see what the volume did */
            if (dc_input_pressed(inp, CONT_B)) {
                ring_hidden = !ring_hidden;
                if (ring_hidden) model->meshes[5].material_flags |=  DMS_MAT_COLLISION_ONLY;
                else             model->meshes[5].material_flags &= ~DMS_MAT_COLLISION_ONLY;
                printf("SCANNER: ring %s\n", ring_hidden ? "hidden" : "drawn");
            }

            /* Y takes the volume away entirely, so the skin is plain */
            if (dc_input_pressed(inp, CONT_Y)) {
                vol_on = !vol_on;
                model->vol_shape = vol_on ? vol_shape_was : 0;
                model->vol_on    = vol_on ? vol_on_was    : 0;
                printf("SCANNER: volume %s\n", vol_on ? "on" : "off");
            }

        }
        dc_camera_orbit(&camera, middle, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        if (playing) dc_model_animate(model, dt);

        /* ---- Rendering ---- */
        dc_set_camera(&camera);
        dc_scene_draw(scene);
        dc_debug_stats();
        dc_frame_end();
    }

    dc_scene_free(scene);
    dc_shutdown();

    return 0;
}
