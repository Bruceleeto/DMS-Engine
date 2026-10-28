// SPDX-License-Identifier: GPL-2.0-only
/*
 * The KOS PVR API, on libpvr.
 *
 * bloom's renderer (pvr.c, verbatim) talks to KOS: pvr_scene_begin, the
 * store-queue direct-render path, pvr_mem_malloc, PVR_SET.  This file is
 * that API with libpvr's software HOLLY underneath, so the same C runs on
 * Linux.  It is one TA buffer set and a synchronous render at
 * pvr_scene_finish: the next scene cannot start until the render has
 * finished anyway, and libpvr's threaded tile renderer does the waiting.
 *
 * VRAM is one 8 MB host block aligned to 16 MB, and a pvr_ptr_t is a host
 * pointer into it.  That is what lets pvr.c's pointer arithmetic, its
 * `& 0xffffff` / `& PVR_RAM_SIZE` masks and to_pvr_txr_ptr() work
 * unchanged: the low 24 bits of a host pointer ARE the 64-bit-path VRAM
 * address.  The 32-bit framebuffer path is the bank interleave, see
 * libpvr/vram.h.
 *
 * Layout (64-bit-path addresses unless said otherwise):
 *
 *   0x000000  OPBs, 300 tiles x (16+0+16+8+16) words, then 3x overflow
 *   0x041A00  region array (type 2, 6 words per tile)
 *   0x044000  vertex buffer (TA_ISP_BASE == PARAM_BASE), pvr_init's
 *             vertex_buf_size (768 KiB by default), with the background
 *             poly in the last 64 bytes
 *   SET_END   frame buffers, as 32-bit-path addresses SET_END (bank A)
 *             and 0x400000 + SET_END (bank B); their 64-bit image is
 *             [2 * SET_END, TEX_BASE)
 *   TEX_BASE  texture heap to VRAM_SIZE (pvr_mem_malloc)
 *
 * With a 768K vertex buffer these are the numbers KOS ends up with for
 * bloom's pvr_init params (opb {16,0,16,8,16}, overflow 3), so the heap is
 * the same size; each 64K more of vertex buffer takes 128K from the heap, as
 * KOS's double buffered vertex buffers do.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <vector>

#include "libpvr/pvr.h"
#include "libpvr/vram.h"
#include "kos_pvr.h"
#include "kos/arch/arch.h" /* GPU_PVR_VRAM_MB */

using R = pvr::RegisterName;

#define VRAM_SIZE      ((uint32_t)GPU_PVR_VRAM_MB << 20) /* kos/arch/arch.h: 8 or 16 MB */
#define VRAM_ALIGN     0x1000000u
#define SCREEN_W       640
#define SCREEN_H       480
#define TILES_X        (SCREEN_W / 32)
#define TILES_Y        (SCREEN_H / 32)
#define NTILES         (TILES_X * TILES_Y)

/* OPB sizes in words per list: OP, OP_MOD, TR, TR_MOD, PT (bloom's emu.c),
 * or pvr_init's (kos_pvr_set_opb_sizes). */
static uint32_t opb_words[5] = { 16, 0, 16, 8, 16 };
static uint32_t opb_codes[5] = { 2, 0, 2, 1, 2 };  /* 0/8/16/32 words -> 0..3 */
#define OPB_OVERFLOW   3

#define OL_BASE        0x000000u
static uint32_t ol_base_size;       /* one OPB per tile per list */
static uint32_t next_opb_init;      /* OL_BASE + ol_base_size */
static uint32_t ol_limit;           /* + OPB_OVERFLOW more of it */
static uint32_t region_base = 0x041A00u;	/* right after the OPB overflow area */
#define L_REGION_BASE    region_base
#define L_REGION_BYTES   (NTILES * 6 * 4)
static uint32_t isp_base = 0x044000u;	/* also PARAM_BASE */
#define ISP_BASE       isp_base
static uint32_t vertex_bytes = 768 * 1024;	/* kos_pvr_set_vertex_bytes */
#define VERTEX_BYTES   vertex_bytes
#define BG_OFFSET      (VERTEX_BYTES - 64)
#define ISP_LIMIT      (ISP_BASE + BG_OFFSET)
#define SET_END        (ISP_BASE + VERTEX_BYTES)          /* 0x104000 at 768K */
#define FRAME_BYTES    (SCREEN_W * SCREEN_H * 2)
static uint32_t frame32[2];        /* { SET_END, 0x400000 + SET_END } */
#define TEX_BASE       (2 * (SET_END + FRAME_BYTES))     /* 0x334000 at 768K */

static uint8_t *vram;
static pvr::PVR *hw;

/* --- KOS-visible state ------------------------------------------------ */

static int  list_open = -1;          /* PVR_LIST_NONE */
static unsigned lists_closed;
static unsigned lists_enabled = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4);
static bool to_texture, next_to_texture;
static uint32_t rtt_addr, rtt_w, rtt_h, rtt_rp;
static uint32_t rtt_pack = 1, next_rtt_pack = 1; /* FB_W_CTRL's pack mode for a render to texture: 1 RGB565, 2 ARGB4444 */
static int  view_target;             /* frame32[] index of the front buffer */
static bool frame_rendered;
static bool dither = true;
static uint32_t bg_color;
static bool presort;                 /* pvr_init autosort_disabled */

/* Direct-render staging: pvr_dr_target() toggles between the two halves. */
alignas(64) static uint8_t dr_buf[64];
extern "C" uint32_t pvr_dr_addr;
uint32_t pvr_dr_addr = 0;

static inline void reg(R r, uint32_t v) { hw->write_register(r, v); }

/* PVR_TASTREAM=<file>: every 32-byte TA object, verbatim, with a marker
 * at each render, so two renderers can be compared byte for byte. */
static FILE *ta_tap;
static bool ta_tap_init;
static void ta_send(const uint8_t *p, size_t n)
{
	if (!ta_tap_init) {
		const char *f = getenv("PVR_TASTREAM");
		ta_tap_init = true;
		if (f)
			ta_tap = fopen(f, "wb");
	}
	if (ta_tap)
		fwrite(p, 1, n, ta_tap);
	hw->send_ta_data(p, n);
}
static void ta_mark(const char *what)
{
	uint8_t buf[32];
	if (!ta_tap)
		return;
	memset(buf, 0, 32);
	snprintf((char *)buf, 32, "==%s==", what);
	fwrite(buf, 1, 32, ta_tap);
	fflush(ta_tap);
}
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline void w32(uint32_t a, uint32_t v) { memcpy(vram + a, &v, 4); }

/* --- texture heap: first-fit over [TEX_BASE, VRAM_SIZE) ---------------- */

static std::map<uint32_t, uint32_t> free_blocks;   /* addr -> size */
static std::map<uint32_t, uint32_t> used_blocks;

static void heap_reset(void)
{
	free_blocks.clear();
	used_blocks.clear();
	free_blocks[TEX_BASE] = VRAM_SIZE - TEX_BASE;
}

extern "C" void *pvr_mem_malloc(size_t size)
{
	size = (size + 63) & ~(size_t)63;
	for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
		if (it->second < size)
			continue;
		uint32_t addr = it->first, rest = it->second - size;
		free_blocks.erase(it);
		if (rest)
			free_blocks[addr + size] = rest;
		used_blocks[addr] = size;
		return vram + addr;
	}
	fprintf(stderr, "pvr_mem_malloc: out of VRAM (%zu bytes)\n", size);
	return NULL;
}

extern "C" void pvr_mem_free(void *chunk)
{
	if (!chunk)
		return;
	uint32_t addr = (uint8_t *)chunk - vram;
	auto it = used_blocks.find(addr);
	if (it == used_blocks.end()) {
		fprintf(stderr, "pvr_mem_free: bad pointer %p\n", chunk);
		return;
	}
	uint32_t size = it->second;
	used_blocks.erase(it);
	/* coalesce with neighbours */
	auto nx = free_blocks.lower_bound(addr);
	if (nx != free_blocks.end() && nx->first == addr + size) {
		size += nx->second;
		free_blocks.erase(nx);
	}
	if (!free_blocks.empty()) {
		auto pv = free_blocks.lower_bound(addr);
		if (pv != free_blocks.begin()) {
			--pv;
			if (pv->first + pv->second == addr) {
				addr = pv->first;
				size += pv->second;
				free_blocks.erase(pv);
			}
		}
	}
	free_blocks[addr] = size;
}

extern "C" size_t pvr_mem_available(void)
{
	size_t n = 0;
	for (auto &b : free_blocks)
		n += b.second;
	return n;
}

/* --- registers ---------------------------------------------------------- */

extern "C" uint32_t pvr_host_reg_read(uint32_t r)
{
	return hw->read_register(r);
}

extern "C" void pvr_host_reg_write(uint32_t r, uint32_t v)
{
	hw->write_register(r, v);
}

/* --- store queues ------------------------------------------------------- */

extern "C" uint32_t *sq_lock(void *dest)
{
	uintptr_t d = (uintptr_t)dest;

	if ((d & 0xff000000) == 0x10000000)          /* PVR_TA_INPUT */
		return (uint32_t *)dr_buf;
	if ((d & 0xff000000) == 0x11000000)          /* PVR_TA_TEX_MEM */
		return (uint32_t *)(vram + (d & 0xffffff));
	return (uint32_t *)dest;
}

extern "C" void sq_unlock(void) {}

extern "C" void sq_wait(void) {}

extern "C" void sq_flush(void *src)
{
	uint8_t *p = (uint8_t *)src;

	if (p >= dr_buf && p < dr_buf + sizeof(dr_buf))
		ta_send(p, 32);
	/* a VRAM line was written in place */
}

extern "C" void *sq_cpy(void *dest, const void *src, size_t n)
{
	uint32_t *d = sq_lock(dest);
	if ((uint8_t *)d == dr_buf) {
		ta_send((const uint8_t *)src, n & ~31u);
		return dest;
	}
	memcpy(d, src, n);
	return dest;
}

extern "C" void *sq_set32(void *dest, uint32_t c, size_t n)
{
	uint32_t *d = sq_lock(dest);
	if ((uint8_t *)d == dr_buf) {
		uint8_t buf[32];
		for (int i = 0; i < 8; i++) memcpy(buf + 4 * i, &c, 4);
		for (size_t i = 0; i + 32 <= n; i += 32)
			ta_send(buf, 32);
		return dest;
	}
	for (size_t i = 0; i + 4 <= n; i += 4)
		d[i / 4] = c;
	return dest;
}

/* --- textures / palette / misc ----------------------------------------- */

extern "C" void pvr_txr_load(const void *src, void *dst, size_t count)
{
	memcpy(dst, src, count);
}

extern "C" void pvr_txr_set_stride(size_t texture_width)
{
	uint32_t t = hw->read_register(R::TEXT_CONTROL) & ~0x1fu;
	reg(R::TEXT_CONTROL, t | ((texture_width / 32) & 0x1f));
}

extern "C" void pvr_set_pal_format(int fmt)
{
	reg(R::PAL_RAM_CTRL, fmt);
}

extern "C" void pvr_set_bg_color(float r, float g, float b)
{
	bg_color = ((int)(255 * r) << 16) | ((int)(255 * g) << 8) | (int)(255 * b);
}

/* the background plane's depth (ISP_BACKGND_D): a polygon no deeper than
   it fails the depth test. KOS starts it at 1e-4, as here, so a sky the
   chip refuses is refused here too */
static float zclip = 1e-4f;
extern "C" void pvr_set_zclip(float zc)
{
	zclip = zc;
}

extern "C" void vid_set_dithering(bool enable)
{
	dither = enable;
}

extern "C" void pvr_set_vertical_scale(float f) { (void)f; }

/* KOS pvr_prim.c, with the cache-line allocate spelled out. */
extern "C" void pvr_mod_compile(void *dst, int list, uint32_t mode, uint32_t cull)
{
	uint32_t *d = (uint32_t *)dst;
	uint32_t cmd = 0x80000000u | ((list & 7) << 24);

	if (mode == 1 || mode == 2)      /* INCLUDE_LAST / EXCLUDE_LAST */
		cmd |= 1 << 6;               /* PVR_TA_CMD_MODIFIERMODE */
	for (int i = 0; i < 8; i++)
		d[i] = cmd;
	d[1] = ((mode & 3) << 29) | ((cull & 3) << 27);
}

extern "C" void copy32(void *dst, const void *src)
{
	memcpy(dst, src, 32);
}

/* --- scene ---------------------------------------------------------------- */

static void write_region_array(int tiles_x, int tiles_y);
static int ra_tiles_x = -1, ra_tiles_y = -1; /* what the region array is written for */

static void ta_init(void)
{
	/* A render to a texture covers only its tiles (as KOS's pvr_scene_begin_rtt
	 * does), not the screen's 20 x 15. */
	int tx = to_texture ? (int)((rtt_w + 31) / 32) : TILES_X;
	int ty = to_texture ? (int)((rtt_h + 31) / 32) : TILES_Y;
	if (tx > TILES_X) tx = TILES_X;
	if (ty > TILES_Y) ty = TILES_Y;
	if (tx != ra_tiles_x || ty != ra_tiles_y)
		write_region_array(tx, ty);

	reg(R::TA_OL_BASE, OL_BASE);
	reg(R::TA_OL_LIMIT, ol_limit);
	reg(R::TA_ISP_BASE, ISP_BASE);
	reg(R::TA_ISP_LIMIT, ISP_LIMIT);
	reg(R::TA_NEXT_OPB_INIT, next_opb_init);
	reg(R::TA_GLOB_TILE_CLIP, ((uint32_t)(ty - 1) << 16) | (uint32_t)(tx - 1));
	reg(R::TA_ALLOC_CTRL, opb_codes[0] | (opb_codes[1] << 4) | (opb_codes[2] << 8)
			      | (opb_codes[3] << 12) | (opb_codes[4] << 16));
	reg(R::TA_LIST_INIT, 0x80000000u);
}

static void write_bg(uint32_t w, uint32_t h)
{
	uint32_t a = ISP_BASE + BG_OFFSET;

	/* KOS pvr_begin_queued_render's background poly. */
	w32(a + 0x00, 0x90800000u);
	w32(a + 0x04, 0x20800440u);
	w32(a + 0x08, 0);
	w32(a + 0x0c, f2u(0.0f));       w32(a + 0x10, f2u((float)h));
	w32(a + 0x14, f2u(1e-7f));      w32(a + 0x18, bg_color);
	w32(a + 0x1c, f2u(0.0f));       w32(a + 0x20, f2u(0.0f));
	w32(a + 0x24, f2u(1e-7f));      w32(a + 0x28, bg_color);
	w32(a + 0x2c, f2u((float)w));   w32(a + 0x30, f2u((float)h));
	w32(a + 0x34, f2u(1e-7f));      w32(a + 0x38, bg_color);
}

static void write_region_array(int tiles_x, int tiles_y)
{
	uint32_t a = L_REGION_BASE;
	uint32_t list_base[5], acc = OL_BASE;

	ra_tiles_x = tiles_x;
	ra_tiles_y = tiles_y;
	for (int l = 0; l < 5; l++) {
		list_base[l] = acc;
		acc += opb_words[l] * 4 * tiles_x * tiles_y;
	}

	for (int ty = 0; ty < tiles_y; ty++) {
		for (int tx = 0; tx < tiles_x; tx++) {
			int t = ty * tiles_x + tx;
			bool last = ty == tiles_y - 1 && tx == tiles_x - 1;
			/* z_clear (bit30) 0 = clear, presort (bit29) 0 = autosort,
			 * flush (bit28) 0 = write to the frame buffer. */
			w32(a, ((uint32_t)last << 31) | ((uint32_t)presort << 29)
			       | (ty << 8) | (tx << 2));
			for (int l = 0; l < 5; l++) {
				uint32_t p = opb_words[l] ? list_base[l] + t * opb_words[l] * 4
							  : 0x80000000u;
				w32(a + 4 + 4 * l, p);
			}
			a += 24;
		}
	}
}

extern "C" void pvr_scene_begin(void)
{
	lists_closed = 0;
	list_open = -1;
	to_texture = next_to_texture;
	next_to_texture = false;
	rtt_pack = next_rtt_pack;
	next_rtt_pack = 1;
	ta_init();
}

extern "C" int pvr_scene_begin_rtt(void *txr, uint32_t render_w,
				   uint32_t render_h, uint32_t stride_px)
{
	if (!txr || !render_w || !render_h || stride_px < render_w || (stride_px & 3))
		return -1;
	rtt_rp = stride_px * 2 / 8;
	rtt_w = render_w;
	rtt_h = render_h;
	rtt_addr = (uint8_t *)txr - vram;
	next_to_texture = true;
	pvr_scene_begin();
	return 0;
}

/* Not KOS: the next pvr_scene_begin_rtt() writes ARGB4444 instead of RGB565
 * (KOS writes the frame buffer's format; on the Dreamcast this is
 * PVR_SET(PVR_FB_CFG_2, 2 | dither) after pvr_scene_begin_rtt()). */
extern "C" void kos_pvr_rtt_argb4444(void)
{
	next_rtt_pack = 2;
}

extern "C" int pvr_list_finish(void);

extern "C" int pvr_list_begin(int list)
{
	if (lists_closed & (1u << list))
		return -1;
	if (list_open != -1 && list_open != list)
		pvr_list_finish();
	list_open = list;
	return 0;
}

extern "C" int pvr_list_finish(void)
{
	uint8_t buf[32];

	if (list_open == -1)
		return -1;

	/* KOS: a blank polyhdr in case the list was empty, then the EOL. */
	memset(buf, 0, 32);
	uint32_t cmd = ((uint32_t)list_open << 24) | 0x80840012u;
	memcpy(buf, &cmd, 4);
	ta_send(buf, 32);
	lists_closed |= 1u << list_open;
	memset(buf, 0, 32);
	ta_send(buf, 32);
	list_open = -1;
	return 0;
}

static void render_now(void)
{
	uint32_t w = to_texture ? rtt_w : SCREEN_W;
	uint32_t h = to_texture ? rtt_h : SCREEN_H;

	write_bg(w, h);

	reg(R::PARAM_BASE, ISP_BASE);
	reg(R::REGION_BASE, L_REGION_BASE);
	reg(R::ISP_BACKGND_T, 0x01000000u | ((BG_OFFSET >> 2) << 3));
	reg(R::ISP_BACKGND_D, f2u(zclip));
	reg(R::FB_W_CTRL, (to_texture ? rtt_pack : 1) | (dither ? 8 : 0)); /* RGB565, or a texture's ARGB4444 */

	if (to_texture) {
		reg(R::FB_W_SOF1, rtt_addr | (1u << 24));
		reg(R::FB_W_SOF2, rtt_addr | (1u << 24));
		reg(R::FB_W_LINESTRIDE, rtt_rp);
	} else {
		uint32_t f = frame32[view_target ^ 1];
		reg(R::FB_W_SOF1, f);
		reg(R::FB_W_SOF2, f);
		reg(R::FB_W_LINESTRIDE, SCREEN_W * 2 / 8);
	}
	reg(R::FB_X_CLIP, (w - 1) << 16);
	reg(R::FB_Y_CLIP, (h - 1) << 16);

	ta_mark(to_texture ? "render rtt" : "render");
	reg(R::STARTRENDER, 0xffffffffu);
	hw->wait_render_done();

	static const char *dbg = getenv("PVR_DEBUG");
	static FILE *dbgf;
	if (dbg && !dbgf)
		dbgf = fopen(dbg, "w");
	if (dbgf) {
		static int n;
		fprintf(dbgf, "pvr render %d%s: lists op%d om%d tr%d tm%d pt%d | opb_ovf%d isp_ovf%d illegal%d ooc%d dl_invalid%d\n",
			n++, to_texture ? " (rtt)" : "",
			hw->is_opaque_list_complete(), hw->is_opaque_modifier_complete(),
			hw->is_translucent_list_complete(), hw->is_translucent_modifier_complete(),
			hw->is_punch_through_complete(),
			hw->is_opb_overflow(), hw->is_isp_overflow(), hw->is_illegal_parameter(),
			hw->is_isp_out_of_cache(), hw->is_display_list_invalid());
		fflush(dbgf);
	}
	hw->clear_ta_interrupts();

	if (!to_texture) {
		view_target ^= 1;
		frame_rendered = true;
	}
}

extern "C" int pvr_scene_finish(void)
{
	if (list_open != -1)
		pvr_list_finish();

	for (int i = 0; i < 5; i++) {
		if (!(lists_enabled & (1u << i)) || (lists_closed & (1u << i)))
			continue;
		pvr_list_begin(i);
		pvr_list_finish();
	}

	render_now();
	if (!to_texture)
		kos_pvr_present();
	return 0;
}

extern "C" int pvr_wait_ready(void) { return 0; }
extern "C" int pvr_check_ready(void) { return 0; }
extern "C" int pvr_wait_render_done(void) { return 0; }

/* KOS: (frame32 & (PVR_RAM_SIZE - 1)) * 2 + PVR_RAM_BASE.  With a 16 MB
 * aligned block the same masks pvr.c applies (& 0xffffff, & PVR_RAM_SIZE,
 * to_pvr_txr_ptr) give the same answers on a host pointer. */
extern "C" void *pvr_get_front_buffer(void)
{
	return vram + 2 * (frame32[view_target] & (VRAM_SIZE - 1));
}

extern "C" void *pvr_get_back_buffer(void)
{
	return vram + 2 * (frame32[view_target ^ 1] & (VRAM_SIZE - 1));
}

/* --- init / present ------------------------------------------------------- */

/* pvr_init(autosort_disabled): translucent polys are drawn in submission
 * order instead of being depth sorted.  Call before kos_pvr_init(). */
extern "C" void kos_pvr_set_presort(int enable)
{
	presort = enable != 0;
}

/* pvr_init(vertex_buf_size): the TA's vertex buffer.  Call before
 * kos_pvr_init(). */
/* pvr_init(opb_sizes): each list's OPB in words (0, 8, 16 or 32), 0 leaving
 * the list off.  Call before kos_pvr_init(). */
extern "C" void kos_pvr_set_opb_sizes(const int *words)
{
	lists_enabled = 0;
	for (int l = 0; l < 5; l++) {
		opb_words[l] = words[l] == 8 || words[l] == 16 || words[l] == 32 ? words[l] : 0;
		opb_codes[l] = opb_words[l] == 32 ? 3 : opb_words[l] / 8;
		if (opb_words[l])
			lists_enabled |= 1u << l;
	}
}

/* KOS pvr_misc.c */
extern "C" void pvr_set_shadow_scale(bool enable, float scale_value)
{
	int s = (int)(scale_value * 255);

	reg(R::FPU_SHAD_SCALE, ((uint32_t)enable << 8) | (s & 0xff));
}

extern "C" void kos_pvr_set_vertex_bytes(unsigned int bytes)
{
	if (bytes)
		vertex_bytes = (bytes + 31) & ~31u;
}

extern "C" int kos_pvr_init(void)
{
	if (hw)
		return 0;

	void *p = NULL;
	if (posix_memalign(&p, VRAM_ALIGN, VRAM_SIZE) || !p) {
		fprintf(stderr, "kos_pvr: cannot allocate VRAM\n");
		return -1;
	}
	vram = (uint8_t *)p;
	memset(vram, 0, VRAM_SIZE);

	ol_base_size = 0;
	for (int l = 0; l < 5; l++)
		ol_base_size += opb_words[l] * 4 * NTILES;
	next_opb_init = OL_BASE + ol_base_size;
	/* (an OPB overflow writes the shared End-of-List word AT TA_OL_LIMIT, which "cannot be used for other data":
	 * one 64-byte block short of the region array, else it lands on tile 0's entry, sets z_clear / last / flush,
	 * and every render after shows only what that left) */
	ol_limit = OL_BASE + ol_base_size * (1 + OPB_OVERFLOW) - 64;
	region_base = (ol_limit + 64 + 0xff) & ~0xffu;
	isp_base = (region_base + L_REGION_BYTES + 0xfff) & ~0xfffu;
	frame32[0] = SET_END;
	frame32[1] = 0x400000u + SET_END;
	if (SET_END + FRAME_BYTES > 0x400000u || TEX_BASE >= VRAM_SIZE) {
		fprintf(stderr, "kos_pvr: a %u byte vertex buffer leaves no room\n", vertex_bytes);
		return -1;
	}
	if (ol_limit > L_REGION_BASE || L_REGION_BASE + L_REGION_BYTES > ISP_BASE) {
		fprintf(stderr, "kos_pvr: VRAM layout overlap (ol_limit %x, region end %x)\n",
			ol_limit, L_REGION_BASE + L_REGION_BYTES);
		return -1;
	}

	hw = new pvr::PVR(vram, VRAM_SIZE);
	heap_reset();

	reg(R::SOFTRESET, 3);
	reg(R::SOFTRESET, 0);

	/* KOS pvr_init register writes, by libpvr's names. */
	reg(R::SDRAM_CFG, 0x15d1c951);
	reg(R::SDRAM_REFRESH, 0x20);
	reg(R::FB_BURSTCTRL, 0x93f39);
	reg(R::ISP_FEED_CFG, 0x00800408 | (presort ? 1 : 0));
	reg(R::FPU_PERP_VAL, 0);
	reg(R::SPAN_SORT_CFG, 0x101);
	reg(R::FOG_COL_RAM, 0x7f7f7f);
	reg(R::FOG_COL_VERT, 0x7f7f7f);
	reg(R::FOG_CLAMP_MIN, 0);
	reg(R::FOG_CLAMP_MAX, 0xffffffff);
	/* KOS writes 7 (pvr_init_shutdown.c: PVR_SET(PVR_UNK_0080, 7)).  The
	 * three bits are independent sampling-position switches -- 2 = TSP
	 * texel, 1 = TSP pixel, 0 = FPU pixel -- and Randy Linden's bleemcast!
	 * hang needed 0 and 1 to *disagree*.  PVR_HALF_OFFSET overrides it so
	 * the question is a measurement here rather than a guess. */
	{
		const char *ho = getenv("PVR_HALF_OFFSET");
		reg(R::HALF_OFFSET, ho ? (uint32_t)strtoul(ho, NULL, 0) : 7u);
	}
	reg(R::FPU_SHAD_SCALE, 1);
	reg(R::FPU_PARAM_CFG, 0x0027df77);        /* bit 21: type-2 region header */
	reg(R::TEXT_CONTROL, 0);
	reg(R::FOG_DENSITY, 0xff07);
	reg(R::Y_COEFF, 0x8040);
	reg(R::SCALER_CTL, 0x400);
	reg(R::PT_ALPHA_REF, 0xff);
	reg(R::FPU_CULL_VAL, f2u(0.00001f));      /* emu.c: PVR_OBJECT_CLIP */
	reg(R::PAL_RAM_CTRL, 0);
	reg(R::VO_BORDER_COL, 0);

	/* display side, for read_framebuffer */
	reg(R::FB_R_CTRL, (1 << 8) | 1);          /* enable, RGB565 */
	reg(R::FB_R_SIZE, ((SCREEN_H - 1) << 10) | (SCREEN_W - 1));
	reg(R::FB_R_SOF1, frame32[0]);
	reg(R::FB_R_SOF2, frame32[0]);

	write_region_array(TILES_X, TILES_Y);
	bg_color = 0;

	pvr_dr_addr = (uint32_t)(uintptr_t)dr_buf;
	view_target = 0;
	frame_rendered = false;
	to_texture = next_to_texture = false;
	list_open = -1;
	lists_closed = 0;
	return 0;
}

extern "C" void kos_pvr_shutdown(void)
{
	delete hw;
	hw = NULL;
	free(vram);
	vram = NULL;
}

extern "C" int kos_pvr_read_front(uint8_t *rgb, int stride, int w, int h)
{
	static std::vector<uint32_t> argb;

	if (!hw || !frame_rendered)
		return 0;

	reg(R::FB_R_SOF1, frame32[view_target]);
	reg(R::FB_R_SOF2, frame32[view_target]);
	argb.resize((size_t)w * h);
	hw->read_framebuffer((uint8_t *)argb.data(), w, h);

	/* PVR_DUMP=<dir>: write every 30th frame as a PPM, for looking at. */
	static const char *dump = getenv("PVR_DUMP");
	static int nframe;
	if (dump && (nframe++ % 30) == 0) {
		char name[512];
		snprintf(name, sizeof(name), "%s/frame_%04d.ppm", dump, nframe / 30);
		FILE *f = fopen(name, "wb");
		if (f) {
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				uint8_t px[3] = { (uint8_t)(argb[i] >> 16), (uint8_t)(argb[i] >> 8), (uint8_t)argb[i] };
				fwrite(px, 1, 3, f);
			}
			fclose(f);
		}
	}

	for (int y = 0; y < h; y++) {
		const uint32_t *s = &argb[(size_t)y * w];
		uint8_t *d = rgb + (size_t)y * stride;
		for (int x = 0; x < w; x++, d += 3) {
			uint32_t px = s[x];
			d[0] = px >> 16;
			d[1] = px >> 8;
			d[2] = px;
		}
	}
	return 1;
}
