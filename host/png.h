#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* RGB24 rows, w*3 bytes each, to an uncompressed PNG. 0 on success. */
int write_png(const char* path, const uint8_t* rgb, int w, int h);
#ifdef __cplusplus
}
#endif
