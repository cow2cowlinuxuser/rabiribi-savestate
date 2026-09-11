#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>
#include <ogc/cache.h>

#include "ddraw.h"
#include "d3d.h"
#include "host.h"
#include "rbo_gx.h"

/* Retail GetDisplayMode fills a 0x7c DDSURFACEDESC2; bit count is at +0x54. */

typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE;
typedef LONG *LPLONG;

typedef struct IDirectDraw7 IDirectDraw7;
typedef struct IDirectDrawSurface7 IDirectDrawSurface7;
typedef struct IDirectDraw7Vtbl IDirectDraw7Vtbl;
typedef struct IDirectDrawSurface7Vtbl IDirectDrawSurface7Vtbl;

struct IDirectDraw7 {
	const IDirectDraw7Vtbl *lpVtbl;
	LONG ref;
	DWORD w, h, bpp;
};

struct IDirectDrawSurface7 {
	const IDirectDrawSurface7Vtbl *lpVtbl;
	LONG ref;
	DWORD w, h, pitch, caps;
	u16 *pixels;
	u8 *tiled;
	IDirectDrawSurface7 *back;
	IDirectDraw7 *dd;
	int dirty;
};

struct IDirectDraw7Vtbl {
	HRESULT (*QueryInterface)(IDirectDraw7 *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirectDraw7 *);
	ULONG (*Release)(IDirectDraw7 *);
	HRESULT (*Compact)(IDirectDraw7 *);
	HRESULT (*CreateClipper)(IDirectDraw7 *, DWORD, LPVOID *, LPVOID);
	HRESULT (*CreatePalette)(IDirectDraw7 *, DWORD, LPVOID, LPVOID *, LPVOID);
	HRESULT (*CreateSurface)(IDirectDraw7 *, LPDDSURFACEDESC2,
				 LPDIRECTDRAWSURFACE7 *, LPVOID);
	HRESULT (*DuplicateSurface)(IDirectDraw7 *, LPDIRECTDRAWSURFACE7,
				    LPDIRECTDRAWSURFACE7 *);
	HRESULT (*EnumDisplayModes)(IDirectDraw7 *, DWORD, LPDDSURFACEDESC2,
				    LPVOID, LPDDENUMMODESCALLBACK2);
	HRESULT (*EnumSurfaces)(IDirectDraw7 *, DWORD, LPDDSURFACEDESC2,
				LPVOID, LPVOID);
	HRESULT (*FlipToGDISurface)(IDirectDraw7 *);
	HRESULT (*GetCaps)(IDirectDraw7 *, LPVOID, LPVOID);
	HRESULT (*GetDisplayMode)(IDirectDraw7 *, LPDDSURFACEDESC2);
	HRESULT (*GetFourCCCodes)(IDirectDraw7 *, DWORD *, DWORD *);
	HRESULT (*GetGDISurface)(IDirectDraw7 *, LPDIRECTDRAWSURFACE7 *);
	HRESULT (*GetMonitorFrequency)(IDirectDraw7 *, DWORD *);
	HRESULT (*GetScanLine)(IDirectDraw7 *, DWORD *);
	HRESULT (*GetVerticalBlankStatus)(IDirectDraw7 *, BOOL *);
	HRESULT (*Initialize)(IDirectDraw7 *, GUID *);
	HRESULT (*RestoreDisplayMode)(IDirectDraw7 *);
	HRESULT (*SetCooperativeLevel)(IDirectDraw7 *, HWND, DWORD);
	HRESULT (*SetDisplayMode)(IDirectDraw7 *, DWORD, DWORD, DWORD, DWORD, DWORD);
	HRESULT (*WaitForVerticalBlank)(IDirectDraw7 *, DWORD, HANDLE);
	HRESULT (*GetAvailableVidMem)(IDirectDraw7 *, LPDDSCAPS2, DWORD *, DWORD *);
	HRESULT (*GetSurfaceFromDC)(IDirectDraw7 *, HDC, LPDIRECTDRAWSURFACE7 *);
	HRESULT (*RestoreAllSurfaces)(IDirectDraw7 *);
	HRESULT (*TestCooperativeLevel)(IDirectDraw7 *);
	HRESULT (*GetDeviceIdentifier)(IDirectDraw7 *, LPDDDEVICEIDENTIFIER2, DWORD);
	HRESULT (*StartModeTest)(IDirectDraw7 *, LPSIZE, DWORD, DWORD);
	HRESULT (*EvaluateMode)(IDirectDraw7 *, DWORD, DWORD *);
};

struct IDirectDrawSurface7Vtbl {
	HRESULT (*QueryInterface)(IDirectDrawSurface7 *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirectDrawSurface7 *);
	ULONG (*Release)(IDirectDrawSurface7 *);
	HRESULT (*AddAttachedSurface)(IDirectDrawSurface7 *, IDirectDrawSurface7 *);
	HRESULT (*AddOverlayDirtyRect)(IDirectDrawSurface7 *, LPRECT);
	HRESULT (*Blt)(IDirectDrawSurface7 *, LPRECT, IDirectDrawSurface7 *,
		       LPRECT, DWORD, LPVOID);
	HRESULT (*BltBatch)(IDirectDrawSurface7 *, LPVOID, DWORD, DWORD);
	HRESULT (*BltFast)(IDirectDrawSurface7 *, DWORD, DWORD,
			   IDirectDrawSurface7 *, LPRECT, DWORD);
	HRESULT (*DeleteAttachedSurface)(IDirectDrawSurface7 *, DWORD,
					 IDirectDrawSurface7 *);
	HRESULT (*EnumAttachedSurfaces)(IDirectDrawSurface7 *, LPVOID, LPVOID);
	HRESULT (*EnumOverlayZOrders)(IDirectDrawSurface7 *, DWORD, LPVOID, LPVOID);
	HRESULT (*Flip)(IDirectDrawSurface7 *, IDirectDrawSurface7 *, DWORD);
	HRESULT (*GetAttachedSurface)(IDirectDrawSurface7 *, LPDDSCAPS2,
				      IDirectDrawSurface7 **);
	HRESULT (*GetBltStatus)(IDirectDrawSurface7 *, DWORD);
	HRESULT (*GetCaps)(IDirectDrawSurface7 *, LPDDSCAPS2);
	HRESULT (*GetClipper)(IDirectDrawSurface7 *, LPVOID *);
	HRESULT (*GetColorKey)(IDirectDrawSurface7 *, DWORD, LPVOID);
	HRESULT (*GetDC)(IDirectDrawSurface7 *, HDC *);
	HRESULT (*GetFlipStatus)(IDirectDrawSurface7 *, DWORD);
	HRESULT (*GetOverlayPosition)(IDirectDrawSurface7 *, LPLONG, LPLONG);
	HRESULT (*GetPalette)(IDirectDrawSurface7 *, LPVOID *);
	HRESULT (*GetPixelFormat)(IDirectDrawSurface7 *, LPDDPIXELFORMAT);
	HRESULT (*GetSurfaceDesc)(IDirectDrawSurface7 *, LPDDSURFACEDESC2);
	HRESULT (*Initialize)(IDirectDrawSurface7 *, IDirectDraw7 *,
			      LPDDSURFACEDESC2);
	HRESULT (*IsLost)(IDirectDrawSurface7 *);
	HRESULT (*Lock)(IDirectDrawSurface7 *, LPRECT, LPDDSURFACEDESC2,
			DWORD, HANDLE);
	HRESULT (*ReleaseDC)(IDirectDrawSurface7 *, HDC);
	HRESULT (*Restore)(IDirectDrawSurface7 *);
	HRESULT (*SetClipper)(IDirectDrawSurface7 *, LPVOID);
	HRESULT (*SetColorKey)(IDirectDrawSurface7 *, DWORD, LPVOID);
	HRESULT (*SetOverlayPosition)(IDirectDrawSurface7 *, LONG, LONG);
	HRESULT (*SetPalette)(IDirectDrawSurface7 *, LPVOID);
	HRESULT (*Unlock)(IDirectDrawSurface7 *, LPRECT);
	HRESULT (*UpdateOverlay)(IDirectDrawSurface7 *, LPRECT,
				 IDirectDrawSurface7 *, LPRECT, DWORD, LPVOID);
	HRESULT (*UpdateOverlayDisplay)(IDirectDrawSurface7 *, DWORD);
	HRESULT (*UpdateOverlayZOrder)(IDirectDrawSurface7 *, DWORD,
				       IDirectDrawSurface7 *);
};

/* leftover typedefs live at top of file */

static const IDirectDraw7Vtbl s_dd7;
static const IDirectDrawSurface7Vtbl s_surf;
static GXTexObj s_tex;
static int s_tex_ready;
static IDirectDraw7 *s_dd;

static void fill_rgb16(DDPIXELFORMAT *pf)
{
	memset(pf, 0, sizeof(*pf));
	pf->dwSize = sizeof(*pf);
	pf->dwFlags = DDPF_RGB;
	pf->dwRGBBitCount = 16;
	pf->dwRBitMask = 0xF800;
	pf->dwGBitMask = 0x07E0;
	pf->dwBBitMask = 0x001F;
}

static HRESULT dd_unimp(IDirectDraw7 *this, const char *n)
{
	(void)this;
	host_trap(n);
	return E_NOTIMPL;
}

static HRESULT surf_unimp(IDirectDrawSurface7 *this, const char *n)
{
	(void)this;
	host_trap(n);
	return E_NOTIMPL;
}

static HRESULT DD7_QueryInterface(IDirectDraw7 *this, REFIID iid, LPVOID *ppv)
{
	if (!ppv)
		return E_POINTER;
	if (host_guid_eq(iid, &IID_IDirect3D7))
		return host_d3d7_query((LPDIRECTDRAW7)this, iid, ppv);
	this->ref++;
	*ppv = this;
	return S_OK;
}

static ULONG DD7_AddRef(IDirectDraw7 *this)
{
	return (ULONG)++this->ref;
}

static ULONG DD7_Release(IDirectDraw7 *this)
{
	if (--this->ref > 0)
		return (ULONG)this->ref;
	free(this);
	return 0;
}

static HRESULT DD7_Compact(IDirectDraw7 *t) { return dd_unimp(t, "DD.Compact"); }
static HRESULT DD7_CreateClipper(IDirectDraw7 *t, DWORD a, LPVOID *b, LPVOID c)
{
	(void)a; (void)b; (void)c;
	return dd_unimp(t, "DD.CreateClipper");
}
static HRESULT DD7_CreatePalette(IDirectDraw7 *t, DWORD a, LPVOID b, LPVOID *c, LPVOID d)
{
	(void)a; (void)b; (void)c; (void)d;
	return dd_unimp(t, "DD.CreatePalette");
}

static IDirectDrawSurface7 *make_surface(IDirectDraw7 *dd, DWORD w, DWORD h, DWORD caps)
{
	IDirectDrawSurface7 *s;
	u32 bytes;

	s = (IDirectDrawSurface7 *)memalign(32, sizeof(*s));
	if (!s)
		return NULL;
	memset(s, 0, sizeof(*s));
	s->lpVtbl = &s_surf;
	s->ref = 1;
	s->w = w;
	s->h = h;
	s->pitch = w * 2;
	s->caps = caps;
	s->dd = dd;
	bytes = w * h * 2;
	s->pixels = (u16 *)memalign(32, bytes);
	if (!s->pixels) {
		free(s);
		return NULL;
	}
	memset(s->pixels, 0, bytes);
	return s;
}

static HRESULT DD7_CreateSurface(IDirectDraw7 *this, LPDDSURFACEDESC2 desc,
				 LPDIRECTDRAWSURFACE7 *out, LPVOID outer)
{
	DWORD w, h, caps, backs;
	IDirectDrawSurface7 *front;

	(void)outer;
	if (!desc || !out)
		return E_POINTER;
	w = (desc->dwFlags & DDSD_WIDTH) ? desc->dwWidth : this->w;
	h = (desc->dwFlags & DDSD_HEIGHT) ? desc->dwHeight : this->h;
	if (!w)
		w = 640;
	if (!h)
		h = 480;
	caps = desc->ddsCaps.dwCaps;
	backs = (desc->dwFlags & DDSD_BACKBUFFERCOUNT) ? desc->dwBackBufferCount : 0;
	front = make_surface(this, w, h, caps);
	if (!front)
		return E_OUTOFMEMORY;
	if ((caps & DDSCAPS_FLIP) && backs > 0) {
		front->back = make_surface(this, w, h, DDSCAPS_BACKBUFFER);
		if (!front->back) {
			free(front->pixels);
			free(front);
			return E_OUTOFMEMORY;
		}
	}
	host_log("DD CreateSurface %ux%u caps %x", (unsigned)w, (unsigned)h,
		 (unsigned)caps);
	*out = front;
	return DD_OK;
}

static HRESULT DD7_DuplicateSurface(IDirectDraw7 *t, LPDIRECTDRAWSURFACE7 a,
				    LPDIRECTDRAWSURFACE7 *b)
{
	(void)a; (void)b;
	return dd_unimp(t, "DD.DuplicateSurface");
}

static HRESULT DD7_EnumDisplayModes(IDirectDraw7 *this, DWORD flags,
				    LPDDSURFACEDESC2 match, LPVOID ctx,
				    LPDDENUMMODESCALLBACK2 cb)
{
	DDSURFACEDESC2 d;

	(void)flags;
	(void)match;
	if (!cb)
		return E_POINTER;
	memset(&d, 0, sizeof(d));
	d.dwSize = sizeof(d);
	d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
	d.dwWidth = this->w ? this->w : 640;
	d.dwHeight = this->h ? this->h : 480;
	d.lPitch = (LONG)(d.dwWidth * 2);
	fill_rgb16(&d.ddpfPixelFormat);
	host_log("DD EnumDisplayModes %ux%u", (unsigned)d.dwWidth,
		 (unsigned)d.dwHeight);
	cb(&d, ctx);
	return DD_OK;
}

static HRESULT DD7_EnumSurfaces(IDirectDraw7 *t, DWORD a, LPDDSURFACEDESC2 b,
				LPVOID c, LPVOID d)
{
	(void)a; (void)b; (void)c; (void)d;
	return dd_unimp(t, "DD.EnumSurfaces");
}
static HRESULT DD7_FlipToGDISurface(IDirectDraw7 *t)
{
	return dd_unimp(t, "DD.FlipToGDISurface");
}
static HRESULT DD7_GetCaps(IDirectDraw7 *t, LPVOID a, LPVOID b)
{
	(void)a; (void)b;
	return dd_unimp(t, "DD.GetCaps");
}

static HRESULT DD7_GetDisplayMode(IDirectDraw7 *this, LPDDSURFACEDESC2 d)
{
	if (!d)
		return E_POINTER;
	memset(d, 0, sizeof(*d));
	d->dwSize = sizeof(*d);
	d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
	d->dwWidth = this->w;
	d->dwHeight = this->h;
	d->lPitch = (LONG)(this->w * 2);
	fill_rgb16(&d->ddpfPixelFormat);
	host_log("DD GetDisplayMode %ux%u %u", (unsigned)this->w,
		 (unsigned)this->h, (unsigned)this->bpp);
	return DD_OK;
}

static HRESULT DD7_GetFourCCCodes(IDirectDraw7 *t, DWORD *a, DWORD *b)
{
	(void)a; (void)b;
	return dd_unimp(t, "DD.GetFourCCCodes");
}
static HRESULT DD7_GetGDISurface(IDirectDraw7 *t, LPDIRECTDRAWSURFACE7 *a)
{
	(void)a;
	return dd_unimp(t, "DD.GetGDISurface");
}
static HRESULT DD7_GetMonitorFrequency(IDirectDraw7 *t, DWORD *hz)
{
	if (hz)
		*hz = 60;
	(void)t;
	return DD_OK;
}
static HRESULT DD7_GetScanLine(IDirectDraw7 *t, DWORD *a)
{
	(void)a;
	return dd_unimp(t, "DD.GetScanLine");
}
static HRESULT DD7_GetVerticalBlankStatus(IDirectDraw7 *t, BOOL *a)
{
	if (a)
		*a = TRUE;
	(void)t;
	return DD_OK;
}
static HRESULT DD7_Initialize(IDirectDraw7 *t, GUID *g)
{
	(void)t;
	(void)g;
	return DD_OK;
}
static HRESULT DD7_RestoreDisplayMode(IDirectDraw7 *t)
{
	(void)t;
	return DD_OK;
}

static HRESULT DD7_SetCooperativeLevel(IDirectDraw7 *this, HWND hwnd, DWORD flags)
{
	(void)this;
	(void)hwnd;
	host_log("DD SetCooperativeLevel %x", (unsigned)flags);
	return DD_OK;
}

static HRESULT DD7_SetDisplayMode(IDirectDraw7 *this, DWORD w, DWORD h, DWORD bpp,
				  DWORD refresh, DWORD flags)
{
	(void)refresh;
	(void)flags;
	this->w = w ? w : 640;
	this->h = h ? h : 480;
	this->bpp = bpp ? bpp : 16;
	host_log("DD SetDisplayMode %ux%u %u", (unsigned)this->w,
		 (unsigned)this->h, (unsigned)this->bpp);
	return DD_OK;
}

static HRESULT DD7_WaitForVerticalBlank(IDirectDraw7 *t, DWORD a, HANDLE b)
{
	(void)t;
	(void)a;
	(void)b;
	VIDEO_WaitVSync();
	return DD_OK;
}

static HRESULT DD7_GetAvailableVidMem(IDirectDraw7 *t, LPDDSCAPS2 a, DWORD *tot, DWORD *free)
{
	(void)t;
	(void)a;
	if (tot)
		*tot = 8 * 1024 * 1024;
	if (free)
		*free = 6 * 1024 * 1024;
	return DD_OK;
}

static HRESULT DD7_GetSurfaceFromDC(IDirectDraw7 *t, HDC a, LPDIRECTDRAWSURFACE7 *b)
{
	(void)a; (void)b;
	return dd_unimp(t, "DD.GetSurfaceFromDC");
}
static HRESULT DD7_RestoreAllSurfaces(IDirectDraw7 *t)
{
	(void)t;
	return DD_OK;
}
static HRESULT DD7_TestCooperativeLevel(IDirectDraw7 *t)
{
	(void)t;
	return DD_OK;
}

static HRESULT DD7_GetDeviceIdentifier(IDirectDraw7 *this, LPDDDEVICEIDENTIFIER2 id,
				       DWORD flags)
{
	(void)this;
	(void)flags;
	if (!id)
		return E_POINTER;
	memset(id, 0, sizeof(*id));
	snprintf(id->szDriver, sizeof(id->szDriver), "rbo_gx");
	snprintf(id->szDescription, sizeof(id->szDescription), "RBO GX DirectDraw7");
	host_log("DD GetDeviceIdentifier");
	return DD_OK;
}

static HRESULT DD7_StartModeTest(IDirectDraw7 *t, LPSIZE a, DWORD b, DWORD c)
{
	(void)a; (void)b; (void)c;
	return dd_unimp(t, "DD.StartModeTest");
}
static HRESULT DD7_EvaluateMode(IDirectDraw7 *t, DWORD a, DWORD *b)
{
	(void)a; (void)b;
	return dd_unimp(t, "DD.EvaluateMode");
}

static const IDirectDraw7Vtbl s_dd7 = {
	DD7_QueryInterface, DD7_AddRef, DD7_Release, DD7_Compact,
	DD7_CreateClipper, DD7_CreatePalette, DD7_CreateSurface,
	DD7_DuplicateSurface, DD7_EnumDisplayModes, DD7_EnumSurfaces,
	DD7_FlipToGDISurface, DD7_GetCaps, DD7_GetDisplayMode,
	DD7_GetFourCCCodes, DD7_GetGDISurface, DD7_GetMonitorFrequency,
	DD7_GetScanLine, DD7_GetVerticalBlankStatus, DD7_Initialize,
	DD7_RestoreDisplayMode, DD7_SetCooperativeLevel, DD7_SetDisplayMode,
	DD7_WaitForVerticalBlank, DD7_GetAvailableVidMem, DD7_GetSurfaceFromDC,
	DD7_RestoreAllSurfaces, DD7_TestCooperativeLevel, DD7_GetDeviceIdentifier,
	DD7_StartModeTest, DD7_EvaluateMode
};

static HRESULT Surf_QueryInterface(IDirectDrawSurface7 *t, REFIID i, LPVOID *p)
{
	(void)i;
	if (!p)
		return E_POINTER;
	t->ref++;
	*p = t;
	return S_OK;
}
static ULONG Surf_AddRef(IDirectDrawSurface7 *t) { return (ULONG)++t->ref; }
static ULONG Surf_Release(IDirectDrawSurface7 *t)
{
	if (--t->ref > 0)
		return (ULONG)t->ref;
	if (t->back)
		t->back->lpVtbl->Release(t->back);
	free(t->pixels);
	free(t->tiled);
	free(t);
	return 0;
}

static HRESULT Surf_AddAttached(IDirectDrawSurface7 *t, IDirectDrawSurface7 *a)
{
	(void)a;
	return surf_unimp(t, "DDS.AddAttachedSurface");
}
static HRESULT Surf_AddOverlay(IDirectDrawSurface7 *t, LPRECT r)
{
	(void)r;
	return surf_unimp(t, "DDS.AddOverlayDirtyRect");
}

static HRESULT Surf_Blt(IDirectDrawSurface7 *dst, LPRECT dr, IDirectDrawSurface7 *src,
			LPRECT sr, DWORD flags, LPVOID fx)
{
	DWORD w, h, y;

	(void)flags;
	(void)fx;
	if (!src || !dst || !dst->pixels || !src->pixels)
		return E_POINTER;
	w = dst->w;
	h = dst->h;
	if (dr) {
		w = (DWORD)(dr->right - dr->left);
		h = (DWORD)(dr->bottom - dr->top);
	}
	if (w > dst->w)
		w = dst->w;
	if (h > dst->h)
		h = dst->h;
	if (w > src->w)
		w = src->w;
	if (h > src->h)
		h = src->h;
	(void)sr;
	for (y = 0; y < h; y++)
		memcpy(dst->pixels + y * dst->w, src->pixels + y * src->w, w * 2);
	dst->dirty = 1;
	return DD_OK;
}

static HRESULT Surf_BltBatch(IDirectDrawSurface7 *t, LPVOID a, DWORD b, DWORD c)
{
	(void)a; (void)b; (void)c;
	return surf_unimp(t, "DDS.BltBatch");
}
static HRESULT Surf_BltFast(IDirectDrawSurface7 *d, DWORD x, DWORD y,
			    IDirectDrawSurface7 *s, LPRECT r, DWORD f)
{
	(void)x; (void)y; (void)r; (void)f;
	return Surf_Blt(d, NULL, s, NULL, 0, NULL);
}
static HRESULT Surf_DeleteAttached(IDirectDrawSurface7 *t, DWORD a, IDirectDrawSurface7 *b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.DeleteAttachedSurface");
}
static HRESULT Surf_EnumAttached(IDirectDrawSurface7 *t, LPVOID a, LPVOID b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.EnumAttachedSurfaces");
}
static HRESULT Surf_EnumOverlayZ(IDirectDrawSurface7 *t, DWORD a, LPVOID b, LPVOID c)
{
	(void)a; (void)b; (void)c;
	return surf_unimp(t, "DDS.EnumOverlayZOrders");
}

static void rgb565_tile(u8 *dst, const u16 *src, DWORD w, DWORD h)
{
	DWORD x, y, ty;
	u16 *out = (u16 *)dst;

	if ((w & 3) == 0 && (h & 3) == 0) {
		for (y = 0; y < h; y += 4) {
			for (x = 0; x < w; x += 4) {
				const u16 *p = src + y * w + x;

				for (ty = 0; ty < 4; ty++) {
					out[0] = p[0];
					out[1] = p[1];
					out[2] = p[2];
					out[3] = p[3];
					out += 4;
					p += w;
				}
			}
		}
		return;
	}
	for (y = 0; y < h; y += 4) {
		for (x = 0; x < w; x += 4) {
			DWORD tx;

			for (ty = 0; ty < 4; ty++) {
				for (tx = 0; tx < 4; tx++) {
					DWORD sx = x + tx;
					DWORD sy = y + ty;
					*out++ = (sx < w && sy < h) ? src[sy * w + sx] : 0;
				}
			}
		}
	}
}

static void present_surface(IDirectDrawSurface7 *vis)
{
	u32 nbytes;

	if (!vis || !vis->pixels)
		return;
	nbytes = vis->w * vis->h * 2;
	if (!vis->tiled) {
		vis->tiled = (u8 *)memalign(32, nbytes);
		if (!vis->tiled)
			return;
	}
	rgb565_tile(vis->tiled, vis->pixels, vis->w, vis->h);
	DCFlushRange(vis->tiled, nbytes);
	GX_InitTexObj(&s_tex, vis->tiled, vis->w, vis->h, GX_TF_RGB565,
		      GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&s_tex, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE,
			 GX_ANISO_1);
	s_tex_ready = 1;
	rbo_gx_apply_vtxfmt0();
	GX_SetNumTexGens(1);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	rbo_gx_set_ui_ortho();
	rbo_gx_blit_fullscreen(&s_tex);
}

static HRESULT Surf_Flip(IDirectDrawSurface7 *this, IDirectDrawSurface7 *other,
			 DWORD flags)
{
	IDirectDrawSurface7 *vis;
	u16 *tmp;

	(void)other;
	(void)flags;
	/* Show the backbuffer D3D just drew. Do not Blt it onto the primary
	 * first: that memcpy of 640x480 was a whole extra frame of work. */
	vis = this->back ? this->back : this;
	present_surface(vis);
	host_log_draw();
	host_present_efb();
	host_note_flip();
	if (this->back && this->pixels && this->back->pixels) {
		tmp = this->pixels;
		this->pixels = this->back->pixels;
		this->back->pixels = tmp;
	}
	return DD_OK;
}

static HRESULT Surf_GetAttached(IDirectDrawSurface7 *this, LPDDSCAPS2 caps,
				IDirectDrawSurface7 **out)
{
	if (!out)
		return E_POINTER;
	if (caps && (caps->dwCaps & DDSCAPS_BACKBUFFER) && this->back) {
		this->back->ref++;
		*out = this->back;
		return DD_OK;
	}
	return E_NOINTERFACE;
}

static HRESULT Surf_GetBltStatus(IDirectDrawSurface7 *t, DWORD a)
{
	(void)t;
	(void)a;
	return DD_OK;
}
static HRESULT Surf_GetCaps(IDirectDrawSurface7 *t, LPDDSCAPS2 c)
{
	if (!c)
		return E_POINTER;
	memset(c, 0, sizeof(*c));
	c->dwCaps = t->caps;
	return DD_OK;
}
static HRESULT Surf_GetClipper(IDirectDrawSurface7 *t, LPVOID *a)
{
	(void)a;
	return surf_unimp(t, "DDS.GetClipper");
}
static HRESULT Surf_GetColorKey(IDirectDrawSurface7 *t, DWORD a, LPVOID b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.GetColorKey");
}
static HRESULT Surf_GetDC(IDirectDrawSurface7 *t, HDC *dc)
{
	if (!dc)
		return E_POINTER;
	*dc = (HDC)t;
	return DD_OK;
}
static HRESULT Surf_GetFlipStatus(IDirectDrawSurface7 *t, DWORD a)
{
	(void)t;
	(void)a;
	return DD_OK;
}
static HRESULT Surf_GetOverlayPos(IDirectDrawSurface7 *t, LPLONG a, LPLONG b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.GetOverlayPosition");
}
static HRESULT Surf_GetPalette(IDirectDrawSurface7 *t, LPVOID *a)
{
	(void)a;
	return surf_unimp(t, "DDS.GetPalette");
}
static HRESULT Surf_GetPixelFormat(IDirectDrawSurface7 *t, LPDDPIXELFORMAT pf)
{
	(void)t;
	if (!pf)
		return E_POINTER;
	fill_rgb16(pf);
	return DD_OK;
}
static HRESULT Surf_GetSurfaceDesc(IDirectDrawSurface7 *t, LPDDSURFACEDESC2 d)
{
	if (!d)
		return E_POINTER;
	memset(d, 0, sizeof(*d));
	d->dwSize = sizeof(*d);
	d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_CAPS | DDSD_PIXELFORMAT;
	d->dwWidth = t->w;
	d->dwHeight = t->h;
	d->lPitch = (LONG)t->pitch;
	d->lpSurface = t->pixels;
	d->ddsCaps.dwCaps = t->caps;
	fill_rgb16(&d->ddpfPixelFormat);
	return DD_OK;
}
static HRESULT Surf_Initialize(IDirectDrawSurface7 *t, IDirectDraw7 *a, LPDDSURFACEDESC2 b)
{
	(void)t; (void)a; (void)b;
	return DD_OK;
}
static HRESULT Surf_IsLost(IDirectDrawSurface7 *t)
{
	(void)t;
	return DD_OK;
}
static HRESULT Surf_Lock(IDirectDrawSurface7 *t, LPRECT r, LPDDSURFACEDESC2 d,
			 DWORD flags, HANDLE ev)
{
	(void)r;
	(void)flags;
	(void)ev;
	if (!d)
		return E_POINTER;
	Surf_GetSurfaceDesc(t, d);
	d->dwFlags |= DDSD_PITCH;
	d->lpSurface = t->pixels;
	return DD_OK;
}
static HRESULT Surf_ReleaseDC(IDirectDrawSurface7 *t, HDC dc)
{
	(void)dc;
	t->dirty = 1;
	return DD_OK;
}
static HRESULT Surf_Restore(IDirectDrawSurface7 *t)
{
	(void)t;
	return DD_OK;
}
static HRESULT Surf_SetClipper(IDirectDrawSurface7 *t, LPVOID a)
{
	(void)a;
	return surf_unimp(t, "DDS.SetClipper");
}
static HRESULT Surf_SetColorKey(IDirectDrawSurface7 *t, DWORD a, LPVOID b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.SetColorKey");
}
static HRESULT Surf_SetOverlayPos(IDirectDrawSurface7 *t, LONG a, LONG b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.SetOverlayPosition");
}
static HRESULT Surf_SetPalette(IDirectDrawSurface7 *t, LPVOID a)
{
	(void)a;
	return surf_unimp(t, "DDS.SetPalette");
}
static HRESULT Surf_Unlock(IDirectDrawSurface7 *t, LPRECT r)
{
	(void)r;
	t->dirty = 1;
	return DD_OK;
}
static HRESULT Surf_UpdateOverlay(IDirectDrawSurface7 *t, LPRECT a,
				  IDirectDrawSurface7 *b, LPRECT c, DWORD d, LPVOID e)
{
	(void)a; (void)b; (void)c; (void)d; (void)e;
	return surf_unimp(t, "DDS.UpdateOverlay");
}
static HRESULT Surf_UpdateOverlayDisplay(IDirectDrawSurface7 *t, DWORD a)
{
	(void)a;
	return surf_unimp(t, "DDS.UpdateOverlayDisplay");
}
static HRESULT Surf_UpdateOverlayZOrder(IDirectDrawSurface7 *t, DWORD a,
					IDirectDrawSurface7 *b)
{
	(void)a; (void)b;
	return surf_unimp(t, "DDS.UpdateOverlayZOrder");
}

static const IDirectDrawSurface7Vtbl s_surf = {
	Surf_QueryInterface, Surf_AddRef, Surf_Release, Surf_AddAttached,
	Surf_AddOverlay, Surf_Blt, Surf_BltBatch, Surf_BltFast,
	Surf_DeleteAttached, Surf_EnumAttached, Surf_EnumOverlayZ, Surf_Flip,
	Surf_GetAttached, Surf_GetBltStatus, Surf_GetCaps, Surf_GetClipper,
	Surf_GetColorKey, Surf_GetDC, Surf_GetFlipStatus, Surf_GetOverlayPos,
	Surf_GetPalette, Surf_GetPixelFormat, Surf_GetSurfaceDesc,
	Surf_Initialize, Surf_IsLost, Surf_Lock, Surf_ReleaseDC, Surf_Restore,
	Surf_SetClipper, Surf_SetColorKey, Surf_SetOverlayPos, Surf_SetPalette,
	Surf_Unlock, Surf_UpdateOverlay, Surf_UpdateOverlayDisplay,
	Surf_UpdateOverlayZOrder
};

int host_surf_rgb565(LPDIRECTDRAWSURFACE7 s, unsigned short **px, DWORD *w,
		     DWORD *h)
{
	IDirectDrawSurface7 *t = (IDirectDrawSurface7 *)s;

	if (!t || !t->pixels)
		return 0;
	if (px)
		*px = t->pixels;
	if (w)
		*w = t->w;
	if (h)
		*h = t->h;
	return 1;
}

void host_surf_release(void *s)
{
	IDirectDrawSurface7 *t = (IDirectDrawSurface7 *)s;

	if (t && t->lpVtbl && t->lpVtbl->Release)
		t->lpVtbl->Release(t);
}

LPDIRECTDRAWSURFACE7 host_dd_create_rgb565(DWORD w, DWORD h)
{
	DDSURFACEDESC2 desc;
	LPDIRECTDRAWSURFACE7 s;

	if (!s_dd || !w || !h)
		return NULL;
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
	desc.dwWidth = w;
	desc.dwHeight = h;
	desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY |
			      DDSCAPS_TEXTURE;
	fill_rgb16(&desc.ddpfPixelFormat);
	s = NULL;
	if (DD7_CreateSurface(s_dd, &desc, &s, NULL) != DD_OK)
		return NULL;
	return s;
}

HRESULT DirectDrawCreateEx(GUID *guid, LPVOID *ppv, REFIID iid, LPVOID unk)
{
	IDirectDraw7 *dd;

	(void)guid;
	(void)iid;
	(void)unk;
	if (!ppv)
		return E_POINTER;
	dd = (IDirectDraw7 *)memalign(32, sizeof(*dd));
	if (!dd)
		return E_OUTOFMEMORY;
	memset(dd, 0, sizeof(*dd));
	dd->lpVtbl = &s_dd7;
	dd->ref = 1;
	s_dd = dd;
	dd->w = 640;
	dd->h = 480;
	dd->bpp = 16;
	*ppv = dd;
	host_log("DirectDrawCreateEx");
	return DD_OK;
}
