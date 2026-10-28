/* Headless regression: coplanar layers with different triangulation must have
 * identical depth. Run with test/depth_layers.sh from any directory. */
#include <stdio.h>
#include <stdint.h>
#include <dc/pvr.h>
#include "kos_pvr.h"

void kos_pvr_present(void) {}

static void quad(float x0, float y0, float x1, float y1, float z,
                 uint32_t color, pvr_depthcmp_mode_t comparison) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = comparison;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.blend.src = PVR_BLEND_ONE;
    cxt.blend.dst = PVR_BLEND_ZERO;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    pvr_vertex_t v[4] = {};
    for (int i = 0; i < 4; ++i) {
        v[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v[i].x = i & 1 ? x1 : x0;
        v[i].y = i & 2 ? y1 : y0;
        v[i].z = z;
        v[i].argb = color;
    }
    pvr_prim(v, sizeof(v));
}

int main() {
    kos_pvr_set_presort(1);
    if (kos_pvr_init()) return 1;
    static uint8_t rgb[640 * 480 * 3];
    int errors = 0;
    const float z = 0.1885f;
    for (int test = 0; test < 4; ++test) {
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_TR_POLY);
        quad(30.3f, 20.7f, 600.8f, 450.4f, z, 0xffff0000, PVR_DEPTHCMP_ALWAYS);
        quad(63.2f, 70.1f, 430.9f, 320.6f,
             test == 2 ? z - 0.01f : test == 3 ? z + 0.01f : z,
             0xff00ff00, test == 1 ? PVR_DEPTHCMP_GREATER : PVR_DEPTHCMP_GEQUAL);
        pvr_list_finish();
        pvr_scene_finish();
        if (!kos_pvr_read_front(rgb, 640 * 3, 640, 480)) return 1;
        const int green = test == 0 || test == 3;
        int wrong = 0;
        // Sample an interior patch away from both quads' shared edges.
        for (int y = 100; y < 150; ++y) {
            for (int x = 100; x < 200; ++x) {
                const uint8_t *p = rgb + (y * 640 + x) * 3;
                if (green ? (p[1] < 240 || p[0] > 8) : (p[0] < 240 || p[1] > 8)) ++wrong;
            }
        }
        printf("depth case %d: %d incorrect pixels\n", test, wrong);
        errors += wrong;
    }
    kos_pvr_shutdown();
    return errors ? 1 : 0;
}
