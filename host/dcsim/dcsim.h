/*
 * dcsim.h: dcsim's engine on the Dreamcast's heap.  Put in front of every
 * engine source file (-include, dc/dc.mk), so all its mallocs go to
 * dc/dcsim_malloc.c: the Dreamcast's own malloc on just the RAM the
 * Dreamcast has free.  What runs out there runs out here.
 */
#if !defined(DCSIM_H) && !defined(__ASSEMBLER__) && !defined(__cplusplus)
#define DCSIM_H

#include <stddef.h>

void *dcsim_malloc(size_t size);
void *dcsim_calloc(size_t n, size_t size);
void *dcsim_realloc(void *p, size_t size);
void *dcsim_memalign(size_t align, size_t size);
void dcsim_free(void *p);
// malloc's in use, and what's free (in the heap and above it)
void DCSim_Info(int *inuse, int *freebytes);
// called when the heap runs out, before malloc returns NULL
extern void (*dcsim_oomhook)(void);
// paints the stack below here, for how deep the game goes (dc/sys_dc.c)
void DCSim_PaintStack(void);

#define malloc		dcsim_malloc
#define calloc		dcsim_calloc
#define realloc		dcsim_realloc
#define memalign	dcsim_memalign
#define free		dcsim_free

#endif
