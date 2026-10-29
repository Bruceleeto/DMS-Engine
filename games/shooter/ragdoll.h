#ifndef RAGDOLL_H
#define RAGDOLL_H

#include <stdbool.h>
#include <sh4zam/shz_sh4zam.h>
#include "dms/dc_engine.h"
#include "dms/collision.h"

/* A body gone limp: a point at each big joint (hips, chest, head, shoulders,
 * elbows, hands, hips, knees, feet), kept their lengths apart, pulled down
 * and kept out of the world. The bones are turned to follow the points, so
 * the mesh lies the way they do.
 *
 * Made for Valve's biped skeleton (ValveBiped.Bip01_*), which every Half-Life
 * 2 and Left 4 Dead person has. */

#define RD_POINTS 15
#define RD_FRAMES 10
#define RD_BONES  64

typedef struct {
    shz_vec3_t p[RD_POINTS], old[RD_POINTS];   /* in the world, now and a step ago */
    shz_vec3_t p0[RD_POINTS];                  /* where they were when it went limp,
                                                * in the model's space */
    float b0[RD_FRAMES][9];                    /* the two-way frames then (hips, chest) */
    float m0[RD_BONES][12];                    /* each bone then: 3 columns and where */
    float len[40];                             /* the sticks' lengths */
    shz_vec3_t rest[RD_POINTS];                /* where they were when it last moved */
    float calm;                                /* how long none has gone far from there */
    bool still;                                /* come to rest: no more steps */
    bool tracked;                              /* rd_track has been called */
} Ragdoll;

/* The bones it needs, found once in a model of the kind: false without them */
bool rd_init(const DMSModel* m);

/* Where the joints are now (the model drawn at pos turned yaw, as dc_draw_ex
 * turns it), kept each frame while it still plays a clip: when it goes limp
 * it carries on as the clip was moving */
void rd_track(Ragdoll* r, const DMSModel* m, shz_vec3_t pos, float yaw);

/* Limp from the pose it has now; push (metres a second) onto its upper body,
 * a shot's knock. rd_track must have been called at least once before. */
void rd_start(Ragdoll* r, const DMSModel* m, shz_vec3_t pos, float yaw, shz_vec3_t push);

/* A step of dt: falls, keeps its lengths, kept out of the world */
void rd_step(Ragdoll* r, ColWorld* col, float dt);

/* The bones put where the points are, ready to draw at pos turned yaw */
void rd_pose(const Ragdoll* r, DMSModel* m, shz_vec3_t pos, float yaw);

#endif
