#ifndef DC_CORONA_H
#define DC_CORONA_H

#include <stdint.h>
#include <sh4zam/shz_sh4zam.h>

/* ================================================================
 * Coronas
 *
 * The glow round a light seen from the front: headlights, a siren, a street
 * lamp. A star-shaped glow that always faces the camera, added over the
 * frame, drawn over everything but what is in front of it. How GTA 3 did its
 * lights.
 *
 * Say where it is every frame it is lit. Stop saying and it fades out; start
 * again and it fades in, so a light switched on and off never pops.
 *
 *     while (playing) {
 *         dc_frame_begin();
 *         dc_set_camera(&camera);
 *         dc_draw(car, pos);
 *         if (flash_on) dc_corona(1, lamp_pos, 0xFF0000, 1.0f, 0);
 *         dc_frame_end();
 *     }
 * ================================================================ */

/* flags for dc_corona() */
#define DC_CORONA_FLARE (1u << 0)   /* a lens flare when looked at: discs from the
                                     * light through the middle of the screen */

/* Lit this frame. id: any number, the same for the same light each frame.
 * pos: where, in the world. rgb: 0xRRGGBB. size: how wide, in world units.
 * flags: DC_CORONA_* or 0. */
void dc_corona(uint32_t id, shz_vec3_t pos, uint32_t rgb, float size, uint32_t flags);

/* Engine use: fades them and queues their drawing. Called by dc_draw_flush()
 * when the game links coronas. */
void dc_corona_flush(void);

#endif /* DC_CORONA_H */
