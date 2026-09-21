#include "dc_draw.h"
#include "dc_draw2d.h"
#include "dc_engine.h"
#include <string.h>
#include <stdio.h>

#define DRAW_QUEUE_MAX 256

typedef enum { ENTRY_MODEL, ENTRY_CALL } EntryKind;

/* One queued draw. A struct, so lights and effects can be added to it later
 * without the calls that fill it changing. */
typedef struct {
    EntryKind       kind;
    DMSModel*       model;
    const DCCamera* cam;
    shz_vec3_t      pos;
    float           scale;
    float           yaw;
    bool            has_rot;
    float           rot[9];
    int             call_list;
    void          (*call_fn)(void* user);
    void*           call_user;
} DrawEntry;

static DrawEntry       queue[DRAW_QUEUE_MAX];
static int             queue_count;
static const DCCamera* current_cam;

static DrawEntry* queue_push(void) {
    if (queue_count >= DRAW_QUEUE_MAX) {
        static bool warned;
        if (!warned) {
            printf("dc_draw: more than %d draws in a frame, the rest are dropped\n",
                   DRAW_QUEUE_MAX);
            warned = true;
        }
        return NULL;
    }
    return &queue[queue_count++];
}

static DrawEntry* queue_model(DMSModel* model, shz_vec3_t pos, float scale) {
    if (!model) return NULL;
    if (!current_cam) {
        static bool warned;
        if (!warned) {
            printf("dc_draw: call dc_set_camera() first\n");
            warned = true;
        }
        return NULL;
    }
    DrawEntry* e = queue_push();
    if (!e) return NULL;
    e->kind = ENTRY_MODEL;
    e->model = model;
    e->cam = current_cam;
    e->pos = pos;
    e->scale = scale;
    e->yaw = 0.0f;
    e->has_rot = false;
    return e;
}

void dc_set_camera(const DCCamera* cam) {
    current_cam = cam;
}

void dc_draw(DMSModel* model, shz_vec3_t pos) {
    queue_model(model, pos, 1.0f);
}

void dc_draw_ex(DMSModel* model, const DCDrawOpts* opts) {
    if (!opts) return;
    DrawEntry* e = queue_model(model, opts->pos, opts->scale != 0.0f ? opts->scale : 1.0f);
    if (!e) return;
    e->yaw = opts->yaw;
    if (opts->rot) {
        e->has_rot = true;
        memcpy(e->rot, opts->rot, sizeof(e->rot));
    }
}

void dc_draw_call(int pvr_list, void (*fn)(void* user), void* user) {
    if (!fn) return;
    DrawEntry* e = queue_push();
    if (!e) return;
    e->kind = ENTRY_CALL;
    e->call_list = pvr_list;
    e->call_fn = fn;
    e->call_user = user;
}

/* Does the model have anything for this list? Saves a walk over its blocks. */
static bool model_uses_list(const DMSModel* model, int pvr_list) {
    if (pvr_list == PVR_LIST_OP_POLY) return model->opaque_count != 0;
    if (pvr_list == PVR_LIST_PT_POLY) return model->cutout_count != 0;
    /* Reflections of solid metallic meshes are drawn in the TR list */
    return model->transparent_count != 0 || model->metallic_count != 0;
}

void dc_draw_flush(void) {
    static const int lists[3] = { PVR_LIST_OP_POLY, PVR_LIST_TR_POLY, PVR_LIST_PT_POLY };

    for (int l = 0; l < 3; l++) {
        for (int i = 0; i < queue_count; i++) {
            const DrawEntry* e = &queue[i];
            if (e->kind == ENTRY_CALL) {
                if (e->call_list != lists[l]) continue;
                dc_list_begin(lists[l]);
                e->call_fn(e->call_user);
            } else {
                if (model_uses_list(e->model, lists[l])) {
                    if (e->has_rot)
                        dc_model_draw_list_oriented(e->model, e->pos, e->scale, e->rot,
                                                    e->cam, lists[l]);
                    else
                        dc_model_draw_list_rotated(e->model, e->pos, e->scale, e->yaw,
                                                   e->cam, lists[l]);
                }
            }
        }
    }
    queue_count = 0;
}
