#ifndef DC_DEBUG_H
#define DC_DEBUG_H

#include "dc_camera.h"

/* Initialize debug primitives. Call once after dc_init(). */
void dc_debug_init(void);

/* Draw a wireframe sphere on the OP list. Auto-opens OP if needed. */
void dc_debug_sphere(shz_vec3_t center, float radius,
                     uint32_t argb, const DCCamera* cam);

/* The standard on-screen readout: FPS top left, and bottom left the frame,
 * animation, camera and draw times with mesh and vertex counts, averaged over
 * 60 frames. Call once a frame, at any point. The engine gathers it all. */
void dc_debug_stats(void);

#endif /* DC_DEBUG_H */
