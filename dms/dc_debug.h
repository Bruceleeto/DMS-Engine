#ifndef DC_DEBUG_H
#define DC_DEBUG_H

#include "dc_camera.h"

/* Initialize debug primitives. Call once after dc_init(). */
void dc_debug_init(void);

/* Draw a wireframe sphere on the OP list. Auto-opens OP if needed. */
void dc_debug_sphere(shz_vec3_t center, float radius,
                     uint32_t argb, const DCCamera* cam);

#endif /* DC_DEBUG_H */
