#ifndef DC_BOLT_H
#define DC_BOLT_H

#include <stdint.h>
#include <sh4zam/shz_sh4zam.h>

/* ================================================================
 * Bolts
 *
 * Electricity: a jagged arc between two points, a white-hot core in a glow
 * of its colour, with a fork or two off it. A new shape every frame, so it
 * crackles. Added over the frame, facing the camera set when it is called.
 * Say where it is every frame it shows.
 *
 *     if (zapping) dc_bolt(gun_tip, hit, 0x6090FF, 0.02f, 0.1f);
 *
 * Costs a 32x32 texture (2 KB), and some 50 quads a bolt.
 * ================================================================ */

/* An arc from `from` to `to`, rgb (0xRRGGBB) its glow, width its core
 * across (metres), wild how far it strays from the straight line (a share
 * of its length: 0.1 is a spark's crooked jump). */
void dc_bolt(shz_vec3_t from, shz_vec3_t to, uint32_t rgb, float width, float wild);

#endif /* DC_BOLT_H */
