#pragma once
/* No memory cards on the host: maple_enum_type never finds one. */
#include <kos/fs.h>
#include <dc/vmu_pkg.h>
static inline int fs_vmu_set_header(file_t fd, vmu_pkg_t* pkg) { (void)fd; (void)pkg; return 0; }
static inline void vmu_set_icon(const char* icon) { (void)icon; }
