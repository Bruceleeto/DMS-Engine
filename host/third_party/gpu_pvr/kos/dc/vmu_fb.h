#ifndef __DC_VMU_FB_H
#define __DC_VMU_FB_H

/* No VMU here.  platform.c prints its per-second stats line to the VMU LCD;
 * send it to stdout instead so the numbers are still visible. */
#include <stdio.h>

#define vmu_printf(...) printf(__VA_ARGS__)
#endif
