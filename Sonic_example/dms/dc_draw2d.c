#include "dc_draw2d.h"
#include "dc_engine.h"
#include "font.h"

void dc_draw2d_init(void) {
    InitFont();
}

void dc_draw_text(const char* text, int x, int y, int size, uint32_t argb) {
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

    /* Convert ARGB uint32 to Color struct */
    Color c;
    c.a = (argb >> 24) & 0xFF;
    c.r = (argb >> 16) & 0xFF;
    c.g = (argb >>  8) & 0xFF;
    c.b =  argb        & 0xFF;

    DrawText(text, x, y, size, c);
}
