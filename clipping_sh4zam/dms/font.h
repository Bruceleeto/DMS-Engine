#ifndef FONT_H
#define FONT_H

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvrtex.h"
#include "font_data.c"

#define DEFAULT_FONT_SIZE 10

typedef struct { float x, y; } Vector2;

typedef struct { unsigned char r, g, b, a; } Color;

#define CLITERAL(type) (type)
#define LIGHTGRAY  CLITERAL(Color){ 200, 200, 200, 255 }
#define GRAY       CLITERAL(Color){ 130, 130, 130, 255 }
#define DARKGRAY   CLITERAL(Color){ 80, 80, 80, 255 }
#define YELLOW     CLITERAL(Color){ 253, 249, 0, 255 }
#define GOLD       CLITERAL(Color){ 255, 203, 0, 255 }
#define ORANGE     CLITERAL(Color){ 255, 161, 0, 255 }
#define PINK       CLITERAL(Color){ 255, 109, 194, 255 }
#define RED        CLITERAL(Color){ 230, 41, 55, 255 }
#define MAROON     CLITERAL(Color){ 190, 33, 55, 255 }
#define GREEN      CLITERAL(Color){ 0, 228, 48, 255 }
#define LIME       CLITERAL(Color){ 0, 158, 47, 255 }
#define DARKGREEN  CLITERAL(Color){ 0, 117, 44, 255 }
#define SKYBLUE    CLITERAL(Color){ 102, 191, 255, 255 }
#define BLUE       CLITERAL(Color){ 0, 121, 241, 255 }
#define DARKBLUE   CLITERAL(Color){ 0, 82, 172, 255 }
#define PURPLE     CLITERAL(Color){ 200, 122, 255, 255 }
#define VIOLET     CLITERAL(Color){ 135, 60, 190, 255 }
#define DARKPURPLE CLITERAL(Color){ 112, 31, 126, 255 }
#define BEIGE      CLITERAL(Color){ 211, 176, 131, 255 }
#define BROWN      CLITERAL(Color){ 127, 106, 79, 255 }
#define DARKBROWN  CLITERAL(Color){ 76, 63, 47, 255 }
#define WHITE      CLITERAL(Color){ 255, 255, 255, 255 }
#define BLACK      CLITERAL(Color){ 0, 0, 0, 255 }
#define BLANK      CLITERAL(Color){ 0, 0, 0, 0 }
#define MAGENTA    CLITERAL(Color){ 255, 0, 255, 255 }
#define RAYWHITE   CLITERAL(Color){ 245, 245, 245, 255 }

static const int charWidths[224] = { 
    3, 1, 4, 6, 5, 7, 6, 2, 3, 3, 5, 5, 2, 4, 1, 7, 5, 2, 5, 5, 5, 5, 5, 5, 5, 5, 1, 1, 3, 4, 3, 6,
    7, 6, 6, 6, 6, 6, 6, 6, 6, 3, 5, 6, 5, 7, 6, 6, 6, 6, 6, 6, 7, 6, 7, 7, 6, 6, 6, 2, 7, 2, 3, 5,
    2, 5, 5, 5, 5, 5, 4, 5, 5, 1, 2, 5, 2, 5, 5, 5, 5, 5, 5, 5, 4, 5, 5, 5, 5, 5, 5, 3, 1, 3, 4, 4,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 5, 5, 5, 7, 1, 5, 3, 7, 3, 5, 4, 1, 7, 4, 3, 5, 3, 3, 2, 5, 6, 1, 2, 2, 3, 5, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 7, 6, 6, 6, 6, 6, 3, 3, 3, 3, 7, 6, 6, 6, 6, 6, 6, 5, 6, 6, 6, 6, 6, 6, 4, 6,
    5, 5, 5, 5, 5, 5, 9, 5, 5, 5, 5, 5, 2, 2, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 3, 5 
};

// Precomputed UVs and width
typedef struct {
    float u1, v1, u2, v2;
    int width;
} CharInfo;

static CharInfo charInfo[224];
static dttex_info_t fontTexture;
static pvr_dr_state_t* currentDrState = NULL;
static int fontLoaded = 0;

static uint32_t ColorToPVR(Color color) {
    return (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
}

static void BuildCharTable(void) {
    const float inv = 1.0f / 128.0f;
    int x = 1, y = 1;
    
    for (int i = 0; i < 224; i++) {
        int w = charWidths[i];
        if (x + w + 1 > 128) { 
            x = 1; 
            y += DEFAULT_FONT_SIZE + 1; 
        }
        
        charInfo[i].u1 = x * inv;
        charInfo[i].v1 = y * inv;
        charInfo[i].u2 = (x + w) * inv;
        charInfo[i].v2 = (y + DEFAULT_FONT_SIZE) * inv;
        charInfo[i].width = w;
        
        x += w + 1;
    }
}


static int pvrtex_load_mem(const unsigned char *data, unsigned int len, dttex_info_t *texinfo) {
    memcpy(&texinfo->hdr, data, sizeof(dt_header_t));
    size_t hdr_size = (1 + texinfo->hdr.header_size) << 5;
    size_t tdatasize = texinfo->hdr.chunk_size - hdr_size;

    texinfo->flags.compressed = fDtIsCompressed(&texinfo->hdr);
    texinfo->flags.mipmapped  = fDtIsMipmapped(&texinfo->hdr);
    texinfo->flags.palettised = fDtIsPalettized(&texinfo->hdr);
    texinfo->flags.strided    = fDtIsStrided(&texinfo->hdr);
    texinfo->flags.twiddled   = fDtIsTwiddled(&texinfo->hdr);
    texinfo->width  = fDtGetPvrWidth(&texinfo->hdr);
    texinfo->height = fDtGetPvrHeight(&texinfo->hdr);
    texinfo->pvrformat = texinfo->hdr.pvr_type & 0xFFC00000;

    texinfo->ptr = pvr_mem_malloc(tdatasize);
    if (!texinfo->ptr) return 0;

    pvr_txr_load(data + hdr_size, texinfo->ptr, tdatasize);
    return 1;
}

static int InitFont(void) {
    if (!pvrtex_load_mem(font_dt, font_dt_len, &fontTexture)) {
        printf("Failed to load embedded font\n");
        return 0;
    }
    BuildCharTable();
    fontLoaded = 1;
    return 1;
}

static void SetDrawingState(pvr_dr_state_t* state) { 
    currentDrState = state; 
}

static void DrawText(const char* text, int posX, int posY, int fontSize, Color color) {
    if (!currentDrState || !fontLoaded) return;
    
    const float scale = (float)fontSize / DEFAULT_FONT_SIZE;
    const float h = DEFAULT_FONT_SIZE * scale;
    const uint32_t pvrColor = ColorToPVR(color);
    const float z = 1.0f / 0.1f;  /* always in front of 3D geo */
    
    float currentX = (float)posX;
    float currentY = (float)posY;
    
    while (*text) {
        if (*text == '\n') { 
            currentX = (float)posX; 
            currentY += fontSize + 2 * scale;
            text++;
            continue;
        }
        
        int index = *text - 32;
        if (index < 0 || index >= 224) {
            text++;
            continue;
        }
        
        const CharInfo* ci = &charInfo[index];
        float w = ci->width * scale;
        
        pvr_vertex_t *vert;
        
        vert = (pvr_vertex_t*)pvr_dr_target(*currentDrState);
        vert->flags = PVR_CMD_VERTEX;
        vert->x = currentX; vert->y = currentY; vert->z = z;
        vert->u = ci->u1; vert->v = ci->v1;
        vert->argb = pvrColor; vert->oargb = 0;
        pvr_dr_commit(vert);
        
        vert = (pvr_vertex_t*)pvr_dr_target(*currentDrState);
        vert->flags = PVR_CMD_VERTEX;
        vert->x = currentX + w; vert->y = currentY; vert->z = z;
        vert->u = ci->u2; vert->v = ci->v1;
        vert->argb = pvrColor; vert->oargb = 0;
        pvr_dr_commit(vert);
        
        vert = (pvr_vertex_t*)pvr_dr_target(*currentDrState);
        vert->flags = PVR_CMD_VERTEX;
        vert->x = currentX; vert->y = currentY + h; vert->z = z;
        vert->u = ci->u1; vert->v = ci->v2;
        vert->argb = pvrColor; vert->oargb = 0;
        pvr_dr_commit(vert);
        
        vert = (pvr_vertex_t*)pvr_dr_target(*currentDrState);
        vert->flags = PVR_CMD_VERTEX_EOL;
        vert->x = currentX + w; vert->y = currentY + h; vert->z = z;
        vert->u = ci->u2; vert->v = ci->v2;
        vert->argb = pvrColor; vert->oargb = 0;
        pvr_dr_commit(vert);
        
        currentX += (ci->width + 2) * scale;
        text++;
    }
}

#endif