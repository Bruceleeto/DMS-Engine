/* dc_file: opening the game's files wherever they are. Assets are read over
 * dcload at /pc/ (dc-tool -m pc) or from the disc at /cd/; a path under one
 * that is not there is looked for under the other, so the same ELF runs
 * either way and a disc build needs no rebuild. */
#ifndef DC_FILE_H
#define DC_FILE_H

#include <kos/fs.h>
#include <stdio.h>
#include <string.h>

/* The same file under the other of /pc/ and /cd/; NULL if it is under neither */
static inline const char *dc_asset_alt(const char *path, char *buf, size_t n) {
  if (strncmp(path, "/pc/", 4) && strncmp(path, "/cd/", 4)) return NULL;
  snprintf(buf, n, "%s%s", path[1] == 'p' ? "/cd/" : "/pc/", path + 4);
  return buf;
}

static inline file_t dc_asset_open(const char *path, int mode) {
  file_t f = fs_open(path, mode);
  char alt[256];
  if (f < 0 && dc_asset_alt(path, alt, sizeof(alt))) f = fs_open(alt, mode);
  return f;
}

static inline FILE *dc_asset_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  char alt[256];
  if (!f && dc_asset_alt(path, alt, sizeof(alt))) f = fopen(alt, mode);
  return f;
}

#endif
