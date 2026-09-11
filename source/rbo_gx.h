#ifndef RBO_GX_H
#define RBO_GX_H

/*
 * DirectDraw7 drop-in (PC FUN_00415b70).
 * GC: GX orthographic 2D blit, TPL texobjs, present ≈ Flip (VSync).
 *
 * Optional aspect: RBO_ASPECT_4_3 (default) vs RBO_ASPECT_16_9 scaffold
 * for Eng widescreen patch later — does not change EFB size yet.
 */

#include <gccore.h>
#include <ogc/tpl.h>

#define RBO_SCREEN_W 640
#define RBO_SCREEN_H 480

typedef enum {
	RBO_ASPECT_4_3 = 0,
	RBO_ASPECT_16_9 = 1
} RboAspect;

void rbo_gx_init(GXRModeObj *rmode, void *gp_fifo, u32 fifo_size);

/*
 * VTXFMT0 = POS XY F32 + TEX0 ST F32. Must run after GX_Init and before any
 * GX_Begin. GX_Init's default VAT is XYZ, so 2D quads over-read the FIFO
 * (Dolphin: opcode 0x43 / GetPointerForRange). Never call GX_Init from here.
 */
void rbo_gx_apply_vtxfmt0(void);

void rbo_gx_set_aspect(RboAspect aspect);
RboAspect rbo_gx_aspect(void);

/* Ortho projection for pixel-ish 2D (0,0 top-left → W,H). */
void rbo_gx_set_ortho2d(f32 w, f32 h);

/* Standard 640x480 ortho used by UI plates. */
void rbo_gx_set_ui_ortho(void);

void rbo_gx_bind_tex(GXTexObj *obj);

/*
 * Textured quad in current UI ortho (POS+TEX0). Caller owns TEV / tex bind
 * unless using blit_src / blit_fullscreen helpers below.
 * Matches main.c drawTexturedQuadUV FIFO layout.
 */
void rbo_gx_blit_rect(f32 dx, f32 dy, f32 dw, f32 dh,
		      f32 u0, f32 v0, f32 u1, f32 v1);

/*
 * UI atlas slice: pixel Src rect → UV, bind tex, REPLACE, emit quad.
 * sheet_w/h = atlas dimensions (Title.Img / SYSTEM.Img / char sheet).
 */
void rbo_gx_blit_src(GXTexObj *obj,
		     f32 dx, f32 dy, f32 dw, f32 dh,
		     f32 sx, f32 sy, f32 sw, f32 sh,
		     f32 sheet_w, f32 sheet_h);

/* Full-screen textured quad in UI ortho space (DD primary surface blit). */
void rbo_gx_blit_fullscreen(GXTexObj *obj);

/*
 * Present stand-in for DD Flip: wait for next VI field.
 * Caller still owns GX_DrawDone / GX_CopyDisp / SetNextFramebuffer.
 */
void rbo_gx_present(void);

#endif /* RBO_GX_H */
