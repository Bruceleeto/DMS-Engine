#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nav.h"
#include "dms/dc_engine.h"
#include "dms/dc_file.h"

/* The eight cells round one, each the other way round from its neighbour in
 * the list (d ^ 1), the straight ones first */
static const int DX[8] = { 1, -1, 0, 0, 1, -1, -1, 1 };
static const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

#define AHEAD    6              /* cells along the way nav_steer looks */

/* The first floor flat enough to stand on, down from top */
static bool flat_floor(ColWorld* col, float x, float z, float top, float* y) {
    ColGroundHit g = col_ground(col, shz_vec3_init(x, top, z), top - col->min_y + 1.0f);
    if (!g.hit || g.normal.y < 0.7f) return false;
    *y = g.y;
    return true;
}

/* The cell at (x, z) on the grid, -1 off it or none there */
static int at(const Nav* nav, int x, int z) {
    if (x < 0 || z < 0 || x >= nav->w || z >= nav->h) return -1;
    const int32_t* t = nav->tiles[(z / NAV_TILE) * nav->tw + x / NAV_TILE];
    return t ? t[(z % NAV_TILE) * NAV_TILE + x % NAV_TILE] : -1;
}

/* A body walks from (a, its floor fa) to (b, fb) without being stopped: no
 * step between too high, and no wall anywhere along the way */
static bool walkable(const Nav* nav, ColWorld* col, shz_vec3_t ca, float fa, shz_vec3_t cb, float fb, float radius) {
    if (fabsf(fa - fb) > NAV_STEP) return false;
    float feet = fa < fb ? fa : fb;
    int steps = (int)(nav->cell * 1.5f / radius) + 1;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        shz_vec3_t p = shz_vec3_init(ca.x + (cb.x - ca.x) * t, feet + NAV_BODY_Y, ca.z + (cb.z - ca.z) * t);
        shz_vec3_t got = col_move_steps(col, p, radius, feet, NAV_STEP);
        float dx = got.x - p.x, dz = got.z - p.z;
        if (dx * dx + dz * dz > 0.05f * 0.05f) return false;
    }
    return true;
}

/* While it is built: what is known of each cell of a tile looked at */
typedef struct {
    float floor[NAV_TILE * NAV_TILE];   /* NAV_NONE: none, or not known yet */
    uint8_t known[NAV_TILE * NAV_TILE];
} Looked;

/* Room for one more walkable cell */
static bool grow(Nav* nav, int* room) {
    if (nav->n < *room) return true;
    int r = *room ? *room * 2 : 1024;
    void* a = realloc(nav->cx, r * sizeof(uint16_t));    if (a) nav->cx = a;
    void* b = realloc(nav->cz, r * sizeof(uint16_t));    if (b) nav->cz = b;
    void* c = realloc(nav->floor, r * sizeof(float));    if (c) nav->floor = c;
    void* d = realloc(nav->links, r);                    if (d) nav->links = d;
    if (!a || !b || !c || !d) return false;
    *room = r;
    return true;
}

int nav_build(Nav* nav, ColWorld* col, float cell, float radius, shz_vec3_t start) {
    memset(nav, 0, sizeof(*nav));
    nav->goal = -1;
    if (!col) return 0;
    nav->cell = cell;
    nav->x0 = col->min_x;
    nav->z0 = col->min_z;
    nav->w = (int)(col->gx * col->cell_size / cell) + 1;
    nav->h = (int)(col->gz * col->cell_size / cell) + 1;
    if (nav->w > 65535 || nav->h > 65535) return 0;
    nav->tw = (nav->w + NAV_TILE - 1) / NAV_TILE;
    nav->th = (nav->h + NAV_TILE - 1) / NAV_TILE;
    int tiles = nav->tw * nav->th;
    nav->tiles = calloc(tiles, sizeof(int32_t*));
    Looked** looked = calloc(tiles, sizeof(Looked*));
    if (!nav->tiles || !looked) { free(looked); nav_free(nav); return 0; }
    uint64_t t0 = dc_time_ms();
    int room = 0;
    bool ok = true;

    /* A cell's tile, and where in it */
    #define TILE_OF(x, z) ((z) / NAV_TILE * nav->tw + (x) / NAV_TILE)
    #define SLOT(x, z) (((z) % NAV_TILE) * NAV_TILE + (x) % NAV_TILE)

    /* Out from where you start, a cell at a time: only floor that can be
     * walked to is looked at. Each cell reached is numbered as it is, and
     * waits its turn in that order (the numbers are the queue). */
    int sx = (int)floorf((start.x - nav->x0) / cell), sz = (int)floorf((start.z - nav->z0) / cell);
    if (sx < 0 || sz < 0 || sx >= nav->w || sz >= nav->h) { free(looked); nav_free(nav); return 0; }
    float y;
    shz_vec3_t m0 = shz_vec3_init(nav->x0 + (sx + 0.5f) * cell, 0.0f, nav->z0 + (sz + 0.5f) * cell);
    if (!flat_floor(col, m0.x, m0.z, start.y + 1.0f, &y)) { free(looked); nav_free(nav); return 0; }

    /* Cell (x, z) numbered, with its floor fy */
    #define REACH(x, z, fy) do { \
        int t_ = TILE_OF(x, z); \
        if (!nav->tiles[t_]) { \
            nav->tiles[t_] = malloc(NAV_TILE * NAV_TILE * sizeof(int32_t)); \
            if (!nav->tiles[t_]) { ok = false; break; } \
            for (int k_ = 0; k_ < NAV_TILE * NAV_TILE; k_++) nav->tiles[t_][k_] = -1; \
        } \
        if (!grow(nav, &room)) { ok = false; break; } \
        nav->tiles[t_][SLOT(x, z)] = nav->n; \
        nav->cx[nav->n] = (uint16_t)(x); nav->cz[nav->n] = (uint16_t)(z); \
        nav->floor[nav->n] = (fy); nav->links[nav->n] = 0; \
        nav->n++; \
    } while (0)

    REACH(sx, sz, y);
    for (int c = 0; ok && c < nav->n; c++) {
        int x = nav->cx[c], z = nav->cz[c];
        for (int d = 0; d < 4 && ok; d++) {
            int nx = x + DX[d], nz = z + DZ[d];
            if (nx < 0 || nz < 0 || nx >= nav->w || nz >= nav->h) continue;
            int t = TILE_OF(nx, nz), s = SLOT(nx, nz);
            if (!looked[t]) {
                looked[t] = malloc(sizeof(Looked));
                if (!looked[t]) { ok = false; break; }
                memset(looked[t]->known, 0, sizeof(looked[t]->known));
            }
            Looked* L = looked[t];
            shz_vec3_t ca = nav_centre(nav, c);
            shz_vec3_t m = shz_vec3_init(nav->x0 + (nx + 0.5f) * cell, 0.0f, nav->z0 + (nz + 0.5f) * cell);
            if (!L->known[s]) {
                L->known[s] = 1;
                /* The floor nearest this one's, not a roof over it */
                L->floor[s] = flat_floor(col, m.x, m.z, nav->floor[c] + 2.0f, &y) ? y : NAV_NONE;
            }
            if (L->floor[s] == NAV_NONE || (nav->links[c] & (1 << d))) continue;
            m.y = L->floor[s];
            if (!walkable(nav, col, ca, ca.y, m, m.y, radius)) continue;
            int o = at(nav, nx, nz);
            if (o < 0) { REACH(nx, nz, m.y); if (!ok) break; o = nav->n - 1; }
            nav->links[c] |= 1 << d;
            nav->links[o] |= 1 << (d ^ 1);
        }
    }
    for (int t = 0; t < tiles; t++) free(looked[t]);
    free(looked);
    if (!ok) { printf("nav: out of memory\n"); nav_free(nav); return 0; }

    /* Slanted links, where both straight ways round the corner are open too
     * (no cutting through the end of a wall) */
    for (int c = 0; c < nav->n; c++) {
        int x = nav->cx[c], z = nav->cz[c];
        for (int d = 4; d < 8; d += 2) {
            int o = at(nav, x + DX[d], z + DZ[d]);
            int cx = at(nav, x + DX[d], z), cz = at(nav, x, z + DZ[d]);
            if (o < 0 || cx < 0 || cz < 0) continue;
            int to_cx = DX[d] > 0 ? 0 : 1, to_cz = DZ[d] > 0 ? 2 : 3;
            if (!(nav->links[c] & (1 << to_cx)) || !(nav->links[c] & (1 << to_cz)) ||
                !(nav->links[cx] & (1 << to_cz)) || !(nav->links[cz] & (1 << to_cx)))
                continue;
            if (!walkable(nav, col, nav_centre(nav, c), nav->floor[c], nav_centre(nav, o), nav->floor[o], radius))
                continue;
            nav->links[c] |= 1 << d;
            nav->links[o] |= 1 << (d ^ 1);
        }
    }

    nav->dist = malloc(nav->n * sizeof(uint16_t));
    nav->queue = malloc(nav->n * sizeof(int));
    nav->queued = calloc(nav->n, 1);
    if (!nav->dist || !nav->queue || !nav->queued) { nav_free(nav); return 0; }
    for (int c = 0; c < nav->n; c++) nav->dist[c] = NAV_FAR;
    int used = 0;
    for (int t = 0; t < tiles; t++) used += nav->tiles[t] != NULL;
    printf("nav: %dx%d cells of %.2f m, %d can be walked to, in %d tiles, %u KB (%u ms)\n", nav->w, nav->h,
           cell, nav->n, used,
           (unsigned)((tiles * sizeof(int32_t*) + used * NAV_TILE * NAV_TILE * sizeof(int32_t) +
                       nav->n * (2 * sizeof(uint16_t) + sizeof(float) + 1 + sizeof(uint16_t) + sizeof(int) + 1)) / 1024),
           (unsigned)(dc_time_ms() - t0));
    #undef REACH
    #undef TILE_OF
    #undef SLOT
    return nav->n;
}

void nav_free(Nav* nav) {
    if (nav->tiles)
        for (int t = 0; t < nav->tw * nav->th; t++) free(nav->tiles[t]);
    free(nav->tiles);
    free(nav->cx);
    free(nav->cz);
    free(nav->floor);
    free(nav->links);
    free(nav->dist);
    free(nav->queue);
    free(nav->queued);
    memset(nav, 0, sizeof(*nav));
    nav->goal = -1;
}

int nav_cell(const Nav* nav, shz_vec3_t p) {
    if (!nav->tiles) return -1;
    return at(nav, (int)floorf((p.x - nav->x0) / nav->cell), (int)floorf((p.z - nav->z0) / nav->cell));
}

shz_vec3_t nav_centre(const Nav* nav, int c) {
    return shz_vec3_init(nav->x0 + (nav->cx[c] + 0.5f) * nav->cell, nav->floor[c],
                         nav->z0 + (nav->cz[c] + 0.5f) * nav->cell);
}

/* The cell next to c the d'th way (there is a link that way) */
static int next(const Nav* nav, int c, int d) {
    return at(nav, nav->cx[c] + DX[d], nav->cz[c] + DZ[d]);
}

bool nav_flow(Nav* nav, shz_vec3_t goal) {
    int g = nav_cell(nav, goal);
    if (g < 0) return false;
    if (g == nav->goal) return true;
    nav->goal = g;
    int n = nav->n;
    for (int c = 0; c < n; c++) nav->dist[c] = NAV_FAR;

    /* Shortest ways out from the goal: a queue, a cell going back in when a
     * shorter way to it turns up. The links go both ways, so the way out
     * from the goal is the way back to it. */
    int head = 0, count = 1;
    nav->dist[g] = 0;
    nav->queue[0] = g;
    nav->queued[g] = 1;
    while (count) {
        int c = nav->queue[head];
        head = head + 1 == n ? 0 : head + 1;
        count--;
        nav->queued[c] = 0;
        uint8_t l = nav->links[c];
        for (int d = 0; d < 8; d++) {
            if (!(l & (1 << d))) continue;
            int o = next(nav, c, d);
            unsigned nd = nav->dist[c] + (d < 4 ? 2u : 3u);
            if (nd >= nav->dist[o]) continue;
            nav->dist[o] = (uint16_t)nd;
            /* Already waiting: it goes on with the shorter way when it comes up */
            if (nav->queued[o] || count >= n) continue;
            nav->queued[o] = 1;
            int tail = head + count;
            if (tail >= n) tail -= n;
            nav->queue[tail] = o;
            count++;
        }
    }
    return true;
}

/* Nothing between a and b at the height of a body, down its middle and
 * both its sides */
static bool clear(ColWorld* col, shz_vec3_t a, shz_vec3_t b, float radius) {
    float dx = b.x - a.x, dz = b.z - a.z;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-4f) return true;
    shz_vec3_t d = shz_vec3_init(dx / len, 0.0f, dz / len);
    /* Over the higher end and any step on the way */
    float y = (a.y > b.y ? a.y : b.y) + NAV_STEP + 0.1f;
    for (int k = -1; k <= 1; k++) {
        shz_vec3_t o = shz_vec3_init(a.x - d.z * radius * k, y, a.z + d.x * radius * k);
        if (col_raycast(col, o, d, len).hit) return false;
    }
    return true;
}

bool nav_steer(const Nav* nav, ColWorld* col, shz_vec3_t from, shz_vec3_t goal, float radius,
               shz_vec3_t* to) {
    int c = nav_cell(nav, from);
    if (c < 0 || nav->dist[c] == NAV_FAR) return false;

    /* Close, and nothing in between: straight there */
    float gx = goal.x - from.x, gz = goal.z - from.z;
    if (c == nav->goal || (gx * gx + gz * gz < 12.0f * 12.0f && fabsf(goal.y - from.y) < 1.0f &&
                           clear(col, from, goal, radius))) {
        *to = goal;
        return true;
    }

    /* Downhill a few cells, then the farthest of them it can see */
    int way[AHEAD];
    int n = 0;
    while (n < AHEAD && c != nav->goal) {
        int best = -1;
        unsigned bd = nav->dist[c];
        uint8_t l = nav->links[c];
        for (int d = 0; d < 8; d++) {
            if (!(l & (1 << d))) continue;
            int o = next(nav, c, d);
            if (nav->dist[o] < bd) { bd = nav->dist[o]; best = o; }
        }
        if (best < 0) break;
        way[n++] = c = best;
    }
    if (!n) return false;
    for (int i = n - 1; i > 0; i--) {
        shz_vec3_t p = nav_centre(nav, way[i]);
        if (clear(col, from, p, radius)) { *to = p; return true; }
    }
    *to = nav_centre(nav, way[0]);
    return true;
}

bool nav_random_spot(const Nav* nav, float min, float max, shz_vec3_t* out) {
    if (nav->goal < 0) return false;
    unsigned lo = (unsigned)(min / nav->cell * 2.0f), hi = (unsigned)(max / nav->cell * 2.0f);
    if (!nav->n) return false;
    for (int tries = 0; tries < 400; tries++) {
        int c = rand() % nav->n;
        unsigned d = nav->dist[c];
        if (d == NAV_FAR || d < lo || d > hi) continue;
        *out = nav_centre(nav, c);
        return true;
    }
    return false;
}

/* ---- In a file ----
 * Off a disc it is the size that costs (a CD reads about 120 KB a second in
 * Flycast), so it is kept small: a header (what it was built from, the
 * grid's size), then each tile that has floor (its number, and a bit for
 * each of its cells that is one), then each of those cells' links, then
 * their floors in 1/128 m over the lowest. The cells are numbered again in
 * that order, which nothing minds. Both ends are little endian. */

#define NAV_MAGIC 0x3256414Eu   /* "NAV2" */
#define NAV_Q     128.0f        /* floor steps a metre */
#define NAV_SLOTS (NAV_TILE * NAV_TILE)
#define NAV_TREC  (2 + NAV_SLOTS / 8)   /* a tile: its number, its bits */

typedef struct {
    uint32_t magic;
    int32_t  tris, gx, gz;      /* the collision it was built over */
    float    min_x, min_z, cell, radius;
    int32_t  sx, sz;            /* the cell it was walked out from */
    int32_t  w, h, n, used;
    float    floor_lo;
} NavFile;

static void nav_key(NavFile* f, ColWorld* col, float cell, float radius, shz_vec3_t start) {
    memset(f, 0, sizeof(*f));
    f->magic = NAV_MAGIC;
    f->tris = col->tri_count;
    f->gx = col->gx;
    f->gz = col->gz;
    f->min_x = col->min_x;
    f->min_z = col->min_z;
    f->cell = cell;
    f->radius = radius;
    f->sx = (int)floorf((start.x - col->min_x) / cell);
    f->sz = (int)floorf((start.z - col->min_z) / cell);
}

bool nav_save(const Nav* nav, ColWorld* col, float radius, shz_vec3_t start, const char* path) {
    if (!nav->n || !col) return false;
    NavFile f;
    nav_key(&f, col, nav->cell, radius, start);
    int tiles = nav->tw * nav->th;
    f.w = nav->w; f.h = nav->h; f.n = nav->n;
    f.floor_lo = nav->floor[0];
    float hi = f.floor_lo;
    for (int c = 0; c < nav->n; c++) {
        if (nav->floor[c] < f.floor_lo) f.floor_lo = nav->floor[c];
        if (nav->floor[c] > hi) hi = nav->floor[c];
    }
    if ((hi - f.floor_lo) * NAV_Q > 65535.0f || tiles > 65535) return false;
    for (int t = 0; t < tiles; t++) f.used += nav->tiles[t] != NULL;

    size_t n = nav->n, size = (size_t)f.used * NAV_TREC + n * 3;
    uint8_t* buf = calloc(size, 1);
    if (!buf) return false;
    uint8_t *tp = buf, *lp = buf + (size_t)f.used * NAV_TREC, *fp16 = lp + n;
    for (int t = 0; t < tiles; t++) {
        const int32_t* cells = nav->tiles[t];
        if (!cells) continue;
        tp[0] = t & 0xFF; tp[1] = t >> 8;
        for (int s = 0; s < NAV_SLOTS; s++) {
            int c = cells[s];
            if (c < 0) continue;
            tp[2 + s / 8] |= 1 << (s % 8);
            *lp++ = nav->links[c];
            unsigned q = (unsigned)((nav->floor[c] - f.floor_lo) * NAV_Q + 0.5f);
            *fp16++ = q & 0xFF; *fp16++ = q >> 8;
        }
        tp += NAV_TREC;
    }
    FILE* fp = fopen(path, "wb");
    bool ok = fp && fwrite(&f, sizeof(f), 1, fp) == 1 && fwrite(buf, 1, size, fp) == size;
    free(buf);
    if (!fp) return false;
    if (fclose(fp) || !ok) { remove(path); return false; }
    return true;
}

int nav_load(Nav* nav, ColWorld* col, float cell, float radius, shz_vec3_t start, const char* path) {
    memset(nav, 0, sizeof(*nav));
    nav->goal = -1;
    if (!col) return 0;
    FILE* fp = dc_asset_fopen(path, "rb");
    if (!fp) return 0;
    uint64_t t0 = dc_time_ms();
    NavFile f, want;
    nav_key(&want, col, cell, radius, start);
    /* The collision is built the same on both, but its sums may not come out
     * to the last bit the same on the SH4 */
    if (fread(&f, sizeof(f), 1, fp) != 1 || f.magic != want.magic || f.tris != want.tris || f.gx != want.gx ||
        f.gz != want.gz || fabsf(f.min_x - want.min_x) > 1e-3f || fabsf(f.min_z - want.min_z) > 1e-3f ||
        f.cell != want.cell || f.radius != want.radius || f.sx != want.sx || f.sz != want.sz ||
        f.n <= 0 || f.w <= 0 || f.h <= 0 || f.w > 65535 || f.h > 65535 || f.used <= 0) {
        fclose(fp);
        return 0;
    }
    nav->w = f.w; nav->h = f.h; nav->n = f.n;
    nav->tw = (f.w + NAV_TILE - 1) / NAV_TILE;
    nav->th = (f.h + NAV_TILE - 1) / NAV_TILE;
    nav->cell = cell;
    nav->x0 = col->min_x;
    nav->z0 = col->min_z;
    int tiles = nav->tw * nav->th;
    size_t n = f.n, size = (size_t)f.used * NAV_TREC + n * 3;
    uint8_t* buf = malloc(size);
    bool ok = buf && fread(buf, 1, size, fp) == size;
    fclose(fp);
    ok = ok && (nav->tiles = calloc(tiles, sizeof(int32_t*))) &&
         (nav->cx = malloc(n * sizeof(uint16_t))) && (nav->cz = malloc(n * sizeof(uint16_t))) &&
         (nav->floor = malloc(n * sizeof(float))) && (nav->links = malloc(n)) &&
         (nav->dist = malloc(n * sizeof(uint16_t))) && (nav->queue = malloc(n * sizeof(int))) &&
         (nav->queued = calloc(n, 1));

    /* Each tile's cells, numbered in turn */
    const uint8_t *tp = buf, *lp = buf + (size_t)f.used * NAV_TREC, *fp16 = lp + n;
    size_t c = 0;
    for (int k = 0; ok && k < f.used; k++, tp += NAV_TREC) {
        int t = tp[0] | tp[1] << 8;
        if (t >= tiles || nav->tiles[t] || !(nav->tiles[t] = malloc(NAV_SLOTS * sizeof(int32_t)))) { ok = false; break; }
        int bx = t % nav->tw * NAV_TILE, bz = t / nav->tw * NAV_TILE;
        for (int s = 0; s < NAV_SLOTS; s++) {
            if (!(tp[2 + s / 8] & (1 << (s % 8)))) { nav->tiles[t][s] = -1; continue; }
            if (c == n) { ok = false; break; }
            nav->tiles[t][s] = (int32_t)c;
            nav->cx[c] = (uint16_t)(bx + s % NAV_TILE);
            nav->cz[c] = (uint16_t)(bz + s / NAV_TILE);
            nav->links[c] = lp[c];
            nav->floor[c] = f.floor_lo + (fp16[c * 2] | fp16[c * 2 + 1] << 8) / NAV_Q;
            nav->dist[c] = NAV_FAR;
            c++;
        }
    }
    free(buf);
    if (!ok || c != n) { nav_free(nav); return 0; }
    printf("nav: %d cells from %s (%u ms)\n", nav->n, path, (unsigned)(dc_time_ms() - t0));
    return nav->n;
}
