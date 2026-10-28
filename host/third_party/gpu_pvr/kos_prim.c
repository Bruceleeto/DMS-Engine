/*
 * KOS pvr_prim.c, for the parts of it bloom uses.
 *
 * bloom's pvr.c builds its own headers and pushes them through the store
 * queues, so it never touches these; platform.c's 24bpp blit does.  The
 * context->header compilation is pure bit twiddling against the same KOS
 * headers we already carry, so it is KOS's code verbatim; only the submission
 * at the bottom is ours, and it just feeds the TA the way sq_flush does.
 */

#include <assert.h>
#include <string.h>
#include <stdint.h>

#include <arch/cache.h>
#include <dc/pvr.h>

void pvr_poly_compile(pvr_poly_hdr_t *dst, const pvr_poly_cxt_t *src)
{
	pvr_txr_ptr_t txr_base;
	uint32_t cmd, mode2, mode3;

	cmd = PVR_CMD_POLYHDR
		| FIELD_PREP(PVR_TA_CMD_TXRENABLE, src->txr.enable)
		| FIELD_PREP(PVR_TA_CMD_TYPE, src->list_type)
		| FIELD_PREP(PVR_TA_CMD_CLRFMT, src->fmt.color)
		| FIELD_PREP(PVR_TA_CMD_SHADE, src->gen.shading)
		| FIELD_PREP(PVR_TA_CMD_UVFMT, src->fmt.uv)
		| FIELD_PREP(PVR_TA_CMD_USERCLIP, src->gen.clip_mode)
		| FIELD_PREP(PVR_TA_CMD_MODIFIER, src->fmt.modifier)
		| FIELD_PREP(PVR_TA_CMD_MODIFIERMODE, src->gen.modifier_mode)
		| FIELD_PREP(PVR_TA_CMD_SPECULAR, src->gen.specular)
		| (1u << 23) | (3u << 18); /* group_en, strip length 6: the TA keeps up to 6 triangles
		                                  * of a strip under one header (at 1 it writes each triangle
		                                  * with its own header and vertices, ~3x the vertex buffer) */

	dcache_alloc_line_with_value(dst, cmd);

	dst->mode1 = FIELD_PREP(PVR_TA_PM1_DEPTHCMP, src->depth.comparison)
		| FIELD_PREP(PVR_TA_PM1_CULLING, src->gen.culling)
		| FIELD_PREP(PVR_TA_PM1_DEPTHWRITE, src->depth.write)
		| FIELD_PREP(PVR_TA_PM1_TXRENABLE, src->txr.enable);

	mode2 = FIELD_PREP(PVR_TA_PM2_SRCBLEND, src->blend.src)
		| FIELD_PREP(PVR_TA_PM2_DSTBLEND, src->blend.dst)
		| FIELD_PREP(PVR_TA_PM2_SRCENABLE, src->blend.src_enable)
		| FIELD_PREP(PVR_TA_PM2_DSTENABLE, src->blend.dst_enable)
		| FIELD_PREP(PVR_TA_PM2_FOG, src->gen.fog_type)
		| FIELD_PREP(PVR_TA_PM2_CLAMP, src->gen.color_clamp)
		| FIELD_PREP(PVR_TA_PM2_ALPHA, src->gen.alpha);

	if (!src->txr.enable) {
		mode3 = 0;
	} else {
		mode2 |= FIELD_PREP(PVR_TA_PM2_TXRALPHA, src->txr.alpha)
			| FIELD_PREP(PVR_TA_PM2_UVFLIP, src->txr.uv_flip)
			| FIELD_PREP(PVR_TA_PM2_UVCLAMP, src->txr.uv_clamp)
			| FIELD_PREP(PVR_TA_PM2_FILTER, src->txr.filter)
			| FIELD_PREP(PVR_TA_PM2_MIPBIAS, src->txr.mipmap_bias)
			| FIELD_PREP(PVR_TA_PM2_TXRENV, src->txr.env)
			| FIELD_PREP(PVR_TA_PM2_USIZE, __builtin_ctz(src->txr.width) - 3)
			| FIELD_PREP(PVR_TA_PM2_VSIZE, __builtin_ctz(src->txr.height) - 3);

		txr_base = to_pvr_txr_ptr(src->txr.base);

		mode3 = FIELD_PREP(PVR_TA_PM3_MIPMAP, src->txr.mipmap)
			| src->txr.format
			| (uint32_t)txr_base;
	}

	dst->mode2 = mode2;
	dst->mode3 = mode3;

	if (src->fmt.modifier && src->gen.modifier_mode) {
		dst->mode2_1 = mode2;
		dst->mode3_1 = mode3;
	}
}

void pvr_poly_cxt_col(pvr_poly_cxt_t *dst, pvr_list_t list)
{
	int alpha;

	memset(dst, 0, sizeof(*dst));

	dst->list_type = list;
	alpha = list > PVR_LIST_OP_MOD;
	dst->fmt.color = PVR_CLRFMT_ARGBPACKED;
	dst->fmt.uv = 0;
	dst->gen.shading = 1;
	dst->depth.comparison = PVR_DEPTHCMP_GREATER;
	dst->depth.write = PVR_DEPTHWRITE_ENABLE;
	dst->gen.culling = PVR_CULLING_CCW;
	dst->txr.enable = 0;
	dst->gen.alpha = alpha;

	if (!alpha) {
		dst->blend.src = PVR_BLEND_ONE;
		dst->blend.dst = PVR_BLEND_ZERO;
	} else {
		dst->blend.src = PVR_BLEND_SRCALPHA;
		dst->blend.dst = PVR_BLEND_INVSRCALPHA;
	}

	dst->blend.src_enable = PVR_BLEND_DISABLE;
	dst->blend.dst_enable = PVR_BLEND_DISABLE;
	dst->gen.fog_type = PVR_FOG_DISABLE;
	dst->gen.color_clamp = 0;
}

void pvr_poly_cxt_txr(pvr_poly_cxt_t *dst, pvr_list_t list,
		      int textureformat, int tw, int th, pvr_ptr_t textureaddr,
		      pvr_filter_mode_t filtering)
{
	int alpha;

	memset(dst, 0, sizeof(*dst));

	dst->list_type = list;
	alpha = list > PVR_LIST_OP_MOD;
	dst->fmt.color = PVR_CLRFMT_ARGBPACKED;
	dst->fmt.uv = 0;
	dst->gen.shading = 1;
	dst->depth.comparison = PVR_DEPTHCMP_GREATER;
	dst->depth.write = PVR_DEPTHWRITE_ENABLE;
	dst->gen.culling = PVR_CULLING_CCW;
	dst->txr.enable = 1;
	dst->gen.alpha = alpha;
	dst->txr.alpha = 0;

	if (!alpha) {
		dst->blend.src = PVR_BLEND_ONE;
		dst->blend.dst = PVR_BLEND_ZERO;
		dst->txr.env = PVR_TXRENV_MODULATE;
	} else {
		dst->blend.src = PVR_BLEND_SRCALPHA;
		dst->blend.dst = PVR_BLEND_INVSRCALPHA;
		dst->txr.env = PVR_TXRENV_MODULATEALPHA;
	}

	dst->blend.src_enable = PVR_BLEND_DISABLE;
	dst->blend.dst_enable = PVR_BLEND_DISABLE;
	dst->gen.fog_type = PVR_FOG_DISABLE;
	dst->gen.color_clamp = 0;
	dst->txr.uv_flip = PVR_UVFLIP_NONE;
	dst->txr.uv_clamp = PVR_UVCLAMP_NONE;
	dst->txr.filter = filtering;
	dst->txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
	dst->txr.width = tw;
	dst->txr.height = th;
	dst->txr.base = textureaddr;
	dst->txr.format = textureformat;
}

/* --- submission: ours, not KOS's ---------------------------------------- */

/* On the DC this is a store-queue burst to PVR_TA_INPUT.  Here it is the same
 * thing sq_flush() does, one 32-byte TA object at a time. */
int pvr_prim(const void *data, size_t size)
{
	const uint8_t *p = data;
	uint32_t *sq;
	size_t i;

	for (i = 0; i + 32 <= size; i += 32) {
		sq = sq_lock((void *)PVR_TA_INPUT);
		memcpy(sq, p + i, 32);
		sq_flush(sq);
	}
	sq_unlock();

	return 0;
}

/* Nothing here has a render clock; bloom only reads rnd_last_time, for an
 * on-screen percentage. */
int pvr_get_stats(pvr_stats_t *stat)
{
	memset(stat, 0, sizeof(*stat));
	return 0;
}
