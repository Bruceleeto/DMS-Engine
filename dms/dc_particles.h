#ifndef DC_PARTICLES_H
#define DC_PARTICLES_H

#include <stdbool.h>
#include <stdint.h>
#include <sh4zam/shz_sh4zam.h>

/* ================================================================
 * Particles
 *
 * A puff of flat pictures that always face the camera: fire, smoke, sparks,
 * dust, a fountain. Say once what they look like and how they move, then draw
 * them every frame; the engine moves them, colours them, turns them to the
 * camera and puts them in the right PVR list.
 *
 *     DCParticles* fire = dc_particles_create(600, &(DCParticleOpts){
 *         .pos    = fire_place,
 *         .speed  = { 0, 30, 0 }, .speed_spread = { 4, 15, 4 },
 *         .gravity = 9.8f,
 *         .life   = 8.0f, .size = 2.0f,
 *         .start  = 0x998080, .middle = 0xFF0000, .end = 0x000000,
 *         .rate   = 75.0f,
 *     });
 *
 *     while (playing) {
 *         dc_frame_begin();
 *         dc_set_camera(&camera);
 *         dc_draw(level, origin);
 *         dc_particles_draw(fire);      // moves them and draws them
 *         dc_frame_end();
 *     }
 *
 * They are added over what is behind them, so black is invisible and they
 * never darken the scene (.smoke draws them mixed in instead, for dark
 * smoke). They cost four vertices each and no CPU work per triangle.
 * ================================================================ */

typedef struct DCParticles DCParticles;

/* What they look like and how they move. Fields left out are zero, which
 * means "as it is": no spread, no gravity, no floor. */
typedef struct {
    shz_vec3_t pos;             /* where they are born */
    shz_vec3_t spread;          /* born up to this far either side of pos */
    shz_vec3_t speed;           /* which way they go, units a second */
    shz_vec3_t speed_spread;    /* and up to this much either side of that */
    float      gravity;         /* pulled down this fast, units a second a second */
    float      drag;            /* 0 to 1: how much speed is lost each second */
    float      life;            /* seconds each one lasts. 0 means 2 */
    float      life_spread;
    float      size;            /* how big across, in world units. 0 means 1 */
    float      size_spread;
    float      grow;            /* size by the end, as a multiple. 0 means 1 */
    float      spin;            /* radians a second they turn on the screen */
    uint32_t   start;           /* colour when born, 0xRRGGBB. 0 means white */
    uint32_t   middle;          /* halfway through. Left out: start */
    uint32_t   end;             /* as they die. Left out: black (they fade out) */
    float      rate;            /* born a second. 0 means only dc_particles_burst() */
    float      floor_y;         /* a floor to bounce off, with .bounce */
    bool       bounce;          /* off floor_y, at .bounce_keep of their speed */
    float      bounce_keep;     /* 0 to 1. 0 means 0.4 */
    bool       smoke;           /* mixed in instead of added: dark smoke, dust */
    const char* texture;        /* a .dt picture, or NULL for a soft round glow */
} DCParticleOpts;

/* most: how many can be alive at once (each one costs 32 bytes of RAM).
 * The options are copied, so the struct above can be a temporary. */
DCParticles* dc_particles_create(int most, const DCParticleOpts* opts);

void dc_particles_free(DCParticles* p);

/* Moves them and draws them. Call it once a frame, after dc_set_camera().
 * Nothing else is needed: new ones are born at .rate, old ones die. */
void dc_particles_draw(DCParticles* p);

/* Where new ones are born from now on (an emitter that follows something) */
void dc_particles_move(DCParticles* p, shz_vec3_t pos);

/* All at once: an explosion, a splash, a puff of dust. Works with .rate 0. */
void dc_particles_burst(DCParticles* p, int how_many);

/* Stop making new ones (the ones alive live out their lives), and start
 * again. A fire being put out. */
void dc_particles_pause(DCParticles* p, bool paused);

/* How big the whole thing is from now on, as a multiple: a fire dying down,
 * a jet opening up. 1 is as asked for. Everything measured in world units
 * goes with it -- how big they are, how fast they set off, how far they
 * scatter -- so a flame at 0.5 is half as tall as well as half as wide. The
 * ones already alive carry on as they were. */
void dc_particles_scale(DCParticles* p, float scale);

/* How many are alive, for the stats line */
int dc_particles_count(const DCParticles* p);

#endif /* DC_PARTICLES_H */
