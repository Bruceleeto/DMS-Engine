#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An uncompressed PNG (stored deflate blocks): no zlib needed. */
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
    uint8_t b[4];
    put32(b, n); fwrite(b, 1, 4, f);
    uint32_t c = crc(0, (const uint8_t*)type, 4);
    fwrite(type, 1, 4, f);
    if (n) { fwrite(data, 1, n, f); c = crc(c, data, n); }
    put32(b, c); fwrite(b, 1, 4, f);
}
int write_png(const char* path, const uint8_t* rgb, int w, int h) {
    size_t row = (size_t)w * 3 + 1, raw_n = row * h;
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t* raw = malloc(raw_n);
    uint8_t* z = malloc(z_n);
    if (!raw || !z) { free(raw); free(z); return -1; }
    for (int y = 0; y < h; ++y) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    uint8_t* o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i += 65535) {
        size_t n = raw_n - i < 65535 ? raw_n - i : 65535;
        *o++ = i + n == raw_n;
        *o++ = n; *o++ = n >> 8; *o++ = ~n; *o++ = ~n >> 8;
        memcpy(o, raw + i, n); o += n;
        for (size_t k = 0; k < n; ++k) { a = (a + raw[i + k]) % 65521; b = (b + a) % 65521; }
    }
    put32(o, b << 16 | a); o += 4;
    FILE* f = fopen(path, "wb");
    if (f) {
        uint8_t ihdr[13] = {0};
        put32(ihdr, w); put32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 2;
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)(o - z));
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(z);
    return f ? 0 : -1;
}
