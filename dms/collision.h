#ifndef COLLISION_H
#define COLLISION_H

#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include "main.h"

/* ---- Collision triangle, rebuilt on the fly from the model's own strip
 * vertices (no second copy of the level in RAM) ---- */
typedef struct {
    shz_vec3_t v0;
    shz_vec3_t edge1;
    shz_vec3_t edge2;
} ColTri;

/* A triangle reference is a pointer to its first strip vertex. DMSVertex is
 * 32-byte aligned, so bit 0 is free: set for odd strip triangles, whose
 * first two vertices swap to keep the winding. */
typedef uintptr_t ColTriRef;

/* ---- 2D grid cell (XZ plane). Only cells with triangles are stored, in a
 * hash table keyed by gz * gx + gx, so empty parts of a big map cost nothing. ---- */
#define COL_EMPTY_KEY 0xFFFFFFFFu
typedef struct {
    uint32_t key;
    uint32_t start;
    uint32_t count;
} ColCell;

/* ---- Collision world ---- */
typedef struct {
    ColTriRef* tri_refs;    /* per cell, points into the model's vertices */
    ColCell*   cells;       /* hash table, cell_mask + 1 slots */
    uint32_t   cell_mask;
    int        tri_count;
    shz_vec3_t pos;         /* model placement (world = vertex * scale + pos) */
    float      scale;
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

/* The model must stay loaded while the ColWorld is used. */
/* Cell size is chosen automatically from the model and the free RAM. */
ColWorld*    col_build(DMSModel* dms_model, shz_vec3_t pos, float scale);
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
#include <malloc.h>
#include <unistd.h>

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

static inline uint32_t col_hash(uint32_t key) {
    return key * 2654435761u;
}

/* Slot for a key: its cell, or the empty slot where it would go */
static inline ColCell* col_slot(ColCell* cells, uint32_t mask, uint32_t key) {
    uint32_t i = col_hash(key) & mask;
    while (cells[i].key != key && cells[i].key != COL_EMPTY_KEY)
        i = (i + 1) & mask;
    return &cells[i];
}

/* Cell at grid position, or NULL if it has no triangles */
static inline const ColCell* col_find_cell(const ColWorld* w, int gx, int gz) {
    const ColCell* c = col_slot(w->cells, w->cell_mask, (uint32_t)(gz * w->gx + gx));
    return c->key == COL_EMPTY_KEY ? NULL : c;
}

static inline shz_vec3_t col_world_vert(const ColWorld* w, const DMSVertex* v) {
    return shz_vec3_init(v->x * w->scale + w->pos.x,
                         v->y * w->scale + w->pos.y,
                         v->z * w->scale + w->pos.z);
}

/* Rebuild a triangle from its reference */
static inline void col_tri_get(const ColWorld* w, ColTriRef ref, ColTri* ct) {
    const DMSVertex* p = (const DMSVertex*)(ref & ~(uintptr_t)1);
    int odd = (int)(ref & 1);
    shz_vec3_t a = col_world_vert(w, &p[odd ? 1 : 0]);
    shz_vec3_t b = col_world_vert(w, &p[odd ? 0 : 1]);
    shz_vec3_t c = col_world_vert(w, &p[2]);
    ct->v0 = a;
    ct->edge1 = shz_vec3_sub(b, a);
    ct->edge2 = shz_vec3_sub(c, a);
}

static inline void col_prefetch_ref(ColTriRef ref) {
    const DMSVertex* p = (const DMSVertex*)(ref & ~(uintptr_t)1);
    SHZ_PREFETCH(&p[0]);
    SHZ_PREFETCH(&p[2]);
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

/* Walk every non-degenerate strip triangle. Returns how many there are;
 * with 'refs' set, also stores each triangle's reference. */
static int col_collect_tris(const ColWorld* w, DMSModel* mdl, ColTriRef* refs) {
    int n = 0;
    for (uint32_t m = 0; m < mdl->mesh_count; m++) {
        DMSMesh* mesh = &mdl->meshes[m];
        int strip_len = 0;
        for (uint32_t v = 0; v < mesh->vertex_count; v++) {
            strip_len++;
            if (strip_len >= 3) {
                ColTriRef ref = (ColTriRef)&mesh->vertices[v - 2]
                              | (ColTriRef)((strip_len - 3) & 1);
                ColTri ct;
                col_tri_get(w, ref, &ct);
                shz_vec3_t nrm = shz_vec3_cross(ct.edge1, ct.edge2);
                if (shz_mag_sqr3f(nrm.x, nrm.y, nrm.z) > 1e-12f) {
                    if (refs) refs[n] = ref;
                    n++;
                }
            }
            if (mesh->vertices[v].flags == 0xF0000000)
                strip_len = 0;
        }
    }
    return n;
}

/* Place the grid for one cell size (origin, dimensions). Returns 0 if the
 * cell keys wouldn't fit in 32 bits. */
static int col_grid_setup(ColWorld* w, float cell_size,
                          float bmin_x, float bmax_x, float bmin_z, float bmax_z) {
    float pad = cell_size * 0.5f;
    w->cell_size = cell_size;
    w->inv_cell = 1.0f / cell_size;
    w->min_x = bmin_x - pad;
    w->min_z = bmin_z - pad;
    float fgx = (bmax_x + pad - w->min_x) * w->inv_cell + 1.0f;
    float fgz = (bmax_z + pad - w->min_z) * w->inv_cell + 1.0f;
    if (fgx * fgz >= 4.0e9f) return 0;
    w->gx = (int)fgx;
    w->gz = (int)fgz;
    return 1;
}

/* Hash slots for 'used' cells: a power of two, at most half full */
static uint32_t col_hash_slots(uint32_t used) {
    uint32_t n = 16;
    while (n < used * 2) n <<= 1;
    return n;
}

/* Exact RAM a grid would take at one cell size, without building it:
 * triangle references + hash table. The cell bitmap used for counting is
 * temporary. Returns 0 if it can't be measured within 'budget'. */
typedef struct { uint32_t refs, used, bytes; } ColGridCost;

static int col_grid_cost(ColWorld* w, const ColTriRef* tris, size_t budget,
                         ColGridCost* out) {
    size_t cells = (size_t)w->gx * (size_t)w->gz;
    size_t map_bytes = (cells + 31) / 32 * 4;
    if (map_bytes > budget) return 0;
    uint32_t* map = (uint32_t*)calloc(1, map_bytes);
    if (!map) return 0;

    uint32_t refs = 0, used = 0;
    for (int i = 0; i < w->tri_count; i++) {
        ColTri ct;
        float tx0, tx1, tz0, tz1;
        col_tri_get(w, tris[i], &ct);
        col_tri_bounds_xz(&ct, &tx0, &tx1, &tz0, &tz1);
        int x0 = col_cell_x(w, tx0), x1 = col_cell_x(w, tx1);
        int z0 = col_cell_z(w, tz0), z1 = col_cell_z(w, tz1);
        refs += (uint32_t)((x1 - x0 + 1) * (z1 - z0 + 1));
        for (int gz = z0; gz <= z1; gz++) {
            for (int gx = x0; gx <= x1; gx++) {
                uint32_t key = (uint32_t)(gz * w->gx + gx);
                uint32_t bit = 1u << (key & 31);
                if (!(map[key >> 5] & bit)) { map[key >> 5] |= bit; used++; }
            }
        }
    }
    free(map);

    out->refs = refs;
    out->used = used;
    out->bytes = refs * sizeof(ColTriRef) + col_hash_slots(used) * sizeof(ColCell);
    return 1;
}

/* Heap RAM still free: what malloc holds unused + what sbrk can still give */
static size_t col_free_ram(void) {
#ifdef _arch_dreamcast
    struct mallinfo mi = mallinfo();
    uintptr_t brk = (uintptr_t)sbrk(0);
    uintptr_t top = _arch_mem_top - THD_KERNEL_STACK_SIZE;
    return (size_t)mi.fordblks + (top > brk ? top - brk : 0);
#else
#ifndef COL_HOST_FREE_RAM
#define COL_HOST_FREE_RAM (64u << 20)   /* PC test builds */
#endif
    return COL_HOST_FREE_RAM;
#endif
}

/* Build the grid at the current cell size; 'used' comes from col_grid_cost,
 * so the hash is allocated once at its final size. */
static int col_build_grid(ColWorld* w, const ColTriRef* tris, const ColGridCost* cost) {
    uint32_t slots = col_hash_slots(cost->used), mask = slots - 1;
    ColCell* cells = (ColCell*)malloc(slots * sizeof(ColCell));
    ColTriRef* refs = (ColTriRef*)malloc(cost->refs * sizeof(ColTriRef));
    if (!cells || !refs) { free(cells); free(refs); return 0; }
    memset(cells, 0xFF, slots * sizeof(ColCell));

    /* Count triangles per cell */
    for (int i = 0; i < w->tri_count; i++) {
        ColTri ct;
        float tx0, tx1, tz0, tz1;
        col_tri_get(w, tris[i], &ct);
        col_tri_bounds_xz(&ct, &tx0, &tx1, &tz0, &tz1);
        int x0 = col_cell_x(w, tx0), x1 = col_cell_x(w, tx1);
        int z0 = col_cell_z(w, tz0), z1 = col_cell_z(w, tz1);
        for (int gz = z0; gz <= z1; gz++) {
            for (int gx = x0; gx <= x1; gx++) {
                uint32_t key = (uint32_t)(gz * w->gx + gx);
                ColCell* c = col_slot(cells, mask, key);
                if (c->key == COL_EMPTY_KEY) { c->key = key; c->count = 0; }
                c->count++;
            }
        }
    }

    uint32_t start = 0;
    for (uint32_t k = 0; k <= mask; k++) {
        if (cells[k].key == COL_EMPTY_KEY) continue;
        cells[k].start = start;
        start += cells[k].count;
        cells[k].count = 0;   /* refilled below */
    }

    for (int i = 0; i < w->tri_count; i++) {
        ColTri ct;
        float tx0, tx1, tz0, tz1;
        col_tri_get(w, tris[i], &ct);
        col_tri_bounds_xz(&ct, &tx0, &tx1, &tz0, &tz1);
        int x0 = col_cell_x(w, tx0), x1 = col_cell_x(w, tx1);
        int z0 = col_cell_z(w, tz0), z1 = col_cell_z(w, tz1);
        for (int gz = z0; gz <= z1; gz++) {
            for (int gx = x0; gx <= x1; gx++) {
                ColCell* c = col_slot(cells, mask, (uint32_t)(gz * w->gx + gx));
                refs[c->start + c->count++] = tris[i];
            }
        }
    }

    w->cells = cells;
    w->cell_mask = mask;
    w->tri_refs = refs;
    return 1;
}

/* ================================================================
 * col_build
 *
 * The cell size is picked automatically. Starting coarse, it halves the
 * cell size while that still fits in half the free RAM (the rest is left
 * for the game), cells still hold more than COL_TARGET_TRIS triangles on
 * average, and halving still helps. Every size is measured exactly before anything is allocated,
 * so the build never runs out of memory part-way.
 * ================================================================ */
#define COL_TARGET_TRIS  8

ColWorld* col_build(DMSModel* mdl, shz_vec3_t pos, float scale) {
    ColWorld* w = (ColWorld*)calloc(1, sizeof(ColWorld));
    if (!w) return NULL;
    w->pos = pos;
    w->scale = scale;

    /* --- Pass 1: count triangles, bounds --- */
    w->tri_count = col_collect_tris(w, mdl, NULL);
    printf("COL: %d valid triangles from %u meshes\n", w->tri_count, (unsigned)mdl->mesh_count);
    if (w->tri_count == 0) { free(w); return NULL; }

    /* Temporary list of every triangle; checked first so malloc never has
     * to fail (KOS prints its own "Out of memory" when sbrk runs out) */
    size_t list_bytes = w->tri_count * sizeof(ColTriRef);
    ColTriRef* tris = list_bytes < col_free_ram() / 2 ? (ColTriRef*)malloc(list_bytes) : NULL;
    if (!tris) {
        printf("COL: not enough RAM for collision (%luKB free)\n",
               (unsigned long)(col_free_ram() / 1024));
        free(w);
        return NULL;
    }
    col_collect_tris(w, mdl, tris);

    float bmin_x =  1e18f, bmin_y =  1e18f, bmin_z =  1e18f;
    float bmax_x = -1e18f, bmax_y = -1e18f, bmax_z = -1e18f;
    for (int i = 0; i < w->tri_count; i++) {
        ColTri ct;
        col_tri_get(w, tris[i], &ct);
        shz_vec3_t b = shz_vec3_add(ct.v0, ct.edge1);
        shz_vec3_t c = shz_vec3_add(ct.v0, ct.edge2);
        bmin_x = col_minf(bmin_x, col_minf(ct.v0.x, col_minf(b.x, c.x)));
        bmax_x = col_maxf(bmax_x, col_maxf(ct.v0.x, col_maxf(b.x, c.x)));
        bmin_y = col_minf(bmin_y, col_minf(ct.v0.y, col_minf(b.y, c.y)));
        bmax_y = col_maxf(bmax_y, col_maxf(ct.v0.y, col_maxf(b.y, c.y)));
        bmin_z = col_minf(bmin_z, col_minf(ct.v0.z, col_minf(b.z, c.z)));
        bmax_z = col_maxf(bmax_z, col_maxf(ct.v0.z, col_maxf(b.z, c.z)));
    }
    w->min_y = bmin_y;
    w->max_y = bmax_y;

    /* --- Pick the cell size --- */
    size_t free_ram = col_free_ram();
    size_t budget = free_ram / 2;

    float extent = col_maxf(bmax_x - bmin_x, bmax_z - bmin_z);
    float cs = 1.0f;                        /* power of two, ~8 cells across */
    while (cs * 8.0f < extent) cs *= 2.0f;
    while (cs * 8.0f > extent * 2.0f && cs > 1.0f / 64.0f) cs *= 0.5f;

    float best_cs = 0.0f;
    ColGridCost best = {0, 0, 0}, cost;
    for (int step = 0; step < 24; step++, cs *= 0.5f) {
        if (!col_grid_setup(w, cs, bmin_x, bmax_x, bmin_z, bmax_z)) break;
        if (!col_grid_cost(w, tris, budget, &cost) || cost.bytes > budget) break;
        /* Stop when halving no longer cuts tris/cell by a quarter (tall
         * stacked geometry never gets below the target) */
        if (best_cs != 0.0f &&
            (uint64_t)cost.refs * best.used * 4 > (uint64_t)best.refs * cost.used * 3) break;
        best_cs = cs;
        best = cost;
        if (cost.refs <= cost.used * COL_TARGET_TRIS) break;   /* fine enough */
    }

    if (best_cs == 0.0f ||
        !col_grid_setup(w, best_cs, bmin_x, bmax_x, bmin_z, bmax_z) ||
        !col_build_grid(w, tris, &best)) {
        printf("COL: not enough RAM for collision (%luKB free)\n",
               (unsigned long)(free_ram / 1024));
        free(tris);
        free(w);
        return NULL;
    }
    free(tris);

    printf("COL: cell_size %.2f, grid %dx%d, %lu cells, %.1f tris/cell, %luKB (%luKB was free)\n",
           w->cell_size, w->gx, w->gz, (unsigned long)best.used,
           best.used ? (float)best.refs / (float)best.used : 0.0f,
           (unsigned long)(best.bytes / 1024), (unsigned long)(free_ram / 1024));
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
 * Gravity ray: D = (0, -1, 0) -- P and inv_det are cheap for this D
 * ================================================================ */
static inline int col_gravity_ray_tri(const ColTri* ct, shz_vec3_t origin,
                                      float max_t, float* out_t) {
    /* P = D x edge2 with D = (0, -1, 0) */
    float px = -ct->edge2.z, pz = ct->edge2.x;
    float det = ct->edge1.x * px + ct->edge1.z * pz;
    if (fabsf(det) < 1e-7f) return 0;
    float inv_det = 1.0f / det;

    shz_vec3_t s = shz_vec3_sub(origin, ct->v0);

    float u = (s.x * px + s.z * pz) * inv_det;
    if (u < 0.0f || u > 1.0f) return 0;

    shz_vec3_t Q = shz_vec3_cross(s, ct->edge1);

    float v = -Q.y * inv_det;
    if (v < 0.0f || u + v > 1.0f) return 0;

    float t = shz_dot6f(ct->edge2.x, ct->edge2.y, ct->edge2.z,
                        Q.x, Q.y, Q.z)
              * inv_det;

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
                const ColCell* cell = col_find_cell(w, gx, gz);
                if (!cell) continue;
                ColTriRef* refs = &w->tri_refs[cell->start];

                for (int i = 0; i < cell->count; i++) {
                    if (SHZ_LIKELY(i + 1 < cell->count))
                        col_prefetch_ref(refs[i + 1]);

                    ColTri ct;
                    col_tri_get(w, refs[i], &ct);
                    shz_vec3_t closest = col_closest_on_tri(&ct, resolved);
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
            const ColCell* cell = col_find_cell(w, gx, gz);
            if (!cell) continue;
            ColTriRef* refs = &w->tri_refs[cell->start];

            for (int i = 0; i < cell->count; i++) {
                if (SHZ_LIKELY(i + 1 < cell->count))
                    col_prefetch_ref(refs[i + 1]);

                ColTri ct;
                float t;
                col_tri_get(w, refs[i], &ct);
                if (col_gravity_ray_tri(&ct, origin, best_t, &t)) {
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
            const ColCell* cell = col_find_cell(w, gx, gz);
            if (!cell) continue;
            ColTriRef* refs = &w->tri_refs[cell->start];

            for (int i = 0; i < cell->count; i++) {
                if (SHZ_LIKELY(i + 1 < cell->count))
                    col_prefetch_ref(refs[i + 1]);

                ColTri ct;
                float t;
                col_tri_get(w, refs[i], &ct);
                if (col_ray_tri(&ct, org, dir, best_t, &t)) {
                    shz_vec3_t n = shz_vec3_cross(ct.edge1, ct.edge2);
                    float inv_len = shz_inv_sqrtf(shz_mag_sqr3f(n.x, n.y, n.z));
                    best_t = t;
                    result.hit = 1;
                    result.t = t;
                    result.normal = shz_vec3_init(n.x * inv_len, n.y * inv_len, n.z * inv_len);
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
    free(w->tri_refs);
    free(w->cells);
    free(w);
}

#endif /* COLLISION_IMPLEMENTATION */
#endif /* COLLISION_H */