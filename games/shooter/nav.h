#ifndef NAV_H
#define NAV_H

#include <stdbool.h>
#include <stdint.h>
#include <sh4zam/shz_sh4zam.h>
#include "dms/collision.h"

/* Where the infected can walk, and the way to you from anywhere on it.
 *
 * A grid over the map: each cell the floor at its middle (flat enough to
 * stand on, the one nearest the floor next to it), joined to the cells round it that
 * a body can walk to without a wall or too high a step between. From the
 * cell you are in, nav_flow() works out how far every other is by the
 * shortest way, once for all of them; each then goes downhill.
 *
 * Only the cells that can be walked to are kept, numbered 0..n-1, found by
 * where they are through a table of 16 by 16 tiles (a tile with none in it
 * is nothing): a big map with a little floor (long tunnels) costs what its
 * floor does, not its size. */

#define NAV_TILE 16

typedef struct {
    int w, h;                   /* cells across x and z */
    float x0, z0, cell;         /* where cell (0, 0) starts, and how wide one is */
    int tw, th;                 /* tiles across */
    int32_t** tiles;            /* each tile's cells' numbers (-1 none), or NULL */
    int n;                      /* cells that can be walked to */
    uint16_t *cx, *cz;          /* each one's place on the grid */
    float* floor;               /* y of its floor */
    uint8_t* links;             /* a bit a neighbour, in nav.c's order */
    uint16_t* dist;             /* the way to the goal, 2 a straight step, 3 a
                                 * slanted one; NAV_FAR where there is none */
    int goal;                   /* the cell dist runs to, -1 none yet */
    int* queue;
    uint8_t* queued;
} Nav;

/* How high a step a body walks up (col_move_steps), and where its sphere is
 * over its feet. Movement must use the same. */
#define NAV_STEP   0.45f
#define NAV_BODY_Y 0.85f

#define NAV_NONE -1e30f
#define NAV_FAR  0xFFFF

/* The grid over the map, a body radius wide, of the floor that can be
 * walked to from start (feet). How many cells that is, 0 if none (or no
 * memory, or start is on no floor). */
int  nav_build(Nav* nav, ColWorld* col, float cell, float radius, shz_vec3_t start);
void nav_free(Nav* nav);

/* The cell a place is in, or -1 off the grid or on no floor. y picks the
 * floor nearest. */
int nav_cell(const Nav* nav, shz_vec3_t p);
shz_vec3_t nav_centre(const Nav* nav, int c);

/* The way to where you are, from everywhere. Only works when you have moved
 * to another cell; false when you are off the grid (the old way stays). */
bool nav_flow(Nav* nav, shz_vec3_t goal);

/* Where something radius wide at from should head for next to get to the
 * goal: a cell a few steps along the way that it can see straight to, or the
 * goal itself if nothing is in between. False if from is nowhere on the way. */
bool nav_steer(const Nav* nav, ColWorld* col, shz_vec3_t from, shz_vec3_t goal, float radius,
               shz_vec3_t* to);

/* A floor somewhere between min and max metres from the goal by the way
 * there, so whatever starts there can get to it. */
bool nav_random_spot(const Nav* nav, float min, float max, shz_vec3_t* out);

#endif
