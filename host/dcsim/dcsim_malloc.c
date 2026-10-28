/*
 * dcsim_malloc.c: the Dreamcast's malloc (newlib's, mallocr.inc) on
 * an arena the size of the RAM it has free: from the end of the Dreamcast
 * build's image (DCSIM_HEAP_START, the .elf's _end) to the top of its RAM
 * less the kernel stack, as KallistiOS gives it out (mm_sbrk).  Less
 * DCSIM_KOS_BYTES, what KallistiOS has malloced before the engine starts.
 * See dcsim.h.
 *
 * DCSIM_MEGS=<n> in the environment is the RAM: 32 by default (a dev
 * kit's), so a build over a retail Dreamcast's 16 still runs and shows by
 * how much. It warns once when it passes 16.
 */
#undef malloc
#undef calloc
#undef realloc
#undef memalign
#undef free

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_MEM_TOP	memtop
#define RETAIL		(16ul * 1024 * 1024)	// a retail Dreamcast's RAM
static uintptr_t memtop;

static unsigned char *arena;	// where DCSIM_HEAP_START is
static uintptr_t sbrkbase;		// as a Dreamcast address
static uintptr_t sbrkpeak;

void (*dcsim_oomhook)(void);

static unsigned long DCSim_LargestFree(void);
static unsigned long DCSim_FreeBytes(void);

static void *dcsim_sbrk(ptrdiff_t increment)
{
	uintptr_t base = sbrkbase, newbase;

	increment = (increment + 3) & ~3;
	newbase = base + increment;
	if (newbase >= DC_MEM_TOP)
	{
		printf("Out of memory. Requested sbrk_base 0x%x, was 0x%x, diff %d\n", (unsigned)newbase, (unsigned)base, (int)increment);
		// what the Dreamcast doesn't say: if it's in pieces
		printf("dcsim: %u K free in all, at most %u K of it in one piece\n",
			(unsigned)(DCSim_FreeBytes() / 1024), (unsigned)(DCSim_LargestFree() / 1024));
		if (dcsim_oomhook)
			dcsim_oomhook();
		return (void *)-1;
	}
	sbrkbase = newbase;
	if (sbrkpeak < newbase)
		sbrkpeak = newbase;
	static int over;
	if (!over && newbase + 64 * 1024 - 0x8c000000u > RETAIL)
	{
		over = 1;
		printf("dcsim: WARNING past the 16 MB of a retail Dreamcast: this would not run there\n");
	}
	// how high the heap's been, each 256K more: what it takes, with DCSIM_MEGS
	// high enough not to run out
	if ((newbase - DCSIM_HEAP_START) >> 18 != (base - DCSIM_HEAP_START) >> 18)
		printf("dcsim: heap top %u K (%u K into a %u MB Dreamcast)\n", (unsigned)((newbase - DCSIM_HEAP_START) / 1024), (unsigned)((newbase - 0x8c000000u) / 1024), (unsigned)((DC_MEM_TOP + 64 * 1024 - 0x8c000000u) >> 20));
	return arena + (base - DCSIM_HEAP_START);
}

// the Dreamcast's malloc, all its names dl_
#define malloc				dl_malloc
#define calloc				dl_calloc
#define realloc				dl_realloc
#define memalign			dl_memalign
#define free				dl_free
#define cfree				dl_cfree
#define valloc				dl_valloc
#define pvalloc				dl_pvalloc
#define mallinfo			dl_mallinfo
#define mallopt				dl_mallopt
#define malloc_stats		dl_malloc_stats
#define malloc_trim			dl_malloc_trim
#define malloc_usable_size	dl_malloc_usable_size
#define HAVE_MMAP			0
#define MORECORE			dcsim_sbrk
#define MORECORE_CLEARS		0
#define malloc_getpagesize	(4096)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "mallocr.inc"
#pragma GCC diagnostic pop
#undef malloc
#undef calloc
#undef realloc
#undef memalign
#undef free

static int DCSim_InArena(void *p)
{
	return arena && (unsigned char *)p >= arena && (unsigned char *)p < arena + (DC_MEM_TOP - DCSIM_HEAP_START);
}

static void DCSim_Report(void)
{
	unsigned long peak = sbrkpeak + 64 * 1024 - 0x8c000000u;
	fprintf(stderr, "dcsim: peak RAM including image, KOS and 64 K stack: %lu bytes (%.3f MiB), %s %ld bytes of a retail 16 MB\n",
		peak, (double)peak / (1024 * 1024), peak > RETAIL ? "OVER by" : "headroom",
		peak > RETAIL ? (long)(peak - RETAIL) : (long)(RETAIL - peak));
}

static void DCSim_Init(void)
{
	unsigned char *block;
	size_t size;
	int megs = 32;

	if (arena)
		return;
	if (getenv("DCSIM_MEGS"))
		megs = atoi(getenv("DCSIM_MEGS"));
	memtop = 0x8c000000u + megs * 1024 * 1024 - 64 * 1024;
	size = DC_MEM_TOP - DCSIM_HEAP_START;
	// the same place in a page as on the Dreamcast, so it lines up alike
	block = (unsigned char *)aligned_alloc(4096, (size + 8192 + 4095) & ~(size_t)4095);
	if (!block)
	{
		fprintf(stderr, "dcsim: no %u bytes for the arena\n", (unsigned)size);
		exit(1);
	}
	arena = block + (DCSIM_HEAP_START & 4095);
	sbrkpeak = sbrkbase = DCSIM_HEAP_START;
	atexit(DCSim_Report);
	// what KallistiOS has taken before the engine starts
	dl_malloc(DCSIM_KOS_BYTES);
	printf("dcsim: Dreamcast heap %u K of %d MB (image ends 0x%x), %u K of it KallistiOS's\n",
		(unsigned)(size / 1024), megs, (unsigned)DCSIM_HEAP_START, (unsigned)(DCSIM_KOS_BYTES / 1024));
}

void *dcsim_malloc(size_t size)
{
	DCSim_Init();
	return dl_malloc(size);
}

void *dcsim_calloc(size_t n, size_t size)
{
	DCSim_Init();
	return dl_calloc(n, size);
}

void *dcsim_memalign(size_t align, size_t size)
{
	DCSim_Init();
	return dl_memalign(align, size);
}

// a block from the system's malloc (strdup and the like) goes back to it
void dcsim_free(void *p)
{
	if (!p)
		return;
	if (DCSim_InArena(p))
		dl_free(p);
	else
		free(p);
}

void *dcsim_realloc(void *p, size_t size)
{
	DCSim_Init();
	if (p && !DCSim_InArena(p))
		return realloc(p, size);
	return dl_realloc(p, size);
}

// the most one malloc can have: the top chunk and the RAM above it, or the
// biggest free chunk below
static unsigned long DCSim_LargestFree(void)
{
	unsigned long largest = chunksize(top) + (DC_MEM_TOP - sbrkbase);
	mbinptr b;
	mchunkptr p;
	int i;

	for (i = 1; i < NAV; i++)
	{
		b = bin_at(i);
		for (p = last(b); p != b; p = p->bk)
			if (chunksize(p) > largest)
				largest = chunksize(p);
	}
	return largest;
}

// free in the heap and above it
static unsigned long DCSim_FreeBytes(void)
{
	return (DC_MEM_TOP - sbrkbase) + dl_mallinfo().fordblks;
}

void DCSim_Info(int *inuse, int *freebytes)
{
	struct dl_mallinfo mi;

	DCSim_Init();
	mi = dl_mallinfo();
	// the whole RAM as the Dreamcast's: the image, KOS, the stack and the
	// heap in use, against everything else
	*inuse = (DCSIM_HEAP_START - 0x8c000000u) + 64 * 1024 + mi.uordblks;
	*freebytes = (DC_MEM_TOP - sbrkbase) + mi.fordblks;
}
