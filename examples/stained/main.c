#include <kos.h>
#include <sh4zam/shz_sh4zam.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dms/dc_engine.h"
#include "dms/dc_input.h"
#include "dms/dc_camera.h"
#include "dms/dc_model.h"
#include "dms/dc_draw.h"
#include "dms/dc_draw2d.h"
#include "dms/dc_debug.h"

/* Where assets are read from: /pc/ over dcload, /cd/ when built with make disc */
#ifndef ASSETS
#define ASSETS "/pc/"
#endif

/* Katana's stained glass sample (k2Staind). A lamp behind a wall shines
 * through a stained glass window onto the floor. As Katana did it: every frame
 * each floor vertex looks along the line to the lamp. Through the glass it
 * takes the glass's colour there, through the wall it is in shadow, past the
 * wall it is lit. dc_model_recolour() sets the colours, and the hardware
 * multiplies them by the floor's texture. */

#define FLOOR_MATERIAL "Material__1"   /* the floor, and the wall wears it too */
#define GLASS_MATERIAL "Material__2"
#define LAMP_MATERIAL  "Material__4"

#define LIT    250      /* Katana's light and shadow */
#define SHADE  50
#define GLASS_N 64      /* the glass is read at this size */

/* The window, worked out once from the model */
typedef struct {
    float n[3], d;              /* its plane: n.p + d */
    float U[3], u0, V[3], v0;   /* where on a triangle: b1 = U.p + u0, b2 = V.p + v0 */
    float tu, tv, tu1, tv1, tu2, tv2;
} Pane;

typedef struct {
    Pane     pane[2];
    int      panes;
    int      a0, a1;            /* the two axes along the wall */
    float    lo[2], hi[2];      /* the wall's extent along them */
    uint32_t glass[GLASS_N * GLASS_N];
    shz_vec3_t lamp;
} Window;

static Window win;

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* A triangle of the glass: its plane, and what turns a point on it into how
 * far it is towards the second and third corners */
static void pane_make(Pane* p, const DMSVertex* a, const DMSVertex* b, const DMSVertex* c) {
    float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
    float e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
    float o[3] = { a->x, a->y, a->z };
    p->n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    p->n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    p->n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    float nl = sqrtf(dot3(p->n, p->n));
    for (int k = 0; k < 3; k++) p->n[k] /= nl;
    p->d = -dot3(p->n, o);
    float d00 = dot3(e1, e1), d01 = dot3(e1, e2), d11 = dot3(e2, e2);
    float inv = 1.0f / (d00 * d11 - d01 * d01);
    for (int k = 0; k < 3; k++) {
        p->U[k] = (d11 * e1[k] - d01 * e2[k]) * inv;
        p->V[k] = (d00 * e2[k] - d01 * e1[k]) * inv;
    }
    p->u0 = -dot3(p->U, o);
    p->v0 = -dot3(p->V, o);
    p->tu = a->u; p->tv = a->v;
    p->tu1 = b->u - a->u; p->tv1 = b->v - a->v;
    p->tu2 = c->u - a->u; p->tv2 = c->v - a->v;
}

static int window_setup(DMSModel* model) {
    const DMSMesh* glass = NULL;
    const DMSMesh* wall = NULL;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        const DMSMesh* mesh = &model->meshes[m];
        if (!strcmp(model->material_names[m], GLASS_MATERIAL)) glass = mesh;
        /* The floor and the wall share a material: the wall is the small one */
        if (!strcmp(model->material_names[m], FLOOR_MATERIAL) &&
            (!wall || mesh->vertex_count < wall->vertex_count)) wall = mesh;
    }
    if (!glass || !wall || glass->vertex_count < 3) return 0;

    const DMSVertex* v = glass->vertices;
    win.panes = 0;
    for (uint32_t i = 2; i < glass->vertex_count && win.panes < 2; i++) {
        if (v[i - 2].flags == PVR_CMD_VERTEX_EOL || v[i - 1].flags == PVR_CMD_VERTEX_EOL) continue;
        pane_make(&win.pane[win.panes++], &v[i - 2], &v[i - 1], &v[i]);
    }

    /* The wall runs along the two axes the glass faces least */
    const float* n = win.pane[0].n;
    int face = fabsf(n[0]) > fabsf(n[1]) ? (fabsf(n[0]) > fabsf(n[2]) ? 0 : 2)
                                         : (fabsf(n[1]) > fabsf(n[2]) ? 1 : 2);
    win.a0 = face == 0 ? 1 : 0;
    win.a1 = face == 2 ? 1 : 2;
    win.lo[0] = wall->bound_min[win.a0]; win.hi[0] = wall->bound_max[win.a0];
    win.lo[1] = wall->bound_min[win.a1]; win.hi[1] = wall->bound_max[win.a1];

    return dc_model_texture_colours(model, GLASS_MATERIAL, win.glass, GLASS_N);
}

static uint32_t grey(uint32_t g) { return (g << 16) | (g << 8) | g; }

/* One floor vertex: what reaches it from the lamp */
static uint32_t floor_colour(shz_vec3_t pos, void* user) {
    (void)user;
    float p[3] = { pos.x, pos.y, pos.z };
    float L[3] = { win.lamp.x, win.lamp.y, win.lamp.z };
    const Pane* g = &win.pane[0];
    float dp = dot3(g->n, p) + g->d, dl = dot3(g->n, L) + g->d;
    if ((dp > 0.0f) == (dl > 0.0f)) return grey(LIT);   /* on the lamp's side */

    float s = dp / (dp - dl);
    float x[3] = { p[0] + (L[0] - p[0]) * s, p[1] + (L[1] - p[1]) * s, p[2] + (L[2] - p[2]) * s };

    for (int i = 0; i < win.panes; i++) {
        const Pane* w = &win.pane[i];
        float b1 = dot3(w->U, x) + w->u0, b2 = dot3(w->V, x) + w->v0;
        if (b1 < -0.001f || b2 < -0.001f || b1 + b2 > 1.002f) continue;
        float u = w->tu + w->tu1 * b1 + w->tu2 * b2;
        float v = w->tv + w->tv1 * b1 + w->tv2 * b2;
        uint32_t tx = (uint32_t)(int)floorf(u * GLASS_N) & (GLASS_N - 1);
        uint32_t ty = (uint32_t)(int)floorf(v * GLASS_N) & (GLASS_N - 1);
        uint32_t c = win.glass[ty * GLASS_N + tx];
        return ((((c >> 16) & 0xff) * LIT / 255) << 16) |
               ((((c >>  8) & 0xff) * LIT / 255) << 8) | ((c & 0xff) * LIT / 255);
    }

    /* Not the glass: the wall, if the line goes through where the wall is */
    if (x[win.a0] > win.lo[0] && x[win.a0] < win.hi[0] &&
        x[win.a1] > win.lo[1] && x[win.a1] < win.hi[1]) return grey(SHADE);
    return grey(LIT);
}

#define LOOK_SPEED 1.5f
#define ZOOM_SPEED 120.0f
#define ZOOM_MIN   80.0f
#define ZOOM_MAX   450.0f

static bool check_exit(void) {
    const DCInput* inp = dc_input_get(0);
    return inp && (inp->buttons & CONT_START);
}

/* Where the middle of the meshes wearing a material is now, following the
 * bone that moves them */
static shz_vec3_t part_now(const DMSModel* model, const char* material) {
    DCBounds b = dc_model_bounds(model, material);
    shz_vec3_t c = shz_vec3_init((b.min.x + b.max.x) * 0.5f, (b.min.y + b.max.y) * 0.5f,
                                 (b.min.z + b.max.z) * 0.5f);
    if (!model->skeleton || !model->material_names) return c;
    for (uint32_t m = 0; m < model->mesh_count; m++) {
        if (strcmp(model->material_names[m], material) || !model->meshes[m].vertex_count)
            continue;
        const shz_mat4x4_t* s = &model->skeleton->bones[model->meshes[m].vertices[0].pad].skinMatrix;
        return shz_vec3_init(
            s->elem2D[0][0] * c.x + s->elem2D[1][0] * c.y + s->elem2D[2][0] * c.z + s->elem2D[3][0],
            s->elem2D[0][1] * c.x + s->elem2D[1][1] * c.y + s->elem2D[2][1] * c.z + s->elem2D[3][1],
            s->elem2D[0][2] * c.x + s->elem2D[1][2] * c.y + s->elem2D[2][2] * c.z + s->elem2D[3][2]);
    }
    return c;
}

int main(int argc, char* argv[]) {
    dc_init((DCInitParams){ .vram_size = 1024 * 1024 });
    dc_draw2d_init();
    dc_debug_init();
    dc_set_clear_color(0xFF101018);

    DMSModel* model = dc_model_load(ASSETS "stained/staind.dms");
    if (!model) { dc_shutdown(); return 1; }

    if (!window_setup(model)) printf("STAINED: could not find the window\n");

    DCBounds all = dc_model_bounds(model, NULL);
    shz_vec3_t look = shz_vec3_init((all.min.x + all.max.x) * 0.5f, 20.0f,
                                    (all.min.z + all.max.z) * 0.5f);

    DCCamera camera;
    dc_camera_init(&camera);
    camera.yaw = 2.6f;
    camera.pitch = 0.45f;

    printf("STAINED: A stops the lamp, triggers zoom, stick turns, Start exits\n");

    float distance = 260.0f;
    bool moving = true;

    while (!check_exit()) {
        dc_frame_begin();
        float dt = dc_delta_time();
        const DCInput* inp = dc_input_get(0);

        if (inp) {
            if (inp->ltrig > 0) distance -= ZOOM_SPEED * dt;
            if (inp->rtrig > 0) distance += ZOOM_SPEED * dt;
            if (distance < ZOOM_MIN) distance = ZOOM_MIN;
            if (distance > ZOOM_MAX) distance = ZOOM_MAX;
            if (dc_input_pressed(inp, CONT_A)) moving = !moving;
        }
        dc_model_animate(model, moving ? dt : 0.0f);
        dc_camera_orbit(&camera, look, distance, inp, LOOK_SPEED, dt);
        dc_camera_update(&camera);

        win.lamp = part_now(model, LAMP_MATERIAL);
        dc_model_recolour(model, FLOOR_MATERIAL, floor_colour, NULL);

        dc_set_camera(&camera);
        dc_draw(model, shz_vec3_init(0.0f, 0.0f, 0.0f));

        dc_debug_stats();
        dc_frame_end();
    }

    dc_model_free(model);
    dc_shutdown();
    return 0;
}
