#ifndef COLLISION_H
#define COLLISION_H

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "main.h"

/* ---- Collision triangle: 64 bytes = 2 cache lines ---- */
typedef struct __attribute__((aligned(32))) {
    shz_vec3_t v0;
    shz_vec3_t edge1;
    shz_vec3_t edge2;
    shz_vec3_t normal;
    shz_vec3_t grav_P;
    float      grav_inv_det;
} ColTri;

/* ---- 2D grid cell (XZ plane) ---- */
typedef struct {
    uint32_t start;
    uint32_t count;
} ColCell;

/* ---- Collision world ---- */
typedef struct {
    ColTri*   tris;
    uint32_t* tri_idx;
    ColCell*  cells;
    int       tri_count;
    int       gx, gz;
    float     cell_size;
    float     inv_cell;
    float     min_x, min_z;
    float     min_y, max_y;
} ColWorld;

/* ---- Ground hit result ---- */
typedef struct {
    int   hit;
    float y;
} ColGroundHit;

/* ---- Ray hit result ---- */
typedef struct {
    int   hit;
    float t;
    shz_vec3_t pos;
    shz_vec3_t normal;
} ColRayHit;

ColWorld*    col_build(DMSModel* dms_model, shz_vec3_t pos, float scale, float cell_size);
shz_vec3_t   col_move(ColWorld* w, shz_vec3_t from, shz_vec3_t to, float radius);
ColGroundHit col_ground(ColWorld* w, shz_vec3_t origin, float max_dist);
ColRayHit  col_raycast(ColWorld* w, shz_vec3_t org, shz_vec3_t dir, float max_dist);
void       col_free(ColWorld* w);

/* ================================================================ */
#ifdef COLLISION_IMPLEMENTATION
/* ================================================================ */

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- helpers ---- */

static inline float col_minf(float a, float b) { return a < b ? a : b; }
static inline float col_maxf(float a, float b) { return a > b ? a : b; }

static inline int col_clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int col_cell_x(ColWorld* w, float x) {
    return col_clamp_i((int)((x - w->min_x) * w->inv_cell), 0, w->gx - 1);
}
static inline int col_cell_z(ColWorld* w, float z) {
    return col_clamp_i((int)((z - w->min_z) * w->inv_cell), 0, w->gz - 1);
}

/* Compute XZ bounding box of a triangle */
static inline void col_tri_bounds_xz(const ColTri* ct,
                                      float* x0, float* x1,
                                      float* z0, float* z1) {
    float bx = ct->v0.x + ct->edge1.x;
    float bz = ct->v0.z + ct->edge1.z;
    float cx = ct->v0.x + ct->edge2.x;
    float cz = ct->v0.z + ct->edge2.z;

    *x0 = col_minf(ct->v0.x, col_minf(bx, cx));
    *x1 = col_maxf(ct->v0.x, col_maxf(bx, cx));
    *z0 = col_minf(ct->v0.z, col_minf(bz, cz));
    *z1 = col_maxf(ct->v0.z, col_maxf(bz, cz));
}

/* ================================================================
 * col_build
 * ================================================================ */
ColWorld* col_build(DMSModel* mdl, shz_vec3_t pos, float scale, float cell_size) {

    /* --- Pass 1: count triangles --- */
    int total_tris = 0;
    for (uint32_t m = 0; m < mdl->mesh_count; m++) {
        DMSMesh* mesh = &mdl->meshes[m];
        int strip_len = 0;
        for (uint32_t v = 0; v < mesh->vertex_count; v++) {
            strip_len++;
            if (strip_len >= 3) total_tris++;
            if (mesh->vertices[v].flags == 0xF0000000)
                strip_len = 0;
        }
    }

    printf("COL: %d triangles from %u meshes\n", total_tris, (unsigned)mdl->mesh_count);
    if (total_tris == 0) return NULL;

    ColWorld* w = (ColWorld*)malloc(sizeof(ColWorld));
    w->tris = (ColTri*)memalign(32, total_tris * sizeof(ColTri));
    w->tri_count = 0;
    w->cell_size = cell_size;
    w->inv_cell = 1.0f / cell_size;

    /* --- Pass 2: extract triangles from strips --- */
    shz_vec3_t ring[3];
    int ring_w = 0;
    int strip_len = 0;

    float bmin_x =  1e18f, bmin_y =  1e18f, bmin_z =  1e18f;
    float bmax_x = -1e18f, bmax_y = -1e18f, bmax_z = -1e18f;

    for (uint32_t m = 0; m < mdl->mesh_count; m++) {
        DMSMesh* mesh = &mdl->meshes[m];
        strip_len = 0;
        ring_w = 0;

        for (uint32_t v = 0; v < mesh->vertex_count; v++) {
            DMSVertex* sv = &mesh->vertices[v];

            float wx = sv->x * scale + pos.x;
            float wy = sv->y * scale + pos.y;
            float wz = sv->z * scale + pos.z;

            bmin_x = col_minf(bmin_x, wx);
            bmax_x = col_maxf(bmax_x, wx);
            bmin_y = col_minf(bmin_y, wy);
            bmax_y = col_maxf(bmax_y, wy);
            bmin_z = col_minf(bmin_z, wz);
            bmax_z = col_maxf(bmax_z, wz);

            ring[ring_w % 3] = shz_vec3_init(wx, wy, wz);
            ring_w++;
            strip_len++;

            if (strip_len >= 3) {
                int tri_idx_in_strip = strip_len - 3;
                shz_vec3_t a, b, c;

                if (tri_idx_in_strip & 1) {
                    a = ring[(ring_w - 2) % 3];
                    b = ring[(ring_w - 3) % 3];
                    c = ring[(ring_w - 1) % 3];
                } else {
                    a = ring[(ring_w - 3) % 3];
                    b = ring[(ring_w - 2) % 3];
                    c = ring[(ring_w - 1) % 3];
                }

                ColTri* ct = &w->tris[w->tri_count];

                ct->v0 = a;
                ct->edge1 = shz_vec3_sub(b, a);
                ct->edge2 = shz_vec3_sub(c, a);

                ct->normal = shz_vec3_cross(ct->edge1, ct->edge2);
                float len_sq = shz_mag_sqr3f(ct->normal.x, ct->normal.y, ct->normal.z);
                if (len_sq > 1e-12f) {
                    float inv_len = shz_inv_sqrtf(len_sq);
                    ct->normal.x *= inv_len;
                    ct->normal.y *= inv_len;
                    ct->normal.z *= inv_len;
                } else {
                    goto next_vert;
                }

                ct->grav_P = shz_vec3_init(-ct->edge2.z, 0.0f, ct->edge2.x);
                float det = shz_dot6f(ct->edge1.x, ct->edge1.y, ct->edge1.z,
                                      ct->grav_P.x, ct->grav_P.y, ct->grav_P.z);
                if (fabsf(det) < 1e-7f) {
                    ct->grav_inv_det = 0.0f;
                } else {
                    ct->grav_inv_det = 1.0f / det;
                }

                w->tri_count++;
            }
            next_vert:

            if (sv->flags == 0xF0000000) {
                strip_len = 0;
                ring_w = 0;
            }
        }
    }

    printf("COL: %d valid triangles\n", w->tri_count);

    /* --- Build 2D XZ grid --- */
    float pad = cell_size * 0.5f;
    w->min_x = bmin_x - pad;
    w->min_z = bmin_z - pad;
    w->min_y = bmin_y;
    w->max_y = bmax_y;

    w->gx = (int)((bmax_x + pad - w->min_x) * w->inv_cell) + 1;
    w->gz = (int)((bmax_z + pad - w->min_z) * w->inv_cell) + 1;

    int total_cells = w->gx * w->gz;
    printf("COL: Grid %dx%d (%d cells), cell_size=%.1f\n",
           w->gx, w->gz, total_cells, cell_size);

    uint32_t* counts = (uint32_t*)calloc(total_cells, sizeof(uint32_t));

    for (int i = 0; i < w->tri_count; i++) {
        float tx0, tx1, tz0, tz1;
        col_tri_bounds_xz(&w->tris[i], &tx0, &tx1, &tz0, &tz1);

        int x0 = col_cell_x(w, tx0), x1 = col_cell_x(w, tx1);
        int z0 = col_cell_z(w, tz0), z1 = col_cell_z(w, tz1);

        for (int gz = z0; gz <= z1; gz++)
            for (int gx = x0; gx <= x1; gx++)
                counts[gz * w->gx + gx]++;
    }

    w->cells = (ColCell*)malloc(total_cells * sizeof(ColCell));
    uint32_t total_refs = 0;
    for (int i = 0; i < total_cells; i++) {
        w->cells[i].start = total_refs;
        w->cells[i].count = counts[i];
        total_refs += counts[i];
    }

    printf("COL: %lu tri-cell references\n", (unsigned long)total_refs);

    w->tri_idx = (uint32_t*)malloc(total_refs * sizeof(uint32_t));
    memset(counts, 0, total_cells * sizeof(uint32_t));

    for (int i = 0; i < w->tri_count; i++) {
        float tx0, tx1, tz0, tz1;
        col_tri_bounds_xz(&w->tris[i], &tx0, &tx1, &tz0, &tz1);

        int x0 = col_cell_x(w, tx0), x1 = col_cell_x(w, tx1);
        int z0 = col_cell_z(w, tz0), z1 = col_cell_z(w, tz1);

        for (int gz = z0; gz <= z1; gz++) {
            for (int gx = x0; gx <= x1; gx++) {
                int ci = gz * w->gx + gx;
                w->tri_idx[w->cells[ci].start + counts[ci]] = i;
                counts[ci]++;
            }
        }
    }

    free(counts);
    printf("COL: Build complete\n");
    return w;
}

/* ================================================================
 * Closest point on triangle (Voronoi region method)
 * ================================================================ */
static inline shz_vec3_t col_closest_on_tri(const ColTri* ct, shz_vec3_t p) {
    shz_vec3_t ap = shz_vec3_sub(p, ct->v0);

    float d1 = shz_dot6f(ct->edge1.x, ct->edge1.y, ct->edge1.z, ap.x, ap.y, ap.z);
    float d2 = shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z, ap.x, ap.y, ap.z);

    if (d1 <= 0.0f && d2 <= 0.0f) return ct->v0;

    shz_vec3_t b = shz_vec3_add(ct->v0, ct->edge1);
    shz_vec3_t bp = shz_vec3_sub(p, b);
    float d3 = shz_dot6f(ct->edge1.x, ct->edge1.y, ct->edge1.z, bp.x, bp.y, bp.z);
    float d4 = shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z, bp.x, bp.y, bp.z);

    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return shz_vec3_init(ct->v0.x + ct->edge1.x * v,
                             ct->v0.y + ct->edge1.y * v,
                             ct->v0.z + ct->edge1.z * v);
    }

    shz_vec3_t c = shz_vec3_add(ct->v0, ct->edge2);
    shz_vec3_t cp = shz_vec3_sub(p, c);
    float d5 = shz_dot6f(ct->edge1.x, ct->edge1.y, ct->edge1.z, cp.x, cp.y, cp.z);
    float d6 = shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z, cp.x, cp.y, cp.z);

    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float ww = d2 / (d2 - d6);
        return shz_vec3_init(ct->v0.x + ct->edge2.x * ww,
                             ct->v0.y + ct->edge2.y * ww,
                             ct->v0.z + ct->edge2.z * ww);
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float ww = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return shz_vec3_init(b.x + (c.x - b.x) * ww,
                             b.y + (c.y - b.y) * ww,
                             b.z + (c.z - b.z) * ww);
    }

    float denom = 1.0f / (va + vb + vc);
    float sv = vb * denom;
    float sw = vc * denom;
    return shz_vec3_init(ct->v0.x + ct->edge1.x * sv + ct->edge2.x * sw,
                         ct->v0.y + ct->edge1.y * sv + ct->edge2.y * sw,
                         ct->v0.z + ct->edge1.z * sv + ct->edge2.z * sw);
}

/* ================================================================
 * Gravity ray: D = (0, -1, 0) -- precomputed P and inv_det
 * ================================================================ */
static inline int col_gravity_ray_tri(const ColTri* ct, shz_vec3_t origin,
                                      float max_t, float* out_t) {
    if (ct->grav_inv_det == 0.0f) return 0;

    shz_vec3_t s = shz_vec3_sub(origin, ct->v0);

    float u = shz_dot6f(s.x, s.y, s.z,
                        ct->grav_P.x, ct->grav_P.y, ct->grav_P.z)
              * ct->grav_inv_det;
    if (u < 0.0f || u > 1.0f) return 0;

    shz_vec3_t Q = shz_vec3_cross(s, ct->edge1);

    float v = -Q.y * ct->grav_inv_det;
    if (v < 0.0f || u + v > 1.0f) return 0;

    float t = shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z,
                        Q.x, Q.y, Q.z)
              * ct->grav_inv_det;

    if (t > 1e-4f && t < max_t) {
        *out_t = t;
        return 1;
    }
    return 0;
}

/* ================================================================
 * General Moller-Trumbore ray-tri
 * ================================================================ */
static inline int col_ray_tri(const ColTri* ct, shz_vec3_t org,
                              shz_vec3_t dir, float max_t, float* out_t) {
    shz_vec3_t h = shz_vec3_cross(dir, ct->edge2);
    float a = shz_dot6f(ct->edge1.x, ct->edge1.y, ct->edge1.z, h.x, h.y, h.z);

    if (a > -1e-7f && a < 1e-7f) return 0;
    float f = 1.0f / a;

    shz_vec3_t s = shz_vec3_sub(org, ct->v0);
    float u = f * shz_dot6f(s.x, s.y, s.z, h.x, h.y, h.z);
    if (u < 0.0f || u > 1.0f) return 0;

    shz_vec3_t q = shz_vec3_cross(s, ct->edge1);
    float v = f * shz_dot6f(dir.x, dir.y, dir.z, q.x, q.y, q.z);
    if (v < 0.0f || u + v > 1.0f) return 0;

    float t = f * shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z, q.x, q.y, q.z);
    if (t > 1e-5f && t < max_t) {
        *out_t = t;
        return 1;
    }
    return 0;
}

/* ================================================================
 * col_move -- sphere collide-and-slide
 * ================================================================ */
#define COL_MOVE_ITERS 3

shz_vec3_t col_move(ColWorld* w, shz_vec3_t from, shz_vec3_t to, float radius) {
    if (!w) return to;

    float radius_sq = radius * radius;
    shz_vec3_t resolved = to;

    for (int iter = 0; iter < COL_MOVE_ITERS; iter++) {
        int pushed = 0;

        int cx0 = col_cell_x(w, resolved.x - radius);
        int cx1 = col_cell_x(w, resolved.x + radius);
        int cz0 = col_cell_z(w, resolved.z - radius);
        int cz1 = col_cell_z(w, resolved.z + radius);

        for (int gz = cz0; gz <= cz1; gz++) {
            for (int gx = cx0; gx <= cx1; gx++) {
                ColCell* cell = &w->cells[gz * w->gx + gx];
                uint32_t* idx = &w->tri_idx[cell->start];

                for (int i = 0; i < cell->count; i++) {
                    if (SHZ_LIKELY(i + 1 < cell->count))
                        SHZ_PREFETCH(&w->tris[idx[i + 1]]);

                    ColTri* ct = &w->tris[idx[i]];
                    shz_vec3_t closest = col_closest_on_tri(ct, resolved);
                    shz_vec3_t diff = shz_vec3_sub(resolved, closest);

                    float dist_sq = shz_mag_sqr3f(diff.x, diff.y, diff.z);

                    if (dist_sq < radius_sq && dist_sq > 1e-12f) {
                        float dist = shz_sqrtf_fsrra(dist_sq);
                        float inv_d = shz_invf_fsrra(dist);
                        float push = radius - dist;

                        resolved.x += diff.x * inv_d * push;
                        resolved.y += diff.y * inv_d * push;
                        resolved.z += diff.z * inv_d * push;
                        pushed = 1;
                    }
                }
            }
        }

        if (!pushed) break;
    }

    return resolved;
}

/* ================================================================
 * col_ground -- gravity raycast using precomputed data
 * ================================================================ */
ColGroundHit col_ground(ColWorld* w, shz_vec3_t origin, float max_dist) {
    ColGroundHit result = { 0, 0.0f };
    if (!w) return result;

    float best_t = max_dist;

    int cx0 = col_cell_x(w, origin.x - 1.0f);
    int cx1 = col_cell_x(w, origin.x + 1.0f);
    int cz0 = col_cell_z(w, origin.z - 1.0f);
    int cz1 = col_cell_z(w, origin.z + 1.0f);

    for (int gz = cz0; gz <= cz1; gz++) {
        for (int gx = cx0; gx <= cx1; gx++) {
            ColCell* cell = &w->cells[gz * w->gx + gx];
            uint32_t* idx = &w->tri_idx[cell->start];

            for (int i = 0; i < cell->count; i++) {
                if (SHZ_LIKELY(i + 1 < cell->count))
                    SHZ_PREFETCH(&w->tris[idx[i + 1]]);

                float t;
                if (col_gravity_ray_tri(&w->tris[idx[i]], origin, best_t, &t)) {
                    best_t = t;
                    result.hit = 1;
                }
            }
        }
    }

    if (result.hit)
        result.y = origin.y - best_t;

    return result;
}

/* ================================================================
 * col_raycast -- general ray
 * ================================================================ */
ColRayHit col_raycast(ColWorld* w, shz_vec3_t org, shz_vec3_t dir, float max_dist) {
    ColRayHit result = { 0, max_dist, org, shz_vec3_init(0, 1, 0) };
    if (!w) return result;

    float ex = org.x + dir.x * max_dist;
    float ez = org.z + dir.z * max_dist;

    float rx0 = col_minf(org.x, ex);
    float rx1 = col_maxf(org.x, ex);
    float rz0 = col_minf(org.z, ez);
    float rz1 = col_maxf(org.z, ez);

    int cx0 = col_cell_x(w, rx0);
    int cx1 = col_cell_x(w, rx1);
    int cz0 = col_cell_z(w, rz0);
    int cz1 = col_cell_z(w, rz1);

    float best_t = max_dist;

    for (int gz = cz0; gz <= cz1; gz++) {
        for (int gx = cx0; gx <= cx1; gx++) {
            ColCell* cell = &w->cells[gz * w->gx + gx];
            uint32_t* idx = &w->tri_idx[cell->start];

            for (int i = 0; i < cell->count; i++) {
                if (SHZ_LIKELY(i + 1 < cell->count))
                    SHZ_PREFETCH(&w->tris[idx[i + 1]]);

                float t;
                if (col_ray_tri(&w->tris[idx[i]], org, dir, best_t, &t)) {
                    best_t = t;
                    result.hit = 1;
                    result.t = t;
                    result.normal = w->tris[idx[i]].normal;
                    result.pos = shz_vec3_init(org.x + dir.x * t,
                                               org.y + dir.y * t,
                                               org.z + dir.z * t);
                }
            }
        }
    }

    return result;
}

/* ================================================================ */
void col_free(ColWorld* w) {
    if (!w) return;
    free(w->tris);
    free(w->tri_idx);
    free(w->cells);
    free(w);
}

#endif /* COLLISION_IMPLEMENTATION */
#endif /* COLLISION_H */