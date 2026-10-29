#ifndef DC_BEAM_H
#define DC_BEAM_H

#include <sh4zam/shz_sh4zam.h>
#include "collision.h"

/* ================================================================
 * Beams of light in the dark
 *
 * A torch, headlights, a searchlight. The solid world is drawn lit, as it
 * is; outside the beams it is darkened. The PVR does it per pixel and for
 * free: one modifier volume in "cheap shadow" mode, made of a sheet across
 * the view just in front of the camera and each beam's cone. Along a pixel's
 * line of sight an odd number of their faces in front of what is there
 * means outside every beam (the sheet only), so darkened; even (the sheet
 * and a cone's near end, or a cone's side in and out) means lit.
 *
 *     dc_init((DCInitParams){ .beams = true });   // before loading models
 *     ...
 *     dc_set_camera(&camera);
 *     dc_beam_dark(0.25f, 0.2f);                   // a quarter of the light outside
 *     dc_beam(camera.pos, forward, 0.25f, 40.0f);  // a torch
 *     dc_draw(level, origin);
 *
 * Only solid (opaque and cut-out) models are darkened, not the translucent
 * ones, particles or 2D. The volume's edge is hard (the PVR has no soft
 * volumes), so over it each beam lays a picture of its light, multiplied in:
 * a hot spot in the middle, softly falling off into the dark by the rim. It
 * lines up with the edge exactly when from is where the camera is (a cone
 * seen from its tip is the same circle at every distance). 8KB of VRAM.
 * ================================================================ */

/* Dark outside the beams this frame, seen by the camera set now: dark is
 * how much light is left there, 0 (black) to 1 (no darkness: nothing is
 * done). Nothing nearer the camera than near is darkened (where the sheet
 * goes): the hands and the gun. Once a frame, before dc_beam(). */
void dc_beam_dark(float dark, float near);

/* The world the beams fall on, as GTA lays its lights on the road: the
 * collision triangles in each beam get its light projected onto them, from
 * where it starts (a hot spot, soft falloff, brighter where a surface faces
 * it, fading with distance), so a lamp off to one side throws a pool that
 * stretches out across the floor. Its rim is the volume's edge, so a beam
 * can start anywhere. NULL: a picture over the beam on the screen instead,
 * which only lines up with the edge when the beam starts at the camera.
 * Once; the world must stay as long as it is set. */
void dc_beam_onto(ColWorld* col);

/* A beam this frame, from from (the camera's place, or near it: the camera
 * must not be inside the cone) along dir (unit), spreading half_angle
 * radians either side, length long. It starts a little past dc_beam_dark's
 * near. Up to 4 a frame. */
void dc_beam(shz_vec3_t from, shz_vec3_t dir, float half_angle, float length);

/* The beam seen in the air: a shaft of light, added over what is behind it,
 * from from (a lamp: the torch's lens) along dir, r0 wide there and
 * spreading half_angle radians either side, fading out by length. rgb
 * 0xRRGGBB, strength 0..1 how bright it gets. Nothing nearer the lamp than
 * start: it fades in from there (so the bit by a held torch, filling the
 * view, is left out). Its sides fade as they turn edge on, so it reads as
 * lit haze, not a cone. Up to 4 a frame, after dc_beam_dark(). */
void dc_beam_shaft(shz_vec3_t from, shz_vec3_t dir, float r0, float half_angle, float start,
                   float length, uint32_t rgb, float strength);

#endif /* DC_BEAM_H */
