#include "dc_debug.h"
#include "dc_engine.h"

#define DBG_RING_SEGS 20
#define DBG_RING_WIDTH 0.04f  /* fraction of radius */

static pvr_poly_hdr_t dbg_poly_header;
static MeshVertex dbg_rings[3][(DBG_RING_SEGS + 1) * 2];

void dc_debug_init(void) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&dbg_poly_header, &cxt);

    static const float axes[3][2][3] = {
        {{1,0,0}, {0,1,0}},
        {{1,0,0}, {0,0,1}},
        {{0,1,0}, {0,0,1}},
    };

    float ri = 1.0f - DBG_RING_WIDTH;
    float ro = 1.0f + DBG_RING_WIDTH;

    for (int ring = 0; ring < 3; ring++) {
        const float *r = axes[ring][0];
        const float *u = axes[ring][1];
        int idx = 0;

        for (int i = 0; i <= DBG_RING_SEGS; i++) {
            float a = (float)i / (float)DBG_RING_SEGS * 6.283185f;
            shz_sincos_t sc = shz_sincosf(a);
            float cx = r[0] * sc.cos + u[0] * sc.sin;
            float cy = r[1] * sc.cos + u[1] * sc.sin;
            float cz = r[2] * sc.cos + u[2] * sc.sin;

            dbg_rings[ring][idx].x = cx * ri;
            dbg_rings[ring][idx].y = cy * ri;
            dbg_rings[ring][idx].z = cz * ri;
            dbg_rings[ring][idx].u = 0.0f;
            dbg_rings[ring][idx].v = 0.0f;
            dbg_rings[ring][idx].argb = 0xFFFFFFFF;
            dbg_rings[ring][idx].flags = PVR_CMD_VERTEX;
            idx++;

            dbg_rings[ring][idx].x = cx * ro;
            dbg_rings[ring][idx].y = cy * ro;
            dbg_rings[ring][idx].z = cz * ro;
            dbg_rings[ring][idx].u = 0.0f;
            dbg_rings[ring][idx].v = 0.0f;
            dbg_rings[ring][idx].argb = 0xFFFFFFFF;
            dbg_rings[ring][idx].flags = PVR_CMD_VERTEX;
            idx++;
        }
        dbg_rings[ring][idx - 1].flags = PVR_CMD_VERTEX_EOL;
    }
}

void dc_debug_sphere(shz_vec3_t center, float radius,
                     uint32_t argb, const DCCamera* cam) {
    dc_list_begin(PVR_LIST_OP_POLY);
    const shz_mat4x4_t* pv = dc_camera_get_pv(cam);

    /* Submit debug poly header */
    shz_sq_memcpy32_1(pvr_dr_target(0), &dbg_poly_header);

    float ox = center.x - cam->pos.x;
    float oy = center.y - cam->pos.y;
    float oz = center.z - cam->pos.z;

    shz_xmtrx_load_4x4((shz_mat4x4_t*)pv);
    shz_xmtrx_translate(ox, oy, -oz);
    shz_xmtrx_apply_scale(radius, radius, radius);

    int ring_verts = (DBG_RING_SEGS + 1) * 2;

    for (int ring = 0; ring < 3; ring++) {
        const MeshVertex* src = dbg_rings[ring];

        SHZ_PREFETCH(&src[0]);
        SHZ_PREFETCH(&src[1]);
        SHZ_PREFETCH(&src[2]);
        SHZ_PREFETCH(&src[3]);

        for (int i = 0; i < ring_verts; i++) {
            SHZ_PREFETCH(&src[i + 4]);

            shz_vec4_t t = shz_xmtrx_transform_vec4(
                shz_vec4_init(src[i].x, src[i].y, -src[i].z, 1.0f));
            t = shz_vec4_swizzle(t, 1, 2, 3, 0);

            float inv_w = (t.w > 0.001f) ? shz_invf_fsrra(t.w) : 0.0f;
            pvr_vertex_t* pv_vert = pvr_dr_target(0);
            pv_vert->flags = src[i].flags;
            pv_vert->x = t.x * inv_w;
            pv_vert->y = t.y * inv_w;
            pv_vert->z = inv_w;
            pv_vert->u = 0.0f;
            pv_vert->v = 0.0f;
            pv_vert->argb = argb;
            pvr_dr_commit(pv_vert);
        }
    }
}
