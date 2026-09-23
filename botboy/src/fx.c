/* The small stuff that sells a hit: puffs of dust, sparks, the electricity
 * round a spin, the trail a pulse leaves, a slime's splash and the mark it
 * leaves on the floor, and the flashes over the whole screen. The N64 kept
 * one pool of particles it moved itself; here each kind is a DMS particle
 * system that moves and draws its own. Speeds were per frame at 30 fps and
 * are per second here. */
#include "level.h"
#include "dms/dc_particles.h"
#include <string.h>
#include <math.h>

#define F 30.0f

static DCParticles *dust, *sparks, *electric, *trail, *splash, *lavaSplash;
static uint32_t flashRgb;
static float    flashTime, flashLen, flashPeak;

/* The N64's 8x8 shadow sprite: the blob under him in the air and the mark a
 * slime leaves, both laid flat on the floor with dc_draw_decal */
static DCImage* shadowImg;

#define MARK_LIFE 3.3f
#define MARKS     16
typedef struct { shz_vec3_t pos, normal; float size, age; bool lava, on; } Mark;
static Mark marks[MARKS];
static int  markNext;

void fx_init(void) {
    /* Big, slow, floaty puffs: born a little either side, drifting up */
    dust = dc_particles_create(48, &(DCParticleOpts){
        .spread = shz_vec3_init(10.0f, 0.0f, 10.0f), .speed = shz_vec3_init(0.0f, 1.05f * F, 0.0f),
        .speed_spread = shz_vec3_init(0.55f * F, 0.25f * F, 0.55f * F),
        .life = 0.32f, .life_spread = 0.08f, .size = 11.0f, .size_spread = 3.0f, .grow = 1.4f,   /* the N64's size 4-7 was a half width */
        .start = 0x9C8C78, .end = 0x9C8C78, .smoke = true });
    /* A starburst of yellow going orange, most of it up and out */
    sparks = dc_particles_create(96, &(DCParticleOpts){
        .spread = shz_vec3_init(1.0f, 1.0f, 1.0f), .speed = shz_vec3_init(0.0f, 3.5f * F, 0.0f),
        .speed_spread = shz_vec3_init(5.0f * F, 1.5f * F, 5.0f * F), .gravity = 0.25f * F * F,
        .life = 0.55f, .life_spread = 0.12f, .size = 2.5f, .size_spread = 1.0f,
        .start = 0xFFF0A0, .middle = 0xFFA030, .end = 0x000000 });
    /* Short, jittery, cyan and white, in a ring round his chest */
    electric = dc_particles_create(64, &(DCParticleOpts){
        .spread = shz_vec3_init(13.0f, 10.0f, 13.0f), .speed = shz_vec3_init(0.0f, 2.0f * F, 0.0f),
        .speed_spread = shz_vec3_init(4.0f * F, 1.0f * F, 4.0f * F),
        .life = 0.2f, .life_spread = 0.05f, .size = 2.0f, .size_spread = 0.6f,
        .start = 0xE0FFFF, .middle = 0x80E0FF, .end = 0x000000 });
    /* The wisps a pulse leaves behind it */
    trail = dc_particles_create(64, &(DCParticleOpts){
        .spread = shz_vec3_init(1.0f, 1.0f, 1.0f), .speed_spread = shz_vec3_init(0.5f * F, 0.5f * F, 0.5f * F),
        .life = 0.4f, .life_spread = 0.1f, .size = 2.5f, .size_spread = 0.5f,
        .start = 0xA080FF, .end = 0x000000 });
    /* Drops of slime, dark or glowing */
    DCParticleOpts drops = {
        .spread = shz_vec3_init(5.0f, 2.0f, 5.0f), .speed = shz_vec3_init(0.0f, 3.0f * F, 0.0f),
        .speed_spread = shz_vec3_init(3.0f * F, 2.0f * F, 3.0f * F), .gravity = 0.6f * F * F,
        .life = 0.5f, .life_spread = 0.15f, .size = 3.0f, .size_spread = 1.0f,
        .start = 0x1E1428, .end = 0x1E1428, .smoke = true };
    splash = dc_particles_create(48, &drops);
    drops.start = 0xFF7020; drops.middle = 0xFF4010; drops.end = 0x000000; drops.smoke = false;
    lavaSplash = dc_particles_create(48, &drops);
    shadowImg = dc_image_load(ASSETS "game/shadow.dt");
    memset(marks, 0, sizeof(marks));
    markNext = 0;
    flashTime = 0.0f;
}

void fx_free(void) {
    dc_particles_free(dust); dc_particles_free(sparks); dc_particles_free(electric); dc_particles_free(trail);
    dc_particles_free(splash); dc_particles_free(lavaSplash);
    dust = sparks = electric = trail = splash = lavaSplash = NULL;
    if (shadowImg) dc_image_free(shadowImg);
    shadowImg = NULL;
}

static void burst(DCParticles* p, shz_vec3_t at, int n) {
    if (!p || n <= 0) return;
    dc_particles_move(p, at);
    dc_particles_burst(p, n);
}

void fx_dust(shz_vec3_t at, int n)     { burst(dust, shz_vec3_init(at.x, at.y + 3.0f, at.z), n); }
void fx_sparks(shz_vec3_t at, int n)   { burst(sparks, at, n); }
void fx_electric(shz_vec3_t at, int n) { burst(electric, shz_vec3_init(at.x, at.y + 10.0f, at.z), n); }
void fx_trail(shz_vec3_t at, int n)    { burst(trail, at, n); }
void fx_splash(shz_vec3_t at, int n, bool lava) { burst(lava ? lavaSplash : splash, at, n); }

/* A slime's mark: on the floor under it, lying along the slope, for 3.3 s.
 * The oldest one goes when all 16 are in use */
void fx_decal(shz_vec3_t at, float scale, bool lava) {
    ColGroundHit g = world_ground(shz_vec3_init(at.x, at.y + 5.0f, at.z), 60.0f);
    Mark* m = &marks[markNext];
    markNext = (markNext + 1) % MARKS;
    m->pos = g.hit ? shz_vec3_init(at.x, g.y + 1.0f, at.z) : shz_vec3_init(at.x, at.y + 1.0f, at.z);
    m->normal = g.hit ? g.normal : shz_vec3_init(0.0f, 1.0f, 0.0f);
    m->size = 20.0f * scale;
    m->age = 0.0f;
    m->lava = lava;
    m->on = true;
}

/* The blob under him in the air: on the floor found below, fainter and
 * smaller the higher he is (gone at 150, 40% across by 100), as the N64 */
void fx_blob(shz_vec3_t at, float floorY, shz_vec3_t normal) {
    float h = at.y - floorY;
    float alpha = 1.0f - h / 150.0f;
    if (alpha <= 0.0f) return;
    float size = 12.0f * fmaxf(0.4f, 1.0f - h / 100.0f);
    dc_draw_decal(shadowImg, &(DCDecalOpts){ .pos = shz_vec3_init(at.x, floorY + 1.0f, at.z), .normal = normal,
                  .size = size, .alpha = alpha * 180.0f / 255.0f, .tint = 0x000000 });
}

/* The whole screen tinted for a moment, strongest at once and fading */
void fx_flash(uint32_t rgb, float seconds, float alpha) {
    flashRgb = rgb & 0xFFFFFF; flashLen = flashTime = seconds; flashPeak = alpha;
}

void fx_update(float dt) {
    if (flashTime > 0.0f) flashTime -= dt;
    for (int i = 0; i < MARKS; i++)
        if (marks[i].on && (marks[i].age += dt) >= MARK_LIFE) marks[i].on = false;
}

/* After the level and everything in it */
void fx_draw(void) {
    dc_particles_draw(dust);
    dc_particles_draw(sparks);
    dc_particles_draw(electric);
    dc_particles_draw(trail);
    dc_particles_draw(splash);
    dc_particles_draw(lavaSplash);
    for (int i = 0; i < MARKS; i++) {
        const Mark* m = &marks[i];
        if (!m->on) continue;
        float fade = 1.0f - m->age / MARK_LIFE;
        dc_draw_decal(shadowImg, &(DCDecalOpts){ .pos = m->pos, .normal = m->normal, .size = m->size,
                      .alpha = fade * (m->lava ? 220.0f : 200.0f) / 255.0f, .tint = m->lava ? 0xFF5014 : 0x141414 });
    }
}

/* With the 2D things, before the fade */
void fx_draw_flash(void) {
    if (flashTime <= 0.0f || flashLen <= 0.0f) return;
    float a = flashPeak * flashTime / flashLen;
    ui_rect(0, 0, VIEW_W, VIEW_H, ((uint32_t)(a * 255.0f) << 24) | flashRgb);
}
