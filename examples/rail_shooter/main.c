#include <kos.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_corona.h"
#include "dms/dc_scene.h"

#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* The jet (Jet: where it starts and its size), its moonlight (the only thing
 * lit), the sky colour, fog and start camera are scenes/main.json. The city
 * is made as it flies, so it stays in code.
 *
 * Fixed pools: no allocation while flying. The world moves towards +Z,
 * keeping the ship at Z=0 and coordinates small for arbitrarily long runs. */
#define SECTIONS 16
#define SPACING 28.0f
#define SHOTS 24
#define SPEED 38.0f
#define SHIP_HALF 0.7f
#define CHASE_DISTANCE 15.0f
#define CHASE_HEIGHT 5.25f
#define TOWER_TYPES 6

typedef struct {
    float z, centre, height[4], width[4], target_x, target_y;
    unsigned char style[4];
    bool target;
} Section;
typedef struct { float x, y, z; bool active; } Shot;

static Section sections[SECTIONS];
static Shot shots[SHOTS];
static DMSModel* cube;
static DCScene* scene;
static DCSceneObject* jet;
static DMSModel* towers[TOWER_TYPES];
static DMSModel* street;
static shz_vec3_t jet_centre, ship_start;
static DCCamera camera;
static uint32_t seed, rng;
static float ship_x, ship_y, distance, cooldown, hurt;
static float velocity_x, velocity_y, bank, pitch, camera_x, camera_y;
static float flight_speed, camera_back;
static uint32_t polygons_drawn;
static int score, shield;
static bool bloom_enabled = true;
static const DCBloom city_bloom = { .strength = 0.40f, .spread = 1.1f, .size = 128 };

static float clamp(float x, float lo, float hi) {
    return fmaxf(lo, fminf(hi, x));
}

/* Sample the rail at a distance along the course. Sections keep their own
 * centre, so turns approach naturally instead of the whole city swaying. */
static float rail_x(float at) {
    return 12.0f * sinf(at * 0.006f) + 5.0f * sinf(at * 0.013f);
}

static float follow(float current, float target, float response, float dt) {
    return current + (target - current) * (1.0f - expf(-response * dt));
}

static float world_ship_x(void) { return rail_x(distance) + ship_x; }

static float random_range(float lo, float hi) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return lo + (hi - lo) * (float)(rng & 0xffff) / 65535.0f;
}

static void generate(Section* s, float z) {
    s->z = z;
    s->centre = rail_x(distance - z);
    for (int i = 0; i < 4; ++i) {
        s->height[i] = random_range(5, i < 2 ? 23 : 40);
        s->width[i] = random_range(5, 9);
        s->style[i] = (unsigned char)random_range(0, TOWER_TYPES - 0.001f);
    }
    s->target_x = s->centre + random_range(-8, 8);
    s->target_y = random_range(5, 42);
    s->target = true;
}

static void reset(void) {
    rng = seed ? seed : 1;
    memset(shots, 0, sizeof(shots));
    ship_x = ship_start.x; ship_y = ship_start.y;
    distance = cooldown = hurt = 0;
    velocity_x = velocity_y = bank = pitch = camera_x = 0;
    camera_y = ship_y + CHASE_HEIGHT;
    flight_speed = SPEED; camera_back = CHASE_DISTANCE;
    score = 0; shield = 5;
    for (int i = 0; i < SECTIONS; ++i)
        generate(&sections[i], 24 - i * SPACING);
    /* A short clear stretch to settle into the controls. */
    sections[0].target = sections[1].target = sections[2].target = false;
}

static void fire(void) {
    for (int i = 0; i < SHOTS; ++i) {
        if (shots[i].active) continue;
        shots[i] = (Shot){world_ship_x(), ship_y, -1.5f, true};
        break;
    }
}

static void update(const DCInput* input, float dt) {
    if (dc_input_pressed(input, CONT_B)) {
        bloom_enabled = !bloom_enabled;
        dc_set_bloom(bloom_enabled ? &city_bloom : NULL);
    }
    if (dc_input_pressed(input, CONT_Y)) { reset(); return; }
    if (dc_input_pressed(input, CONT_X)) { seed += 0x9e3779b9u; reset(); return; }
    if (!shield) return;

    float x = input->stick_x;
    float y = -input->stick_y;
    if (input->buttons & CONT_DPAD_LEFT) x -= 1;
    if (input->buttons & CONT_DPAD_RIGHT) x += 1;
    if (input->buttons & CONT_DPAD_UP) y += 1;
    if (input->buttons & CONT_DPAD_DOWN) y -= 1;
    velocity_x = follow(velocity_x, clamp(x, -1, 1) * 22, 9, dt);
    velocity_y = follow(velocity_y, clamp(y, -1, 1) * 17, 9, dt);
    ship_x = clamp(ship_x + velocity_x * dt, -12, 12);
    ship_y = clamp(ship_y + velocity_y * dt, 3, 48);
    if ((ship_x <= -12 && velocity_x < 0) || (ship_x >= 12 && velocity_x > 0)) velocity_x = 0;
    if ((ship_y <= 3 && velocity_y < 0) || (ship_y >= 48 && velocity_y > 0)) velocity_y = 0;
    bank = follow(bank, -velocity_x * 0.025f, 7, dt);
    /* Forward is -Z: positive X rotation lifts the nose towards +Y. */
    pitch = follow(pitch, velocity_y * 0.012f, 7, dt);
    flight_speed = follow(flight_speed,
        SPEED + 48 * input->rtrig - 22 * input->ltrig, 3, dt);
    distance += flight_speed * dt;
    camera_x = follow(camera_x, rail_x(distance) + ship_x * 0.65f, 3, dt);
    camera_y = follow(camera_y, ship_y + CHASE_HEIGHT, 4, dt);
    camera_back = follow(camera_back, CHASE_DISTANCE + (flight_speed - SPEED) * 0.08f, 3, dt);
    camera.fov = 60 + (flight_speed - SPEED) * 0.18f;
    hurt = fmaxf(0, hurt - dt);
    cooldown -= dt;
    if (dc_input_held(input, CONT_A) && cooldown <= 0) {
        fire(); cooldown = 0.12f;
    }

    for (int i = 0; i < SECTIONS; ++i) sections[i].z += flight_speed * dt;
    for (int i = 0; i < SECTIONS; ++i) {
        if (sections[i].z <= 52) continue;
        float back = sections[0].z;
        for (int j = 1; j < SECTIONS; ++j) back = fminf(back, sections[j].z);
        generate(&sections[i], back - SPACING);
    }

    for (int i = 0; i < SHOTS; ++i) {
        Shot* b = &shots[i];
        if (!b->active) continue;
        float previous = b->z;
        b->z -= 140 * dt;
        /* Sweep through the relative motion so a fast shot cannot skip a target. */
        for (int j = 0; j < SECTIONS; ++j) {
            Section* s = &sections[j];
            if (!s->target || fabsf(b->x - s->target_x) > 1.5f ||
                fabsf(b->y - s->target_y) > 1.5f) continue;
            if (previous >= s->z - flight_speed * dt - 1.5f && b->z <= s->z + 1.5f) {
                s->target = false; b->active = false; score += 100; break;
            }
        }
        if (b->z < -SECTIONS * SPACING) b->active = false;
    }
    for (int i = 0; i < SECTIONS; ++i) {
        Section* s = &sections[i];
        if (!s->target || hurt > 0) continue;
        if (fabsf(s->z) < 1.25f + SHIP_HALF &&
            fabsf(s->target_x - world_ship_x()) < 1.25f + SHIP_HALF &&
            fabsf(s->target_y - ship_y) < 1.25f + SHIP_HALF) {
            s->target = false; --shield; hurt = 1.2f;
        }
    }
}

static void box(float x, float y, float z, float w, float h, float d, uint32_t colour) {
    dc_draw_ex(cube, &(DCDrawOpts){
        .pos = { .x = x, .y = y, .z = z },
        .stretch = { .x = w, .y = h, .z = d }, .tint = colour
    });
}

/* Untextured background, like the demos' background image, but with a
 * gradient that follows camera pitch. Its depth stays behind the whole city. */
static void sky_quad(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    pvr_vertex_t v = { .z = 0.00001f };
    v.flags = PVR_CMD_VERTEX; v.x = x; v.y = y; v.argb = top; pvr_prim(&v, sizeof(v));
    v.x = x+w; pvr_prim(&v, sizeof(v));
    v.x = x; v.y = y+h; v.argb = bottom; pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL; v.x = x+w; pvr_prim(&v, sizeof(v));
}

static void draw_sky(void* user) {
    (void)user;
    pvr_poly_cxt_t context;
    pvr_poly_hdr_t header;
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.fog_type = PVR_FOG_DISABLE;
    context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&header, &context);
    pvr_prim(&header, sizeof(header));
    float horizon = clamp(240 - camera.pitch * 420, 90, 330);
    sky_quad(0, 0, SCR_W, horizon*0.65f, 0xFF040916, 0xFF10243C);
    sky_quad(0, horizon*0.65f, SCR_W, horizon*0.35f, 0xFF10243C, 0xFF303B54);
    sky_quad(0, horizon, SCR_W, SCR_H-horizon, 0xFF303B54, 0xFF102035);
    for (int i = 0; i < 36; ++i) {
        float x = fmodf(i*137.0f + 900 - (camera.yaw-SHZ_F_PI)*310, SCR_W);
        float y = 12 + (i*47 % 150) - camera.pitch*100;
        if (y < 4 || y > horizon*0.7f) continue;
        uint32_t colour = i%4 ? 0xFF52677F : 0xFFB2C6D5;
        sky_quad(x, y, i%4 ? 1 : 1.5f, 1, colour, colour);
    }
}

static void draw(void) {
    /* Partial follow leaves room to move across the screen. Lag lets the
     * scenery slide past during a dodge; looking ahead reveals rail turns. */
    camera.pos = shz_vec3_init(camera_x, camera_y, camera_back);
    dc_camera_look_at(&camera, shz_vec3_init(
        rail_x(distance + 65) + ship_x * 0.25f,
        ship_y - 3 + velocity_y * 0.65f, -65));
    dc_camera_update(&camera);
    dc_set_camera(&camera);
    dc_draw_call(PVR_LIST_OP_POLY, draw_sky, NULL);
    for (int i = 0; i < SECTIONS; ++i) {
        const Section* s = &sections[i];
        /* Fog hides the recycle boundary. Also leaves room in the 256-entry
         * draw queue for bloom's copy of the visible scene and live shots. */
        if (s->z < -330 || s->z > camera_back + 14) continue;
        dc_draw(street, shz_vec3_init(s->centre, 0, s->z));
        for (int j = 0; j < 4; ++j) {
            float x = s->centre + (j & 1 ? 1 : -1) * (j < 2 ? 20 : 36);
            dc_draw_ex(towers[s->style[j]], &(DCDrawOpts){
                .pos = { .x = x, .y = 0, .z = s->z },
                .stretch = { .x = s->width[j], .y = s->height[j], .z = 13 }
            });
        }
        if (s->target) box(s->target_x, s->target_y, s->z, 2.5f, 2.5f, 2.5f, 0xFF7040);
        if (s->z > -100 && s->z < 10) {
            for (int side = 0; side < 2; ++side)
                dc_corona(10+i*2+side,
                    shz_vec3_init(s->centre+(side ? 12.8f : -12.8f), 4.4f, s->z-8),
                    0xFFCC85, 0.65f, 0);
        }
    }
    for (int i = 0; i < SHOTS; ++i)
        if (shots[i].active) box(shots[i].x, shots[i].y, shots[i].z, 0.18f, 0.18f, 2, 0xFFFF80);
    /* Build a rigid rotation through the same SH4 matrix API as the engine.
     * Start at identity so neutral input cannot introduce pitch or shear. */
    shz_mat4x4_t attitude __attribute__((aligned(32)));
    shz_xmtrx_init_identity();
    shz_xmtrx_rotate_z(bank);
    shz_xmtrx_rotate_x(pitch);
    shz_xmtrx_store_4x4(&attitude);
    float rotation[9];
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            rotation[column * 3 + row] = attitude.elem2D[column][row];
    /* The imported GLB already faces -Z with canopy and tail fins up.
     * Apply only the flight rotation; no asset-axis correction is needed. */
    /* Keep the model centred on the flight position as it banks. */
    shz_vec3_t centre = jet_centre;
    float jet_scale = jet->scale.x;
    jet->pos = shz_vec3_init(
        world_ship_x() - jet_scale * (rotation[0]*centre.x + rotation[3]*centre.y + rotation[6]*centre.z),
        ship_y - jet_scale * (rotation[1]*centre.x + rotation[4]*centre.y + rotation[7]*centre.z),
        -jet_scale * (rotation[2]*centre.x + rotation[5]*centre.y + rotation[8]*centre.z));
    jet->rot = dc_scene_rot_degrees(rotation);
    jet->tint = hurt > 0 ? 0xFF7090 : 0xFFFFFF;
    /* Only the jet is lit, by the scene's lights */
    DCLight lights[DC_MAX_LIGHTS];
    for (int i = 0; i < scene->light_count; ++i) lights[i] = scene->lights[i].light;
    if (scene->light_count) dc_set_lights(lights, scene->light_count);
    dc_scene_draw(scene);
    dc_set_light(NULL);
    for (int side = 0; side < 2; ++side) {
        float x = (side ? 0.16f : -0.16f) - centre.x;
        float y = -0.14f - centre.y, z = 1.03f - centre.z;
        shz_vec3_t nozzle = shz_vec3_init(
            world_ship_x()+jet_scale*(rotation[0]*x+rotation[3]*y+rotation[6]*z),
            ship_y+jet_scale*(rotation[1]*x+rotation[4]*y+rotation[7]*z),
            jet_scale*(rotation[2]*x+rotation[5]*y+rotation[8]*z));
        float boost = clamp((flight_speed-SPEED)/48, 0, 1);
        dc_corona(1+side, nozzle, boost > 0.2f ? 0xFFAE63 : 0x61CFFF,
                  0.65f + boost*0.7f, 0);
    }
    /* World-space aim frame follows the shot line despite camera movement. */
    float aim_x = world_ship_x();
    for (int i = 0; i < 3; ++i) {
        float z = -25 - i * 22;
        box(aim_x - 0.8f, ship_y, z, 0.08f, 1.6f, 0.08f, 0x60FFB0);
        box(aim_x + 0.8f, ship_y, z, 0.08f, 1.6f, 0.08f, 0x60FFB0);
        box(aim_x, ship_y - 0.8f, z, 1.6f, 0.08f, 0.08f, 0x60FFB0);
        box(aim_x, ship_y + 0.8f, z, 1.6f, 0.08f, 0.08f, 0x60FFB0);
    }

    char text[96];
    dc_draw_rect(16, 12, 306, 77, 0x9007111F);
    dc_draw_rect(16, 12, 3, 77, 0xFF52BDCC);
    dc_draw_rect(508, 12, 116, 32, 0x9007111F);
    snprintf(text, sizeof(text), "FPS %.1f", dc_fps());
    dc_draw_text(text, (int)SCR_W - 24 - dc_text_width(text, 20), 20, 20, DC_COLOR_WHITE);
    snprintf(text, sizeof(text), "SCORE %06d   DIST %dm", score, (int)distance);
    dc_draw_text(text, 24, 20, 20, DC_COLOR_WHITE);
    snprintf(text, sizeof(text), "NIGHT RUN  /  SECTOR %02d", 1+(int)distance/500);
    dc_draw_text(text, 24, 45, 14, 0xFF7EA4BA);
    snprintf(text, sizeof(text), "POLYS %lu", (unsigned long)polygons_drawn);
    dc_draw_text(text, 24, 68, 16, DC_COLOR_WHITE);
    dc_draw_rect(16, 386, 180, 58, 0xB007111F);
    dc_draw_text("SHIELD", 24, 394, 14, 0xFF7EA4BA);
    for (int i = 0; i < 5; ++i)
        dc_draw_rect(24+i*32, 416, 26, 8, i < shield ? 0xFF52D6B6 : 0xFF243447);
    dc_draw_rect(448, 386, 176, 58, 0xB007111F);
    snprintf(text, sizeof(text), "SPD %02d  ALT %02d", (int)flight_speed, (int)ship_y);
    dc_draw_text(text, 458, 394, 14, 0xFF91CCD8);
    dc_draw_rect(458, 416, 154, 5, 0xFF243447);
    dc_draw_rect(458, 416, 154*clamp(flight_speed/86, 0, 1), 5, 0xFFF0BA75);
    dc_draw_text("A FIRE  L/R SPEED  B BLOOM  Y RETRY  X NEW  START EXIT",
                 24, 455, 12, 0xFF7B91A4);
    if (!shield) {
        dc_draw_rect(90, 185, 460, 90, 0xD0101828);
        dc_draw_text("SHIP LOST", 225, 198, 24, DC_COLOR_ORANGE);
        dc_draw_text("Y RETRY / X NEW COURSE", 152, 239, 16, DC_COLOR_WHITE);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    /* Double-buffered vertex storage, with room for the visual effects. */
    dc_init((DCInitParams){ .vram_size = 1700 * 1024 });
    dc_draw2d_init();
    cube = dc_model_cube();
    if (!cube) { dc_shutdown(); return 1; }
    scene = dc_scene_play(ASSETS, "scenes/main.json");
    jet = scene ? dc_scene_find(scene, "Jet") : NULL;
    if (!jet || !jet->m) {
        printf("Cannot load the scene's Jet; run make assets and mount pc/ with dc-tool -c pc.\n");
        dc_scene_free(scene);
        dc_model_free(cube);
        dc_shutdown();
        return 1;
    }
    /* The lights are put on for the jet alone (in draw) */
    dc_scene_apply(scene);
    dc_set_light(NULL);
    dc_scene_camera(scene, &camera);
    DCBounds bounds = dc_model_bounds(jet->m, NULL);
    jet_centre = shz_vec3_init((bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f, (bounds.min.z + bounds.max.z) * 0.5f);
    /* The ship flies where the scene puts the middle of the jet (x from the rail) */
    ship_start = shz_vec3_init(jet->pos.x + jet->scale.x * jet_centre.x,
                               jet->pos.y + jet->scale.x * jet_centre.y, 0);
    for (uint32_t i = 0; i < jet->m->mesh_count; ++i)
        jet->m->meshes[i].rim_color = 0x152C40;
    street = dc_model_load(ASSETS "city/street.dms");
    bool city_loaded = street != NULL;
    for (int i = 0; i < TOWER_TYPES; ++i) {
        char path[96];
        snprintf(path, sizeof(path), ASSETS "city/tower%d.dms", i);
        towers[i] = dc_model_load(path);
        if (!towers[i]) city_loaded = false;
    }
    if (!city_loaded) {
        printf("Cannot load city models; run make assets.\n");
        for (int i = 0; i < TOWER_TYPES; ++i) dc_model_free(towers[i]);
        dc_model_free(street);
        dc_scene_free(scene);
        dc_model_free(cube);
        dc_shutdown();
        return 1;
    }
    seed = (uint32_t)timer_us_gettime64();
    dc_set_bloom(&city_bloom);
    reset();
    for (;;) {
        dc_frame_begin();
        const DCInput* input = dc_input_get(0);
        if (dc_input_pressed(input, CONT_START)) { dc_frame_end(); break; }
        /* Cap stalls so collisions and controls do not jump after a pause. */
        update(input, fminf(dc_delta_time(), 1.0f / 30.0f));
        draw();
        dc_frame_end();
        /* Queue has now rendered: display this count on the next frame. */
        polygons_drawn = dc_model_get_stats()->tris_drawn;
    }
    dc_model_free(cube);
    dc_set_bloom(NULL);
    dc_scene_free(scene);
    dc_model_free(street);
    for (int i = 0; i < TOWER_TYPES; ++i) dc_model_free(towers[i]);
    dc_shutdown();
    return 0;
}
