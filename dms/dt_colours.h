#ifndef DT_COLOURS_H
#define DT_COLOURS_H

/* Reads a .dt texture's colours back on the CPU, from the file's bytes still in
 * RAM. Used to know what colour a see-through texture turns light into. Only
 * the top mip level is read. Twiddled textures, compressed (VQ) or not, in
 * ARGB1555, RGB565 or ARGB4444. Anything else returns 0 and nothing is read.
 * Kept free of KOS so it can be checked on a PC. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pvrtex/file_dctex.h>

/* Interleaves the bits of x and y, y in the lowest: where the texel at (x, y)
 * sits in a twiddled square */
static inline uint32_t dt_twiddle(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for (uint32_t b = 0; b < 11; b++) {
        r |= ((y >> b) & 1u) << (2 * b);
        r |= ((x >> b) & 1u) << (2 * b + 1);
    }
    return r;
}

/* A twiddled rectangle is stored as squares of its short side, one after another */
static inline uint32_t dt_twiddle_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t s = w < h ? w : h;
    uint32_t block = w > h ? x / s : y / s;
    return block * s * s + dt_twiddle(x % s, y % s);
}

static inline uint32_t dt_to_argb(uint16_t p, unsigned fmt) {
    uint32_t a, r, g, b;
    switch (fmt) {
    case FDT_FMT_ARGB1555:
        a = (p & 0x8000) ? 255 : 0;
        r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        break;
    case FDT_FMT_RGB565:
        a = 255;
        r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        break;
    default: /* FDT_FMT_ARGB4444 */
        a = (p >> 12) & 15; r = (p >> 8) & 15; g = (p >> 4) & 15; b = p & 15;
        a *= 17; r *= 17; g *= 17; b *= 17;
        break;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Fills out[n * n] with the texture shrunk to n by n (nearest texel), as
 * 0xAARRGGBB, row by row from v = 0. The whole PVR size is covered, which is
 * what the vertices' u and v span. Returns 1 if it could be read. */
static inline int dt_read_colours(const void* file, size_t size, uint32_t* out, uint32_t n) {
    const fDtHeader* h = (const fDtHeader*)file;
    if (size < sizeof(fDtHeader) || !fDtFourccMatches(h)) return 0;
    unsigned fmt = fDtGetPixelFormat(h);
    if (fmt != FDT_FMT_ARGB1555 && fmt != FDT_FMT_RGB565 && fmt != FDT_FMT_ARGB4444) return 0;
    if (!fDtIsTwiddled(h) || fDtIsStrided(h)) return 0;

    uint32_t w = fDtGetPvrWidth(h), ht = fDtGetPvrHeight(h);
    const uint8_t* data = (const uint8_t*)file + fDtGetHeaderSize(h);
    const uint8_t* end = (const uint8_t*)file + size;
    int vq = fDtIsCompressed(h);
    int mip = fDtIsMipmapped(h);   /* mipmaps are square */

    const uint16_t* book = NULL;
    uint32_t book_skip = 0;        /* a small codebook's first index */
    const uint8_t* texels = data;
    if (vq) {
        uint32_t cb = fDtGetCodebookSizeBytes(h);
        book = (const uint16_t*)data;
        book_skip = 256 - cb / 8;
        texels = data + cb;
        /* The smaller levels come first: 1x1 takes one byte, then each
         * level of side d a byte per 2x2 */
        if (mip) {
            uint32_t off = 1;
            for (uint32_t d = 2; d < w; d <<= 1) off += (d / 2) * (d / 2);
            texels += off;
        }
    } else if (mip) {
        uint32_t off = 6;          /* 1x1 sits 6 bytes in */
        for (uint32_t d = 1; d < w; d <<= 1) off += d * d * 2;
        texels += off;
    }

    for (uint32_t j = 0; j < n; j++) {
        uint32_t y = (j * ht + ht / 2) / n;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t x = (i * w + w / 2) / n;
            uint16_t p;
            if (vq) {
                uint32_t idx_at = dt_twiddle_rect(x / 2, y / 2, w / 2, ht / 2);
                if (texels + idx_at >= end) return 0;
                uint32_t e = texels[idx_at];
                if (e < book_skip) return 0;
                /* An entry is 2x2 texels in twiddled order */
                const uint16_t* ent = book + (e - book_skip) * 4;
                p = ent[((x & 1) << 1) | (y & 1)];
            } else {
                uint32_t at = dt_twiddle_rect(x, y, w, ht) * 2;
                if (texels + at + 2 > end) return 0;
                memcpy(&p, texels + at, 2);
            }
            out[j * n + i] = dt_to_argb(p, fmt);
        }
    }
    return 1;
}

#endif /* DT_COLOURS_H */
