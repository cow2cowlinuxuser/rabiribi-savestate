#include <malloc.h>
#include <math.h>
#include <string.h>

#include <gctypes.h>

#include "d3d.h"
#include "host.h"

typedef struct IDirect3D7 IDirect3D7;
typedef struct IDirect3DDevice7 IDirect3DDevice7;
typedef struct IDirect3D7Vtbl IDirect3D7Vtbl;
typedef struct IDirect3DDevice7Vtbl IDirect3DDevice7Vtbl;

struct IDirect3D7 {
	const IDirect3D7Vtbl *lpVtbl;
	LONG ref;
	LPDIRECTDRAW7 dd;
};

struct IDirect3DDevice7 {
	const IDirect3DDevice7Vtbl *lpVtbl;
	LONG ref;
	IDirect3D7 *d3d;
	LPDIRECTDRAWSURFACE7 target;
	LPDIRECTDRAWSURFACE7 tex0;
};

struct IDirect3D7Vtbl {
	HRESULT (*QueryInterface)(IDirect3D7 *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirect3D7 *);
	ULONG (*Release)(IDirect3D7 *);
	HRESULT (*EnumDevices)(IDirect3D7 *, LPVOID, LPVOID);
	HRESULT (*CreateDevice)(IDirect3D7 *, REFCLSID, LPDIRECTDRAWSURFACE7,
				LPDIRECT3DDEVICE7 *);
	HRESULT (*CreateVertexBuffer)(IDirect3D7 *, LPVOID, LPVOID *, DWORD);
	HRESULT (*EnumZBufferFormats)(IDirect3D7 *, REFCLSID, LPVOID, LPVOID);
	HRESULT (*EvictManagedTextures)(IDirect3D7 *);
};

struct IDirect3DDevice7Vtbl {
	HRESULT (*QueryInterface)(IDirect3DDevice7 *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirect3DDevice7 *);
	ULONG (*Release)(IDirect3DDevice7 *);
	HRESULT (*GetCaps)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*EnumTextureFormats)(IDirect3DDevice7 *, LPVOID, LPVOID);
	HRESULT (*BeginScene)(IDirect3DDevice7 *);
	HRESULT (*EndScene)(IDirect3DDevice7 *);
	HRESULT (*GetDirect3D)(IDirect3DDevice7 *, LPDIRECT3D7 *);
	HRESULT (*SetRenderTarget)(IDirect3DDevice7 *, LPDIRECTDRAWSURFACE7, DWORD);
	HRESULT (*GetRenderTarget)(IDirect3DDevice7 *, LPDIRECTDRAWSURFACE7 *);
	HRESULT (*Clear)(IDirect3DDevice7 *, DWORD, LPVOID, DWORD, DWORD, float,
			 DWORD);
	HRESULT (*SetTransform)(IDirect3DDevice7 *, DWORD, LPVOID);
	HRESULT (*GetTransform)(IDirect3DDevice7 *, DWORD, LPVOID);
	HRESULT (*SetViewport)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*MultiplyTransform)(IDirect3DDevice7 *, DWORD, LPVOID);
	HRESULT (*GetViewport)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*SetMaterial)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*GetMaterial)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*SetLight)(IDirect3DDevice7 *, DWORD, LPVOID);
	HRESULT (*GetLight)(IDirect3DDevice7 *, DWORD, LPVOID);
	HRESULT (*SetRenderState)(IDirect3DDevice7 *, DWORD, DWORD);
	HRESULT (*GetRenderState)(IDirect3DDevice7 *, DWORD, DWORD *);
	HRESULT (*BeginStateBlock)(IDirect3DDevice7 *);
	HRESULT (*EndStateBlock)(IDirect3DDevice7 *, DWORD *);
	HRESULT (*PreLoad)(IDirect3DDevice7 *, LPDIRECTDRAWSURFACE7);
	HRESULT (*DrawPrimitive)(IDirect3DDevice7 *, DWORD, DWORD, LPVOID, DWORD,
				 DWORD);
	HRESULT (*DrawIndexedPrimitive)(IDirect3DDevice7 *, DWORD, DWORD, LPVOID,
					DWORD, LPVOID, DWORD, DWORD);
	HRESULT (*SetClipStatus)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*GetClipStatus)(IDirect3DDevice7 *, LPVOID);
	HRESULT (*DrawPrimitiveStrided)(IDirect3DDevice7 *, DWORD, DWORD, LPVOID,
					DWORD, DWORD);
	HRESULT (*DrawIndexedPrimitiveStrided)(IDirect3DDevice7 *, DWORD, DWORD,
					       LPVOID, DWORD, LPVOID, DWORD, DWORD);
	HRESULT (*DrawPrimitiveVB)(IDirect3DDevice7 *, DWORD, LPVOID, DWORD, DWORD,
				   DWORD);
	HRESULT (*DrawIndexedPrimitiveVB)(IDirect3DDevice7 *, DWORD, LPVOID, DWORD,
					  DWORD, LPVOID, DWORD, DWORD);
	HRESULT (*ComputeSphereVisibility)(IDirect3DDevice7 *, LPVOID, LPVOID,
					   DWORD, DWORD, DWORD *);
	HRESULT (*GetTexture)(IDirect3DDevice7 *, DWORD, LPDIRECTDRAWSURFACE7 *);
	HRESULT (*SetTexture)(IDirect3DDevice7 *, DWORD, LPDIRECTDRAWSURFACE7);
	HRESULT (*GetTextureStageState)(IDirect3DDevice7 *, DWORD, DWORD, DWORD *);
	HRESULT (*SetTextureStageState)(IDirect3DDevice7 *, DWORD, DWORD, DWORD);
	HRESULT (*ValidateDevice)(IDirect3DDevice7 *, DWORD *);
	HRESULT (*ApplyStateBlock)(IDirect3DDevice7 *, DWORD);
	HRESULT (*CaptureStateBlock)(IDirect3DDevice7 *, DWORD);
	HRESULT (*DeleteStateBlock)(IDirect3DDevice7 *, DWORD);
	HRESULT (*CreateStateBlock)(IDirect3DDevice7 *, DWORD, DWORD *);
	HRESULT (*Load)(IDirect3DDevice7 *, LPDIRECTDRAWSURFACE7, LPVOID,
			LPDIRECTDRAWSURFACE7, LPVOID, DWORD);
	HRESULT (*LightEnable)(IDirect3DDevice7 *, DWORD, BOOL);
	HRESULT (*GetLightEnable)(IDirect3DDevice7 *, DWORD, BOOL *);
	HRESULT (*SetClipPlane)(IDirect3DDevice7 *, DWORD, float *);
	HRESULT (*GetClipPlane)(IDirect3DDevice7 *, DWORD, float *);
	HRESULT (*GetInfo)(IDirect3DDevice7 *, DWORD, LPVOID, DWORD);
};

typedef struct {
	float x, y, z, rhw;
	DWORD diff, spec;
	float u, v;
} D3DVtx;

const GUID IID_IDirect3D7 = {
	0xf5049e77, 0x4861, 0x11d2,
	{0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}
};
const GUID IID_IDirect3DRGBDevice = {
	0xa4665c60, 0x2673, 0x11cf,
	{0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}
};

static const IDirect3D7Vtbl s_d3d7;
static const IDirect3DDevice7Vtbl s_dev;

int host_guid_eq(REFIID a, const GUID *b)
{
	if (!a || !b)
		return 0;
	return memcmp(a, b, sizeof(GUID)) == 0;
}

static HRESULT d3d_unimp(IDirect3D7 *t, const char *n)
{
	(void)t;
	host_trap(n);
	return E_NOTIMPL;
}

static HRESULT dev_unimp(IDirect3DDevice7 *t, const char *n)
{
	(void)t;
	host_trap(n);
	return E_NOTIMPL;
}

static HRESULT D3D7_QI(IDirect3D7 *this, REFIID iid, LPVOID *ppv)
{
	(void)iid;
	if (!ppv)
		return E_POINTER;
	this->ref++;
	*ppv = this;
	return S_OK;
}

static ULONG D3D7_AddRef(IDirect3D7 *this)
{
	return (ULONG)++this->ref;
}

static ULONG D3D7_Release(IDirect3D7 *this)
{
	if (--this->ref > 0)
		return (ULONG)this->ref;
	free(this);
	return 0;
}

static HRESULT D3D7_EnumDevices(IDirect3D7 *t, LPVOID cb, LPVOID ctx)
{
	(void)t;
	(void)cb;
	(void)ctx;
	return D3D_OK;
}

static HRESULT D3D7_CreateVB(IDirect3D7 *t, LPVOID a, LPVOID *b, DWORD c)
{
	(void)a;
	(void)b;
	(void)c;
	return d3d_unimp(t, "D3D.CreateVertexBuffer");
}

static HRESULT D3D7_EnumZ(IDirect3D7 *t, REFCLSID g, LPVOID cb, LPVOID ctx)
{
	(void)g;
	(void)cb;
	(void)ctx;
	return D3D_OK;
}

static HRESULT D3D7_Evict(IDirect3D7 *t)
{
	(void)t;
	return D3D_OK;
}

static HRESULT Dev_QI(IDirect3DDevice7 *this, REFIID iid, LPVOID *ppv)
{
	(void)iid;
	if (!ppv)
		return E_POINTER;
	this->ref++;
	*ppv = this;
	return S_OK;
}

static ULONG Dev_AddRef(IDirect3DDevice7 *this)
{
	return (ULONG)++this->ref;
}

static ULONG Dev_Release(IDirect3DDevice7 *this)
{
	if (--this->ref > 0)
		return (ULONG)this->ref;
	free(this);
	return 0;
}

static HRESULT Dev_GetCaps(IDirect3DDevice7 *this, LPVOID desc)
{
	unsigned char *p;

	(void)this;
	if (!desc)
		return E_POINTER;
	p = (unsigned char *)desc;
	memset(p, 0, 252);
	/* dwDevCaps */
	*(DWORD *)p = 0x00000041;
	/* dwDeviceRenderBitDepth at +0x9c (after two 0x4c PRIMCAPS) */
	*(DWORD *)(p + 0x9c) = DDBD_16;
	*(DWORD *)(p + 0xa4) = 1;
	*(DWORD *)(p + 0xa8) = 1;
	*(DWORD *)(p + 0xac) = 1024;
	*(DWORD *)(p + 0xb0) = 1024;
	return D3D_OK;
}

static HRESULT Dev_EnumTex(IDirect3DDevice7 *t, LPVOID cb, LPVOID ctx)
{
	(void)t;
	(void)cb;
	(void)ctx;
	return D3D_OK;
}

static HRESULT Dev_BeginScene(IDirect3DDevice7 *t)
{
	u16 *fb;
	DWORD w, h;

	if (t && t->target && host_surf_rgb565(t->target, &fb, &w, &h) && fb)
		memset(fb, 0, (size_t)w * (size_t)h * 2u);
	return D3D_OK;
}

static HRESULT Dev_EndScene(IDirect3DDevice7 *t)
{
	(void)t;
	return D3D_OK;
}

static HRESULT Dev_GetDirect3D(IDirect3DDevice7 *this, LPDIRECT3D7 *out)
{
	if (!out)
		return E_POINTER;
	this->d3d->ref++;
	*out = this->d3d;
	return D3D_OK;
}

static HRESULT Dev_SetRT(IDirect3DDevice7 *this, LPDIRECTDRAWSURFACE7 s, DWORD f)
{
	(void)f;
	this->target = s;
	return D3D_OK;
}

static HRESULT Dev_GetRT(IDirect3DDevice7 *this, LPDIRECTDRAWSURFACE7 *out)
{
	if (!out)
		return E_POINTER;
	*out = this->target;
	return D3D_OK;
}

static HRESULT Dev_Clear(IDirect3DDevice7 *t, DWORD n, LPVOID r, DWORD f,
			 DWORD c, float z, DWORD s)
{
	(void)t;
	(void)n;
	(void)r;
	(void)f;
	(void)c;
	(void)z;
	(void)s;
	return D3D_OK;
}

static HRESULT Dev_Ok1(IDirect3DDevice7 *t, DWORD a, LPVOID b)
{
	(void)t;
	(void)a;
	(void)b;
	return D3D_OK;
}

static HRESULT Dev_Ok0(IDirect3DDevice7 *t, LPVOID a)
{
	(void)t;
	(void)a;
	return D3D_OK;
}

static HRESULT Dev_SetRS(IDirect3DDevice7 *t, DWORD s, DWORD v)
{
	(void)t;
	(void)s;
	(void)v;
	return D3D_OK;
}

static HRESULT Dev_GetRS(IDirect3DDevice7 *t, DWORD s, DWORD *v)
{
	(void)t;
	(void)s;
	if (v)
		*v = 0;
	return D3D_OK;
}

static HRESULT Dev_BeginSB(IDirect3DDevice7 *t)
{
	(void)t;
	return D3D_OK;
}

static HRESULT Dev_EndSB(IDirect3DDevice7 *t, DWORD *h)
{
	(void)t;
	if (h)
		*h = 0;
	return D3D_OK;
}

static HRESULT Dev_PreLoad(IDirect3DDevice7 *t, LPDIRECTDRAWSURFACE7 s)
{
	(void)t;
	(void)s;
	return D3D_OK;
}

static u16 rgb565_d3d(DWORD c)
{
	unsigned r = (c >> 16) & 0xff;
	unsigned g = (c >> 8) & 0xff;
	unsigned b = c & 0xff;

	return (u16)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static int nearly(float a, float b)
{
	float d = a - b;

	return d < 0.75f && d > -0.75f;
}

static void plot565(u16 *fb, DWORD w, DWORD h, int x, int y, u16 c)
{
	if ((unsigned)x < w && (unsigned)y < h)
		fb[y * w + x] = c;
}

/* Title sprites are screen-aligned strips (TL,TR,BL,BR). The barycentric
 * filler scans each triangle's AABB, so a 640x480 quad tests ~600k pixels
 * in float. One nearest blit is the same image at interactive rate. */
static void blit_aabb(u16 *fb, DWORD w, DWORD h, int x0, int y0, int x1, int y1,
		      u16 *tex, DWORD tw, DWORD th, float u0, float v0,
		      float u1, float v1, u16 solid)
{
	int x, y, dw, dh, su, sv, du, dv, u, v, sx, sy;
	u16 pix, *row, *srcrow;

	if (x0 >= (int)w || y0 >= (int)h || x1 <= 0 || y1 <= 0)
		return;
	dw = x1 - x0;
	dh = y1 - y0;
	if (dw <= 0 || dh <= 0)
		return;

	if (!tex || !tw || !th) {
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > (int)w)
			x1 = (int)w;
		if (y1 > (int)h)
			y1 = (int)h;
		for (y = y0; y < y1; y++) {
			row = fb + y * (int)w + x0;
			for (x = x0; x < x1; x++)
				*row++ = solid;
		}
		return;
	}

	su = (int)(u0 * (float)tw * 65536.f);
	sv = (int)(v0 * (float)th * 65536.f);
	du = (int)(((u1 - u0) * (float)tw * 65536.f) / (float)dw);
	dv = (int)(((v1 - v0) * (float)th * 65536.f) / (float)dh);
	if (x0 < 0) {
		su += du * -x0;
		x0 = 0;
	}
	if (y0 < 0) {
		sv += dv * -y0;
		y0 = 0;
	}
	if (x1 > (int)w)
		x1 = (int)w;
	if (y1 > (int)h)
		y1 = (int)h;

	v = sv;
	for (y = y0; y < y1; y++, v += dv) {
		sy = v >> 16;
		if (sy < 0)
			sy = 0;
		if (sy >= (int)th)
			sy = (int)th - 1;
		srcrow = tex + sy * (int)tw;
		row = fb + y * (int)w + x0;
		u = su;
		for (x = x0; x < x1; x++, u += du) {
			sx = u >> 16;
			if (sx < 0)
				sx = 0;
			else if (sx >= (int)tw)
				sx = (int)tw - 1;
			pix = srcrow[sx];
			if (pix)
				*row = pix;
			row++;
		}
	}
}

static int aabb_from_strip4(const D3DVtx *q, int *x0, int *y0, int *x1, int *y1)
{
	if (!nearly(q[0].x, q[2].x) || !nearly(q[1].x, q[3].x) ||
	    !nearly(q[0].y, q[1].y) || !nearly(q[2].y, q[3].y))
		return 0;
	*x0 = (int)q[0].x;
	*y0 = (int)q[0].y;
	*x1 = (int)q[3].x;
	*y1 = (int)q[3].y;
	if (*x1 < *x0) {
		int t = *x0;

		*x0 = *x1;
		*x1 = t;
	}
	if (*y1 < *y0) {
		int t = *y0;

		*y0 = *y1;
		*y1 = t;
	}
	return *x1 > *x0 && *y1 > *y0;
}

static u16 sample565(u16 *tex, DWORD tw, DWORD th, float u, float v)
{
	int x, y;

	x = (int)(u * (float)tw);
	y = (int)(v * (float)th);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= (int)tw)
		x = (int)tw - 1;
	if (y >= (int)th)
		y = (int)th - 1;
	return tex[y * tw + x];
}

static void fill_tri(u16 *fb, DWORD w, DWORD h, const D3DVtx *a, const D3DVtx *b,
		     const D3DVtx *c, u16 *tex, DWORD tw, DWORD th)
{
	int minx, maxx, miny, maxy, x, y;
	float x0, y0, x1, y1, x2, y2, area, w0, w1, w2, u, v;
	u16 solid, pix;

	x0 = a->x;
	y0 = a->y;
	x1 = b->x;
	y1 = b->y;
	x2 = c->x;
	y2 = c->y;
	area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (area == 0.f)
		return;
	minx = (int)floorf(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
	maxx = (int)ceilf(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
	miny = (int)floorf(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
	maxy = (int)ceilf(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
	if (minx < 0)
		minx = 0;
	if (miny < 0)
		miny = 0;
	if (maxx >= (int)w)
		maxx = (int)w - 1;
	if (maxy >= (int)h)
		maxy = (int)h - 1;
	solid = rgb565_d3d(a->diff);
	for (y = miny; y <= maxy; y++) {
		for (x = minx; x <= maxx; x++) {
			float px = (float)x + 0.5f;
			float py = (float)y + 0.5f;

			w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
			w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
			w2 = ((x0 - px) * (y1 - py) - (x1 - px) * (y0 - py)) / area;
			if (!((w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) ||
			      (w0 <= 0.f && w1 <= 0.f && w2 <= 0.f)))
				continue;
			if (tex && tw && th) {
				u = w0 * a->u + w1 * b->u + w2 * c->u;
				v = w0 * a->v + w1 * b->v + w2 * c->v;
				pix = sample565(tex, tw, th, u, v);
				if (pix == 0)
					continue;
			} else {
				pix = solid;
			}
			plot565(fb, w, h, x, y, pix);
		}
	}
}

static const D3DVtx *vtx_at(const void *verts, DWORD i)
{
	return (const D3DVtx *)((const char *)verts + i * sizeof(D3DVtx));
}

static void draw_tri_ix(IDirect3DDevice7 *this, const void *verts, DWORD i0,
			DWORD i1, DWORD i2)
{
	u16 *fb, *tex;
	DWORD w, h, tw, th;

	if (!host_surf_rgb565(this->target, &fb, &w, &h))
		return;
	tex = NULL;
	tw = th = 0;
	if (this->tex0)
		host_surf_rgb565(this->tex0, &tex, &tw, &th);
	fill_tri(fb, w, h, vtx_at(verts, i0), vtx_at(verts, i1), vtx_at(verts, i2),
		 tex, tw, th);
}

static HRESULT draw_indexed(IDirect3DDevice7 *this, DWORD prim, const void *verts,
			    DWORD vcount, const unsigned short *idx, DWORD icount)
{
	DWORD i;
	unsigned short i0, i1, i2;
	D3DVtx q[4];
	int x0, y0, x1, y1;
	u16 *fb, *tex;
	DWORD w, h, tw, th;

	(void)vcount;
	if (!this->target || !verts)
		return D3D_OK;
	if (prim == D3DPT_TRIANGLESTRIP && icount >= 4 && idx) {
		q[0] = *vtx_at(verts, idx[0]);
		q[1] = *vtx_at(verts, idx[1]);
		q[2] = *vtx_at(verts, idx[2]);
		q[3] = *vtx_at(verts, idx[3]);
		if (aabb_from_strip4(q, &x0, &y0, &x1, &y1) &&
		    host_surf_rgb565(this->target, &fb, &w, &h)) {
			tex = NULL;
			tw = th = 0;
			if (this->tex0)
				host_surf_rgb565(this->tex0, &tex, &tw, &th);
			blit_aabb(fb, w, h, x0, y0, x1, y1, tex, tw, th,
				  q[0].u, q[0].v, q[3].u, q[3].v,
				  rgb565_d3d(q[0].diff));
			return D3D_OK;
		}
	}
	if (prim == D3DPT_TRIANGLELIST) {
		for (i = 0; i + 2 < icount; i += 3)
			draw_tri_ix(this, verts, idx[i], idx[i + 1], idx[i + 2]);
	} else if (prim == D3DPT_TRIANGLESTRIP) {
		for (i = 0; i + 2 < icount; i++) {
			i0 = idx[i];
			i1 = idx[i + 1];
			i2 = idx[i + 2];
			if (i & 1) {
				unsigned short t = i1;

				i1 = i2;
				i2 = t;
			}
			draw_tri_ix(this, verts, i0, i1, i2);
		}
	} else if (prim == D3DPT_TRIANGLEFAN) {
		i0 = idx[0];
		for (i = 1; i + 1 < icount; i++)
			draw_tri_ix(this, verts, i0, idx[i], idx[i + 1]);
	}
	return D3D_OK;
}

static HRESULT Dev_DrawPrim(IDirect3DDevice7 *this, DWORD prim, DWORD fvf,
			    LPVOID verts, DWORD count, DWORD flags)
{
	unsigned short tmp[8];
	DWORD i, n;

	(void)fvf;
	(void)flags;
	n = count;
	if (n > 8)
		n = 8;
	for (i = 0; i < n; i++)
		tmp[i] = (unsigned short)i;
	return draw_indexed(this, prim, verts, count, tmp, n);
}

static HRESULT Dev_DrawIndexed(IDirect3DDevice7 *this, DWORD prim, DWORD fvf,
			       LPVOID verts, DWORD vcount, LPVOID indices,
			       DWORD icount, DWORD flags)
{
	static const unsigned short fan4[4] = {0, 1, 2, 3};
	const unsigned short *idx = (const unsigned short *)indices;

	(void)fvf;
	(void)flags;
	if (!idx || icount == 0) {
		idx = fan4;
		icount = (vcount < 4) ? vcount : 4;
	}
	return draw_indexed(this, prim, verts, vcount, idx, icount);
}

static HRESULT Dev_GetTex(IDirect3DDevice7 *this, DWORD stage,
			  LPDIRECTDRAWSURFACE7 *out)
{
	if (!out)
		return E_POINTER;
	*out = (stage == 0) ? this->tex0 : NULL;
	return D3D_OK;
}

static HRESULT Dev_SetTex(IDirect3DDevice7 *this, DWORD stage,
			  LPDIRECTDRAWSURFACE7 s)
{
	if (stage == 0)
		this->tex0 = s;
	return D3D_OK;
}

static HRESULT Dev_GetTSS(IDirect3DDevice7 *t, DWORD a, DWORD b, DWORD *c)
{
	(void)t;
	(void)a;
	(void)b;
	if (c)
		*c = 0;
	return D3D_OK;
}

static HRESULT Dev_SetTSS(IDirect3DDevice7 *t, DWORD a, DWORD b, DWORD c)
{
	(void)t;
	(void)a;
	(void)b;
	(void)c;
	return D3D_OK;
}

static HRESULT Dev_Validate(IDirect3DDevice7 *t, DWORD *n)
{
	(void)t;
	if (n)
		*n = 1;
	return D3D_OK;
}

static HRESULT Dev_SB(IDirect3DDevice7 *t, DWORD h)
{
	(void)t;
	(void)h;
	return D3D_OK;
}

static HRESULT Dev_CreateSB(IDirect3DDevice7 *t, DWORD ty, DWORD *h)
{
	(void)t;
	(void)ty;
	if (h)
		*h = 0;
	return D3D_OK;
}

static HRESULT Dev_Load(IDirect3DDevice7 *t, LPDIRECTDRAWSURFACE7 a, LPVOID b,
			LPDIRECTDRAWSURFACE7 c, LPVOID d, DWORD e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return dev_unimp(t, "D3DDev.Load");
}

static HRESULT Dev_LightEn(IDirect3DDevice7 *t, DWORD a, BOOL b)
{
	(void)t;
	(void)a;
	(void)b;
	return D3D_OK;
}

static HRESULT Dev_GetLightEn(IDirect3DDevice7 *t, DWORD a, BOOL *b)
{
	(void)t;
	(void)a;
	if (b)
		*b = 0;
	return D3D_OK;
}

static HRESULT Dev_SetClip(IDirect3DDevice7 *t, DWORD a, float *b)
{
	(void)t;
	(void)a;
	(void)b;
	return D3D_OK;
}

static HRESULT Dev_GetClip(IDirect3DDevice7 *t, DWORD a, float *b)
{
	(void)t;
	(void)a;
	(void)b;
	return D3D_OK;
}

static HRESULT Dev_GetInfo(IDirect3DDevice7 *t, DWORD a, LPVOID b, DWORD c)
{
	(void)t;
	(void)a;
	(void)b;
	(void)c;
	return D3D_OK;
}

static HRESULT Dev_DrawStrided(IDirect3DDevice7 *t, DWORD a, DWORD b, LPVOID c,
			       DWORD d, DWORD e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return dev_unimp(t, "D3DDev.DrawStrided");
}

static HRESULT Dev_DrawIStrided(IDirect3DDevice7 *t, DWORD a, DWORD b, LPVOID c,
				DWORD d, LPVOID e, DWORD f, DWORD g)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	return dev_unimp(t, "D3DDev.DrawIStrided");
}

static HRESULT Dev_DrawVB(IDirect3DDevice7 *t, DWORD a, LPVOID b, DWORD c,
			  DWORD d, DWORD e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return dev_unimp(t, "D3DDev.DrawVB");
}

static HRESULT Dev_DrawIVB(IDirect3DDevice7 *t, DWORD a, LPVOID b, DWORD c,
			   DWORD d, LPVOID e, DWORD f, DWORD g)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	return dev_unimp(t, "D3DDev.DrawIVB");
}

static HRESULT Dev_Sphere(IDirect3DDevice7 *t, LPVOID a, LPVOID b, DWORD c,
			  DWORD d, DWORD *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return dev_unimp(t, "D3DDev.SphereVis");
}

static HRESULT D3D7_CreateDevice(IDirect3D7 *this, REFCLSID clsid,
				 LPDIRECTDRAWSURFACE7 surf,
				 LPDIRECT3DDEVICE7 *out)
{
	IDirect3DDevice7 *dev;

	(void)clsid;
	if (!out)
		return E_POINTER;
	if (!surf)
		return E_POINTER;
	dev = (IDirect3DDevice7 *)memalign(32, sizeof(*dev));
	if (!dev)
		return E_OUTOFMEMORY;
	memset(dev, 0, sizeof(*dev));
	dev->lpVtbl = &s_dev;
	dev->ref = 1;
	dev->d3d = this;
	dev->target = surf;
	this->ref++;
	*out = dev;
	return D3D_OK;
}

static const IDirect3D7Vtbl s_d3d7 = {
	D3D7_QI, D3D7_AddRef, D3D7_Release, D3D7_EnumDevices, D3D7_CreateDevice,
	D3D7_CreateVB, D3D7_EnumZ, D3D7_Evict
};

static const IDirect3DDevice7Vtbl s_dev = {
	Dev_QI, Dev_AddRef, Dev_Release, Dev_GetCaps, Dev_EnumTex, Dev_BeginScene,
	Dev_EndScene, Dev_GetDirect3D, Dev_SetRT, Dev_GetRT, Dev_Clear, Dev_Ok1,
	Dev_Ok1, Dev_Ok0, Dev_Ok1, Dev_Ok0, Dev_Ok0, Dev_Ok0, Dev_Ok1, Dev_Ok1,
	Dev_SetRS, Dev_GetRS, Dev_BeginSB, Dev_EndSB, Dev_PreLoad, Dev_DrawPrim,
	Dev_DrawIndexed, Dev_Ok0, Dev_Ok0, Dev_DrawStrided, Dev_DrawIStrided,
	Dev_DrawVB, Dev_DrawIVB, Dev_Sphere, Dev_GetTex, Dev_SetTex, Dev_GetTSS,
	Dev_SetTSS, Dev_Validate, Dev_SB, Dev_SB, Dev_SB, Dev_CreateSB, Dev_Load,
	Dev_LightEn, Dev_GetLightEn, Dev_SetClip, Dev_GetClip, Dev_GetInfo
};

HRESULT host_d3d7_query(LPDIRECTDRAW7 dd, REFIID iid, LPVOID *ppv)
{
	IDirect3D7 *d3d;

	(void)iid;
	if (!ppv)
		return E_POINTER;
	d3d = (IDirect3D7 *)memalign(32, sizeof(*d3d));
	if (!d3d)
		return E_OUTOFMEMORY;
	memset(d3d, 0, sizeof(*d3d));
	d3d->lpVtbl = &s_d3d7;
	d3d->ref = 1;
	d3d->dd = dd;
	*ppv = d3d;
	return S_OK;
}
