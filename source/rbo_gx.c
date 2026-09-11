#include <string.h>

#include "rbo_gx.h"

static GXRModeObj *s_rmode;
static RboAspect s_aspect = RBO_ASPECT_4_3;

void rbo_gx_init(GXRModeObj *rmode, void *gp_fifo, u32 fifo_size)
{
	s_rmode = rmode;
	s_aspect = RBO_ASPECT_4_3;
	(void)gp_fifo;
	(void)fifo_size;
	/* Caller already GX_Init'd this fifo. A second GX_Init / SetCPUFifo
	 * with an uninitialized GXFifoObj is how you get a base at 0x40. */
	rbo_gx_apply_vtxfmt0();
}

void rbo_gx_apply_vtxfmt0(void)
{
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
}

void rbo_gx_set_aspect(RboAspect aspect)
{
	s_aspect = aspect;
	(void)s_rmode;
}

RboAspect rbo_gx_aspect(void)
{
	return s_aspect;
}

void rbo_gx_set_ortho2d(f32 w, f32 h)
{
	Mtx44 proj;

	/* Match historical main.c: guOrtho(..., 0, 479, 0, 639, 0, 300). */
	if (w <= 0.0f)
		w = 639.0f;
	if (h <= 0.0f)
		h = 479.0f;
	guOrtho(proj, 0.0f, h, 0.0f, w, 0.0f, 300.0f);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
}

void rbo_gx_set_ui_ortho(void)
{
	rbo_gx_set_ortho2d(639.0f, 479.0f);
}

void rbo_gx_bind_tex(GXTexObj *obj)
{
	if (!obj)
		return;
	GX_LoadTexObj(obj, GX_TEXMAP0);
}

void rbo_gx_blit_rect(f32 dx, f32 dy, f32 dw, f32 dh,
		      f32 u0, f32 v0, f32 u1, f32 v1)
{
	/* Same FIFO layout as main.c drawTexturedQuadUV — POS+TEX0 only. */
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(dx, dy);
		GX_TexCoord2f32(u0, v0);
		GX_Position2f32(dx + dw, dy);
		GX_TexCoord2f32(u1, v0);
		GX_Position2f32(dx + dw, dy + dh);
		GX_TexCoord2f32(u1, v1);
		GX_Position2f32(dx, dy + dh);
		GX_TexCoord2f32(u0, v1);
	GX_End();
}

void rbo_gx_blit_src(GXTexObj *obj,
		     f32 dx, f32 dy, f32 dw, f32 dh,
		     f32 sx, f32 sy, f32 sw, f32 sh,
		     f32 sheet_w, f32 sheet_h)
{
	Mtx identity;
	f32 u0, v0, u1, v1;

	if (!obj || sheet_w <= 0.0f || sheet_h <= 0.0f)
		return;

	u0 = sx / sheet_w;
	v0 = sy / sheet_h;
	u1 = (sx + sw) / sheet_w;
	v1 = (sy + sh) / sheet_h;

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_LoadTexObj(obj, GX_TEXMAP0);
	rbo_gx_blit_rect(dx, dy, dw, dh, u0, v0, u1, v1);
}

void rbo_gx_blit_fullscreen(GXTexObj *obj)
{
	Mtx identity;

	if (!obj)
		return;

	/*
	 * Keep this identical to main.c drawFullscreenTex.
	 * Extra TEV/ortho thrashing previously desynced Dolphin's FIFO.
	 */
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_LoadTexObj(obj, GX_TEXMAP0);
	rbo_gx_blit_rect(0.0f, 0.0f, (f32)RBO_SCREEN_W, (f32)RBO_SCREEN_H,
			 0.0f, 0.0f, 1.0f, 1.0f);
}

void rbo_gx_present(void)
{
	VIDEO_WaitVSync();
}
