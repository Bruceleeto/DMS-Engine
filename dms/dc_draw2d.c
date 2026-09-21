#include "dc_draw2d.h"
#include "dc_engine.h"
#include "font.h"
#include "dc_draw.h"
#include "pvrtex.h"
#include <stdlib.h>
#include <string.h>

void dc_draw2d_init(void) {
    InitFont();
}

/* Text is kept until the end of the frame, so it can be asked for at any point
 * and still lands in the punch-through list after everything else. */
#define TEXT_QUEUE_MAX 64
#define TEXT_MAX_CHARS 64

typedef struct {
    char     text[TEXT_MAX_CHARS];
    int      x, y, size;
    uint32_t argb;
} TextEntry;

static TextEntry text_queue[TEXT_QUEUE_MAX];
static int       text_count;

void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb) {
    if (!text || text_count >= TEXT_QUEUE_MAX) return;
    TextEntry* e = &text_queue[text_count++];
    strncpy(e->text, text, TEXT_MAX_CHARS - 1);
    e->text[TEXT_MAX_CHARS - 1] = '\0';
    e->x = x; e->y = y; e->size = size;
    e->argb = argb;
}

void dc_draw2d_flush(void) {
    if (text_count == 0) return;
    pvr_dr_state_t* dr = dc_list_begin(PVR_LIST_PT_POLY);
    SetDrawingState(dr);

    /* Submit font texture header */
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY,
                     fontTexture.pvrformat,
                     fontTexture.width, fontTexture.height,
                     fontTexture.ptr, PVR_FILTER_NONE);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    for (int i = 0; i < text_count; i++) {
        const TextEntry* e = &text_queue[i];
        Color c;
        c.a = (e->argb >> 24) & 0xFF;
        c.r = (e->argb >> 16) & 0xFF;
        c.g = (e->argb >>  8) & 0xFF;
        c.b =  e->argb        & 0xFF;
        DrawText(e->text, e->x, e->y, e->size, c);
    }
    text_count = 0;
}

/* ---- Images ---- */

struct DCImage {
    dttex_info_t tex;
};

DCImage* dc_image_load(const char* filename) {
    DCImage* img = (DCImage*)calloc(1, sizeof(DCImage));
    if (!img) return NULL;
    if (!pvrtex_load(filename, &img->tex)) {
        printf("dc_image_load: cannot load %s\n", filename);
        free(img);
        return NULL;
    }
    return img;
}

void dc_image_free(DCImage* img) {
    if (!img) return;
    pvrtex_unload(&img->tex);
    free(img);
}

/* Depth is 1/w. Just in front of the PVR's own background plane (0.0001 in
 * KOS), and behind anything nearer than 5000 units. */
#define BACKGROUND_DEPTH 0.0002f

static void background_draw(void* user) {
    const DCImage* img = (const DCImage*)user;
    pvr_dr_state_t* dr = dc_dr_state();
    (void)dr;   /* this KOS's pvr_dr_target() does not use it */

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, img->tex.pvrformat,
                     img->tex.width, img->tex.height, img->tex.ptr,
                     PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_hdr_t* hdr = (pvr_poly_hdr_t*)pvr_dr_target(*dr);
    pvr_poly_compile(hdr, &cxt);
    pvr_dr_commit(hdr);

    static const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t* v = (pvr_vertex_t*)pvr_dr_target(*dr);
        v->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v->x = corner[i][0] * 640.0f;
        v->y = corner[i][1] * 480.0f;
        v->z = BACKGROUND_DEPTH;
        v->u = corner[i][0];
        v->v = corner[i][1];
        v->argb = 0xFFFFFFFF;
        v->oargb = 0;
        pvr_dr_commit(v);
    }
}

void dc_set_environment(const DCImage* img) {
    dc_model_set_environment(img ? &img->tex : NULL);
}

void dc_draw_background(const DCImage* img) {
    if (img) dc_draw_call(PVR_LIST_OP_POLY, background_draw, (void*)img);
}
