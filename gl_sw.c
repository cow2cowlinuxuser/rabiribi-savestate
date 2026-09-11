#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "savestate.h"
#include "swalloc.h"
#include "swrast.h"

#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

/* Software OpenGL drop-in so Haydee 1 can rewind the same way the D3D
 * devices do: every texture and target is process memory, SwapBuffers is
 * the Present seam. GLSL is stored, not executed; draws feed attrib 0
 * through the same clip-space path D3D11 already uses. Compute shaders
 * are accepted and ignored. PhysX is not our problem — this build of
 * Haydee already ships the CPU libraries. */

PROC gl_lookup_proc(const char *name);

#define GL_TEXUNITS 32
#define GL_ATTRS 16
#define GL_UBOS 16
#define GL_IMM_MAX 4096
#define GL_EXT_COUNT 14

static const char *k_exts[GL_EXT_COUNT] = {
	"GL_ARB_framebuffer_object",
	"GL_ARB_vertex_buffer_object",
	"GL_ARB_vertex_array_object",
	"GL_ARB_shader_objects",
	"GL_ARB_shading_language_100",
	"GL_ARB_map_buffer_range",
	"GL_ARB_uniform_buffer_object",
	"GL_ARB_sampler_objects",
	"GL_ARB_instanced_arrays",
	"GL_ARB_draw_instanced",
	"GL_ARB_texture_storage",
	"GL_ARB_sync",
	"GL_ARB_compute_shader",
	"GL_EXT_texture_compression_s3tc",
};

typedef struct GlTex {
	GLuint id;
	GLenum target;
	int w, h;
	uint32_t *pixels;
	int has;
} GlTex;

typedef struct GlBuf {
	GLuint id;
	unsigned char *data;
	size_t size;
	int mapped;
} GlBuf;

typedef struct GlAttrib {
	int enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	GLsizeiptr offset;
	GLuint buf;
	int normalized;
	GLuint divisor;
} GlAttrib;

typedef struct GlVao {
	GLuint id;
	GlAttrib attr[GL_ATTRS];
	GLuint ebo;
} GlVao;

typedef struct GlShader {
	GLuint id;
	GLenum type;
	char *src;
} GlShader;

typedef struct GlProg {
	GLuint id;
	int linked;
} GlProg;

typedef struct GlFbo {
	GLuint id;
	GLuint color;
	GLenum color_tgt;
	GLuint depth;
	GLenum depth_tgt;
} GlFbo;

typedef struct GlRbo {
	GLuint id;
	int w, h;
	uint32_t *color;
	float *depth;
} GlRbo;

typedef struct GlSamp {
	GLuint id;
	int mag, min_f;
} GlSamp;

typedef struct GlShare {
	LONG ref;
	GlTex **tex;
	int ntex, ctex;
	GlBuf **buf;
	int nbuf, cbuf;
	GlVao **vao;
	int nvao, cvao;
	GlShader **sh;
	int nsh, csh;
	GlProg **prog;
	int nprog, cprog;
	GlFbo **fbo;
	int nfbo, cfbo;
	GlRbo **rbo;
	int nrbo, crbo;
	GlSamp **samp;
	int nsamp, csamp;
	GLuint next_id;
	GlVao *vao0;
} GlShare;

typedef struct GlCtx {
	HDC hdc;
	HWND hwnd;
	GlShare *share;
	SwRast fb;
	GLuint draw_fbo, read_fbo;
	GLuint vao, prog;
	GLuint texunit;
	GLuint tex2d[GL_TEXUNITS];
	GLuint buf_array, buf_element, buf_uniform[GL_UBOS];
	GLuint sampler[GL_TEXUNITS];
	int vp[4], scissor[4];
	int en_blend, en_depth, en_cull, en_scissor, en_tex2d;
	GLenum blend_src, blend_dst, blend_op;
	GLenum depth_func, cull_face, front_face;
	GLboolean depth_mask, color_mask[4];
	float clear_c[4];
	double clear_z;
	int unpack_align;
	int in_begin;
	GLenum begin_mode;
	float cur_u, cur_v;
	uint32_t cur_color;
	SwVert imm[GL_IMM_MAX];
	int nimm;
	GLenum err;
} GlCtx;

/* Current context is per-thread. Haydee spins a pile of 1x1 loader
 * contexts on worker threads; a process-global current made every
 * SwapBuffers present a 1x1 hidden window while the 2560x1334 one sat
 * idle. */
static __thread GlCtx *g_cur;
static GlCtx *g_all[64];
static int g_nctx;
static CRITICAL_SECTION g_ctx_lock;
static LONG g_lock_ready;
static LONG g_present_n;
static LONG g_makecurrent_n;
static LONG g_frame_draws;
static LONG g_frame_tris;
static LONG g_frame_skip;
static int g_swap_interval;
static GlShare *g_share;

static void ctx_lock_enter(void)
{
	if (g_lock_ready < 2) {
		if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
			InitializeCriticalSection(&g_ctx_lock);
			InterlockedExchange(&g_lock_ready, 2);
		} else {
			while (g_lock_ready < 2)
				Sleep(0);
		}
	}
	EnterCriticalSection(&g_ctx_lock);
}

static int trace_on(void)
{
	static int v = -1;
	if (v < 0) {
		const char *s = getenv("GLSW_TRACE");
		v = (s && *s && *s != '0') ? 1 : 0;
	}
	return v;
}

static void gl_vlog(const char *fmt, va_list ap)
{
	static volatile LONG n;
	char line[640];
	FILE *f;
	if (InterlockedIncrement(&n) > 20000)
		return;
	_vsnprintf(line, sizeof(line), fmt, ap);
	line[sizeof(line) - 1] = 0;
	OutputDebugStringA("[gl_sw] ");
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
	f = fopen("gl_sw.log", "a");
	if (f) {
		fprintf(f, "%s\n", line);
		fflush(f);
		fclose(f);
	}
}

static void gl_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	gl_vlog(fmt, ap);
	va_end(ap);
}

static void gl_trace(const char *fmt, ...)
{
	va_list ap;
	if (!trace_on())
		return;
	va_start(ap, fmt);
	gl_vlog(fmt, ap);
	va_end(ap);
}

void gl_ni(const char *name)
{
	static char seen[160][64];
	static int n;
	int i;
	for (i = 0; i < n; i++)
		if (strcmp(seen[i], name) == 0)
			return;
	if (n < 160) {
		strncpy(seen[n], name, 63);
		seen[n][63] = 0;
		n++;
	}
	gl_log("NI %s", name);
}

static GlCtx *cur(void)
{
	return g_cur;
}

static void ctx_lock_leave(void)
{
	LeaveCriticalSection(&g_ctx_lock);
}

static void ctx_register(GlCtx *c)
{
	ctx_lock_enter();
	if (g_nctx < (int)(sizeof(g_all) / sizeof(g_all[0])))
		g_all[g_nctx++] = c;
	ctx_lock_leave();
}

static void ctx_unregister(GlCtx *c)
{
	int i;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++)
		if (g_all[i] == c) {
			g_all[i] = g_all[g_nctx - 1];
			g_nctx--;
			break;
		}
	ctx_lock_leave();
}

static GlCtx *ctx_for_hwnd(HWND hwnd)
{
	GlCtx *best = NULL;
	int i, area, best_area = 0;
	if (!hwnd)
		return NULL;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++) {
		GlCtx *c = g_all[i];
		if (!c || c->hwnd != hwnd)
			continue;
		area = c->fb.width * c->fb.height;
		if (!best || area > best_area) {
			best = c;
			best_area = area;
		}
	}
	ctx_lock_leave();
	return best;
}

static GlCtx *ctx_largest(void)
{
	GlCtx *best = NULL;
	int i, area, best_area = 0;
	ctx_lock_enter();
	for (i = 0; i < g_nctx; i++) {
		GlCtx *c = g_all[i];
		int cw, ch;
		RECT rc;
		if (!c)
			continue;
		cw = c->fb.width;
		ch = c->fb.height;
		if (c->hwnd && GetClientRect(c->hwnd, &rc)) {
			int ww = rc.right - rc.left;
			int wh = rc.bottom - rc.top;
			if (ww * wh > cw * ch) {
				cw = ww;
				ch = wh;
			}
		}
		area = cw * ch;
		if (area > best_area) {
			best = c;
			best_area = area;
		}
	}
	ctx_lock_leave();
	return best;
}

static int hwnd_area(HWND hwnd)
{
	RECT rc;
	if (!hwnd || !GetClientRect(hwnd, &rc))
		return 0;
	return (rc.right - rc.left) * (rc.bottom - rc.top);
}

static void set_err(GLenum e)
{
	GlCtx *c = cur();
	if (c && !c->err)
		c->err = e;
}

static GLuint alloc_id(GlShare *s)
{
	if (!s->next_id)
		s->next_id = 1;
	return s->next_id++;
}

static void *grow(void **arr, int *n, int *cap, size_t elem)
{
	if (*n >= *cap) {
		int nc = *cap ? *cap * 2 : 32;
		void *p = realloc(*arr, (size_t)nc * elem);
		if (!p)
			return NULL;
		memset((char *)p + (size_t)*cap * elem, 0, (size_t)(nc - *cap) * elem);
		*arr = p;
		*cap = nc;
	}
	return (char *)*arr + (size_t)(*n) * elem;
}

static GlShare *share_new(void)
{
	GlShare *s = (GlShare *)calloc(1, sizeof(*s));
	if (s) {
		s->ref = 1;
		s->vao0 = (GlVao *)calloc(1, sizeof(GlVao));
	}
	return s;
}

static GlShare *share_global(void)
{
	if (!g_share)
		g_share = share_new();
	if (g_share)
		InterlockedIncrement(&g_share->ref);
	return g_share;
}

static void tex_free(GlTex *t)
{
	if (!t)
		return;
	free(t->pixels);
	free(t);
}

static void buf_free(GlBuf *b)
{
	if (!b)
		return;
	free(b->data);
	free(b);
}

static void share_release(GlShare *s)
{
	int i;
	if (!s || InterlockedDecrement(&s->ref) > 0)
		return;
	for (i = 0; i < s->ntex; i++)
		tex_free(s->tex[i]);
	for (i = 0; i < s->nbuf; i++)
		buf_free(s->buf[i]);
	for (i = 0; i < s->nvao; i++)
		free(s->vao[i]);
	for (i = 0; i < s->nsh; i++) {
		if (s->sh[i])
			free(s->sh[i]->src);
		free(s->sh[i]);
	}
	for (i = 0; i < s->nprog; i++)
		free(s->prog[i]);
	for (i = 0; i < s->nfbo; i++)
		free(s->fbo[i]);
	for (i = 0; i < s->nrbo; i++) {
		if (s->rbo[i]) {
			free(s->rbo[i]->color);
			free(s->rbo[i]->depth);
		}
		free(s->rbo[i]);
	}
	for (i = 0; i < s->nsamp; i++)
		free(s->samp[i]);
	free(s->tex);
	free(s->buf);
	free(s->vao);
	free(s->sh);
	free(s->prog);
	free(s->fbo);
	free(s->rbo);
	free(s->samp);
	free(s->vao0);
	if (g_share == s)
		g_share = NULL;
	free(s);
}

static GlTex *tex_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->ntex; i++)
		if (s->tex[i] && s->tex[i]->id == id)
			return s->tex[i];
	return NULL;
}

static GlBuf *buf_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nbuf; i++)
		if (s->buf[i] && s->buf[i]->id == id)
			return s->buf[i];
	return NULL;
}

static GlVao *vao_get(GlShare *s, GLuint id)
{
	int i;
	if (!s)
		return NULL;
	if (id == 0)
		return s->vao0;
	for (i = 0; i < s->nvao; i++)
		if (s->vao[i] && s->vao[i]->id == id)
			return s->vao[i];
	return NULL;
}

static GlFbo *fbo_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nfbo; i++)
		if (s->fbo[i] && s->fbo[i]->id == id)
			return s->fbo[i];
	return NULL;
}

static GlRbo *rbo_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nrbo; i++)
		if (s->rbo[i] && s->rbo[i]->id == id)
			return s->rbo[i];
	return NULL;
}

static GlShader *sh_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nsh; i++)
		if (s->sh[i] && s->sh[i]->id == id)
			return s->sh[i];
	return NULL;
}

static GlProg *prog_get(GlShare *s, GLuint id)
{
	int i;
	if (!s || !id)
		return NULL;
	for (i = 0; i < s->nprog; i++)
		if (s->prog[i] && s->prog[i]->id == id)
			return s->prog[i];
	return NULL;
}

static int ensure_fb(GlCtx *c, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (c->fb.color && c->fb.width == w && c->fb.height == h)
		return 1;
	swrast_resize(&c->fb, w, h);
	c->fb.hwnd = c->hwnd;
	return c->fb.color != NULL;
}

static GlCtx *display_ctx(GlCtx *c)
{
	if (c && c->fb.width * c->fb.height > 16)
		return c;
	{
		GlCtx *big = ctx_largest();
		if (big)
			return big;
	}
	return c;
}

static void size_from_dc(GlCtx *c, int *w, int *h)
{
	RECT rc;
	HWND hwnd = c->hwnd ? c->hwnd : WindowFromDC(c->hdc);
	c->hwnd = hwnd;
	if (hwnd && GetClientRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
		*w = rc.right - rc.left;
		*h = rc.bottom - rc.top;
		return;
	}
	*w = 1280;
	*h = 720;
}

static void bind_draw_target(GlCtx *c, SwRast *out, SwTex *tex)
{
	GlFbo *f;
	GlTex *t;
	memset(out, 0, sizeof(*out));
	memset(tex, 0, sizeof(*tex));
	if (!c->draw_fbo) {
		GlCtx *d = display_ctx(c);
		int w, h;
		size_from_dc(d, &w, &h);
		ensure_fb(d, w, h);
		*out = d->fb;
		return;
	}
	f = fbo_get(c->share, c->draw_fbo);
	if (!f)
		return;
	t = tex_get(c->share, f->color);
	if (t && t->pixels) {
		out->color = t->pixels;
		out->width = t->w;
		out->height = t->h;
		tex->pixels = t->pixels;
		tex->width = t->w;
		tex->height = t->h;
	}
}

static void bind_read_target(GlCtx *c, SwRast *out)
{
	SwTex tex;
	GLuint save = c->draw_fbo;
	c->draw_fbo = c->read_fbo;
	bind_draw_target(c, out, &tex);
	c->draw_fbo = save;
}

static uint32_t pack_argb(float r, float g, float b, float a)
{
	int ir, ig, ib, ia;
	if (r < 0)
		r = 0;
	if (r > 1)
		r = 1;
	if (g < 0)
		g = 0;
	if (g > 1)
		g = 1;
	if (b < 0)
		b = 0;
	if (b > 1)
		b = 1;
	if (a < 0)
		a = 0;
	if (a > 1)
		a = 1;
	ir = (int)(r * 255.0f + 0.5f);
	ig = (int)(g * 255.0f + 0.5f);
	ib = (int)(b * 255.0f + 0.5f);
	ia = (int)(a * 255.0f + 0.5f);
	return ((UINT)ia << 24) | ((UINT)ir << 16) | ((UINT)ig << 8) | (UINT)ib;
}

static int d3d_blend(GLenum f)
{
	switch (f) {
	case GL_ZERO:
		return 1; /* D3DBLEND_ZERO */
	case GL_ONE:
		return 2;
	case GL_SRC_COLOR:
		return 3;
	case GL_ONE_MINUS_SRC_COLOR:
		return 4;
	case GL_SRC_ALPHA:
		return 5;
	case GL_ONE_MINUS_SRC_ALPHA:
		return 6;
	case GL_DST_ALPHA:
		return 7;
	case GL_ONE_MINUS_DST_ALPHA:
		return 8;
	case GL_DST_COLOR:
		return 9;
	case GL_ONE_MINUS_DST_COLOR:
		return 10;
	default:
		return 5;
	}
}

static int d3d_zfunc(GLenum f)
{
	switch (f) {
	case GL_NEVER:
		return 1;
	case GL_LESS:
		return 2;
	case GL_EQUAL:
		return 3;
	case GL_LEQUAL:
		return 4;
	case GL_GREATER:
		return 5;
	case GL_NOTEQUAL:
		return 6;
	case GL_GEQUAL:
		return 7;
	case GL_ALWAYS:
		return 8;
	default:
		return 4;
	}
}

static void fill_state(GlCtx *c, SwState *st, int rt_h)
{
	swrast_state_defaults(st);
	st->blend_enable = c->en_blend;
	st->src_blend = d3d_blend(c->blend_src);
	st->dst_blend = d3d_blend(c->blend_dst);
	st->blend_op = 1; /* ADD */
	st->z_enable = c->en_depth;
	st->z_write = c->depth_mask;
	st->z_func = d3d_zfunc(c->depth_func);
	st->cull = c->en_cull ? 3 : 1; /* D3DCULL_CCW / NONE — GL default CCW */
	st->scissor_enable = c->en_scissor;
	if (rt_h < 1)
		rt_h = c->fb.height;
	st->scissor_x0 = c->scissor[0];
	st->scissor_y0 = rt_h - (c->scissor[1] + c->scissor[3]);
	st->scissor_x1 = c->scissor[0] + c->scissor[2];
	st->scissor_y1 = rt_h - c->scissor[1];
	st->bilinear = 1;
	st->addr_u = 1; /* WRAP */
	st->addr_v = 1;
	st->write_mask = 0xffffffffu;
	if (!c->color_mask[0])
		st->write_mask &= ~0x00ff0000u;
	if (!c->color_mask[1])
		st->write_mask &= ~0x0000ff00u;
	if (!c->color_mask[2])
		st->write_mask &= ~0x000000ffu;
	if (!c->color_mask[3])
		st->write_mask &= ~0xff000000u;
}

static unsigned type_bytes(GLenum type)
{
	switch (type) {
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		return 1;
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
		return 2;
	case GL_FLOAT:
	case GL_UNSIGNED_INT:
	case GL_INT:
		return 4;
	default:
		return 4;
	}
}

static void load_attr(float out[4], const unsigned char *p, const GlAttrib *a)
{
	int i;
	out[0] = out[1] = out[2] = 0;
	out[3] = 1;
	if (!p)
		return;
	for (i = 0; i < a->size && i < 4; i++) {
		const unsigned char *c = p + (size_t)i * type_bytes(a->type);
		switch (a->type) {
		case GL_FLOAT:
			out[i] = *(const float *)c;
			break;
		case GL_UNSIGNED_BYTE:
			out[i] = a->normalized ? c[0] / 255.0f : (float)c[0];
			break;
		case GL_UNSIGNED_SHORT:
			out[i] = a->normalized ? (*(const GLushort *)c) / 65535.0f
					       : (float)(*(const GLushort *)c);
			break;
		case GL_UNSIGNED_INT:
			out[i] = (float)(*(const GLuint *)c);
			break;
		case GL_INT:
			out[i] = (float)(*(const GLint *)c);
			break;
		default:
			out[i] = *(const float *)c;
			break;
		}
	}
}

static const unsigned char *attr_ptr(GlCtx *c, const GlAttrib *a, unsigned idx)
{
	GlBuf *b;
	size_t stride, off;
	if (!a->enabled)
		return NULL;
	stride = a->stride ? (size_t)a->stride : (size_t)a->size * type_bytes(a->type);
	off = (size_t)a->offset + (size_t)idx * stride;
	if (a->buf) {
		b = buf_get(c->share, a->buf);
		if (!b || !b->data || off + type_bytes(a->type) > b->size)
			return NULL;
		return b->data + off;
	}
	return (const unsigned char *)(uintptr_t)a->offset + (size_t)idx * stride;
}

static void vert_from_attr(GlCtx *c, SwVert *v, unsigned idx, int vp_w, int vp_h, int vp_x,
			   int vp_y)
{
	GlVao *vao = vao_get(c->share, c->vao);
	const GlAttrib *pos = NULL, *uv = NULL, *col = NULL;
	float p[4], t[4], k[4];
	float x, y, z, w, rhw;
	int i;

	memset(v, 0, sizeof(*v));
	v->color = 0xffffffffu;
	v->rhw = 1;
	if (vao) {
		for (i = 0; i < GL_ATTRS; i++) {
			if (!vao->attr[i].enabled)
				continue;
			if (!pos)
				pos = &vao->attr[i];
			else if (!uv && vao->attr[i].size <= 2)
				uv = &vao->attr[i];
			else if (!col && vao->attr[i].size >= 3)
				col = &vao->attr[i];
		}
	}
	if (pos)
		load_attr(p, attr_ptr(c, pos, idx), pos);
	else {
		p[0] = p[1] = p[2] = 0;
		p[3] = 1;
	}
	w = p[3] != 0.0f ? p[3] : 1.0f;
	rhw = 1.0f / w;
	x = p[0] * rhw;
	y = p[1] * rhw;
	z = p[2] * rhw;
	v->x = (x * 0.5f + 0.5f) * (float)vp_w + (float)vp_x;
	v->y = (0.5f - y * 0.5f) * (float)vp_h + (float)vp_y;
	v->z = z * 0.5f + 0.5f;
	v->rhw = rhw;
	if (!pos || (x > 2.0f || x < -2.0f || y > 2.0f || y < -2.0f)) {
		/* Same escape hatch as D3D11: raw pixel-space verts skip clip. */
		if (p[0] > 2.0f || p[0] < -2.0f || p[1] > 2.0f || p[1] < -2.0f) {
			v->x = p[0] + (float)vp_x;
			v->y = (float)vp_h - p[1] + (float)vp_y;
			v->z = 0.5f;
			v->rhw = 1.0f;
		}
	}
	if (uv) {
		load_attr(t, attr_ptr(c, uv, idx), uv);
		v->u = t[0];
		v->v = t[1];
	}
	if (col) {
		load_attr(k, attr_ptr(c, col, idx), col);
		v->color = pack_argb(k[0], k[1], k[2], k[3]);
	}
}

static GlTex *bound_tex2d(GlCtx *c)
{
	return tex_get(c->share, c->tex2d[c->texunit]);
}

static void draw_tris(GlCtx *c, GLenum mode, unsigned count, GLenum itype, const void *idxp,
		      unsigned first, int basevertex)
{
	SwRast rt;
	SwTex tex;
	SwState st;
	SwTri *batch;
	GlTex *gt;
	GlVao *vao;
	GlBuf *ebo;
	unsigned i, ntri, maxv;
	int vp_w, vp_h, vp_x, vp_y;
	const unsigned char *ip = NULL;
	unsigned esz = 0;

	if (!c || !count)
		return;
	if (mode == GL_POLYGON)
		mode = GL_TRIANGLE_FAN;
	bind_draw_target(c, &rt, &tex);
	if (!rt.color || rt.width <= 0)
		return;
	g_frame_draws++;
	vp_x = c->vp[0];
	vp_y = rt.height - (c->vp[1] + c->vp[3]);
	vp_w = c->vp[2] ? c->vp[2] : rt.width;
	vp_h = c->vp[3] ? c->vp[3] : rt.height;
	if (vp_h < 0) {
		vp_y = c->vp[1];
		vp_h = -vp_h;
	}
	if (vp_w < 2 || vp_h < 2) {
		vp_x = 0;
		vp_y = 0;
		vp_w = rt.width;
		vp_h = rt.height;
	}
	fill_state(c, &st, rt.height);
	gt = bound_tex2d(c);
	if (gt && gt->pixels) {
		tex.pixels = gt->pixels;
		tex.width = gt->w;
		tex.height = gt->h;
	} else if (!tex.pixels)
		memset(&tex, 0, sizeof(tex));

	vao = vao_get(c->share, c->vao);
	if (vao && vao->ebo) {
		ebo = buf_get(c->share, vao->ebo);
		ip = ebo && ebo->data ? ebo->data + (size_t)(uintptr_t)idxp : NULL;
		esz = (itype == GL_UNSIGNED_INT) ? 4u : (itype == GL_UNSIGNED_BYTE) ? 1u : 2u;
	} else if (idxp) {
		ip = (const unsigned char *)idxp;
		esz = (itype == GL_UNSIGNED_INT) ? 4u : (itype == GL_UNSIGNED_BYTE) ? 1u : 2u;
	}

	if (mode == GL_POINTS)
		ntri = count * 2;
	else if (mode == GL_TRIANGLES || mode == GL_PATCHES)
		ntri = count / 3;
	else if (mode == GL_TRIANGLE_STRIP)
		ntri = count >= 3 ? count - 2 : 0;
	else if (mode == GL_TRIANGLE_FAN)
		ntri = count >= 3 ? count - 2 : 0;
	else if (mode == GL_QUADS)
		ntri = (count / 4) * 2;
	else if (mode == GL_TRIANGLES_ADJACENCY)
		ntri = count / 6;
	else if (mode == GL_TRIANGLE_STRIP_ADJACENCY)
		ntri = count >= 6 ? (count / 2) - 2 : 0;
	else if (mode == GL_LINES)
		ntri = (count / 2) * 2;
	else if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP)
		ntri = count >= 2 ? (count - (mode == GL_LINE_LOOP ? 0 : 1)) * 2 : 0;
	else {
		char tag[40];
		_snprintf(tag, sizeof(tag), "draw mode 0x%x", (unsigned)mode);
		gl_ni(tag);
		g_frame_skip++;
		return;
	}
	if (!ntri)
		return;
	batch = (SwTri *)malloc((size_t)ntri * sizeof(SwTri));
	if (!batch)
		return;
	maxv = count + first;
	for (i = 0; i < ntri; i++) {
		unsigned k;
		unsigned vid[3];
		SwVert sv[3];
		if (mode == GL_TRIANGLES || mode == GL_PATCHES) {
			vid[0] = i * 3;
			vid[1] = i * 3 + 1;
			vid[2] = i * 3 + 2;
		} else if (mode == GL_TRIANGLES_ADJACENCY) {
			vid[0] = i * 6;
			vid[1] = i * 6 + 2;
			vid[2] = i * 6 + 4;
		} else if (mode == GL_TRIANGLE_STRIP_ADJACENCY) {
			vid[0] = 2 * i;
			vid[1] = 2 * i + 2;
			vid[2] = 2 * i + 4;
			if (i & 1) {
				unsigned t = vid[1];
				vid[1] = vid[2];
				vid[2] = t;
			}
		} else if (mode == GL_QUADS) {
			unsigned q = (i / 2) * 4;
			if ((i & 1) == 0) {
				vid[0] = q;
				vid[1] = q + 1;
				vid[2] = q + 2;
			} else {
				vid[0] = q;
				vid[1] = q + 2;
				vid[2] = q + 3;
			}
		} else if (mode == GL_TRIANGLE_STRIP) {
			vid[0] = i;
			vid[1] = i + 1;
			vid[2] = i + 2;
			if (i & 1) {
				unsigned t = vid[1];
				vid[1] = vid[2];
				vid[2] = t;
			}
		} else if (mode == GL_POINTS) {
			vid[0] = vid[1] = vid[2] = i / 2;
		} else if (mode == GL_LINES) {
			unsigned seg = i / 2;
			vid[0] = seg * 2;
			vid[1] = seg * 2 + 1;
			vid[2] = vid[1];
		} else if (mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
			unsigned seg = i / 2;
			vid[0] = seg;
			vid[1] = seg + 1;
			if (mode == GL_LINE_LOOP && vid[1] >= count)
				vid[1] = 0;
			vid[2] = vid[1];
		} else {
			vid[0] = 0;
			vid[1] = i + 1;
			vid[2] = i + 2;
		}
		for (k = 0; k < 3; k++) {
			unsigned geom = first + vid[k];
			if (ip && esz) {
				unsigned idx = first + vid[k];
				if (esz == 4)
					geom = ((const GLuint *)ip)[idx];
				else if (esz == 2)
					geom = ((const GLushort *)ip)[idx];
				else
					geom = ip[idx];
			}
			geom += (unsigned)basevertex;
			vert_from_attr(c, &sv[k], geom, vp_w, vp_h, vp_x, vp_y);
			(void)maxv;
		}
		if (mode == GL_POINTS) {
			static const float ox[2][3] = { { -1, 1, 1 }, { -1, 1, -1 } };
			static const float oy[2][3] = { { -1, -1, 1 }, { -1, 1, 1 } };
			int t = (int)(i & 1);
			for (k = 0; k < 3; k++) {
				sv[k].x += ox[t][k];
				sv[k].y += oy[t][k];
			}
		} else if (mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) {
			float dx = sv[1].x - sv[0].x, dy = sv[1].y - sv[0].y;
			float len = (float)sqrt(dx * dx + dy * dy);
			float nx, ny;
			if (len < 1.0f)
				len = 1.0f;
			nx = -dy / len;
			ny = dx / len;
			if ((i & 1) == 0) {
				sv[2] = sv[1];
				sv[0].x += nx;
				sv[0].y += ny;
				sv[1].x += nx;
				sv[1].y += ny;
				sv[2].x -= nx;
				sv[2].y -= ny;
			} else {
				SwVert v0 = sv[0], v1 = sv[1];
				sv[0] = v0;
				sv[0].x += nx;
				sv[0].y += ny;
				sv[1] = v1;
				sv[1].x -= nx;
				sv[1].y -= ny;
				sv[2] = v0;
				sv[2].x -= nx;
				sv[2].y -= ny;
			}
		}
		batch[i].a = sv[0];
		batch[i].b = sv[1];
		batch[i].c = sv[2];
	}
	g_frame_tris += (LONG)ntri;
	gl_trace("draw mode=%u ntri=%u rt=%dx%d fbo=%u tex=%ux%u", mode, ntri, rt.width, rt.height,
		 c->draw_fbo, tex.width, tex.height);
	swrast_triangles(&rt, batch, (int)ntri, tex.pixels ? &tex : NULL, &st);
	free(batch);
}

/* ---- WGL pixel format: GDI forwards Choose/Set/Swap here. ---- */

BOOL WINAPI wglSwapBuffers(HDC hdc);

static PIXELFORMATDESCRIPTOR g_pfd;
static int g_pfd_set;

int WINAPI wglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	if (pfd)
		g_pfd = *pfd;
	g_pfd.nSize = sizeof(g_pfd);
	g_pfd.nVersion = 1;
	g_pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	g_pfd.iPixelType = PFD_TYPE_RGBA;
	g_pfd.cColorBits = 32;
	g_pfd.cDepthBits = 24;
	g_pfd.cStencilBits = 8;
	g_pfd.iLayerType = PFD_MAIN_PLANE;
	gl_log("wglChoosePixelFormat -> 1");
	return 1;
}

BOOL WINAPI wglSetPixelFormat(HDC hdc, int fmt, const PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	(void)fmt;
	if (pfd)
		g_pfd = *pfd;
	g_pfd_set = 1;
	gl_log("wglSetPixelFormat %d", fmt);
	return TRUE;
}

int WINAPI wglGetPixelFormat(HDC hdc)
{
	(void)hdc;
	return g_pfd_set ? 1 : 0;
}

int WINAPI wglDescribePixelFormat(HDC hdc, int fmt, UINT size, PIXELFORMATDESCRIPTOR *pfd)
{
	(void)hdc;
	(void)fmt;
	if (!pfd || size < sizeof(*pfd))
		return 1;
	memset(pfd, 0, sizeof(*pfd));
	pfd->nSize = sizeof(*pfd);
	pfd->nVersion = 1;
	pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd->iPixelType = PFD_TYPE_RGBA;
	pfd->cColorBits = 32;
	pfd->cRedBits = pfd->cGreenBits = pfd->cBlueBits = pfd->cAlphaBits = 8;
	pfd->cDepthBits = 24;
	pfd->cStencilBits = 8;
	pfd->iLayerType = PFD_MAIN_PLANE;
	return 1;
}

static GlCtx *ctx_from_rc(HGLRC rc)
{
	return (GlCtx *)rc;
}

HGLRC WINAPI wglCreateContext(HDC hdc)
{
	GlCtx *c;
	int w, h;
	savestate_hooks_install();
	c = (GlCtx *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->hdc = hdc;
	c->hwnd = WindowFromDC(hdc);
	c->share = share_global();
	c->blend_src = GL_SRC_ALPHA;
	c->blend_dst = GL_ONE_MINUS_SRC_ALPHA;
	c->blend_op = GL_FUNC_ADD;
	c->depth_func = GL_LESS;
	c->cull_face = GL_BACK;
	c->front_face = GL_CCW;
	c->depth_mask = GL_TRUE;
	c->color_mask[0] = c->color_mask[1] = c->color_mask[2] = c->color_mask[3] = GL_TRUE;
	c->clear_z = 1.0;
	c->unpack_align = 4;
	c->cur_color = 0xffffffffu;
	size_from_dc(c, &w, &h);
	c->vp[0] = 0;
	c->vp[1] = 0;
	c->vp[2] = w;
	c->vp[3] = h;
	c->scissor[0] = 0;
	c->scissor[1] = 0;
	c->scissor[2] = w;
	c->scissor[3] = h;
	swrast_init(&c->fb, c->hwnd, w, h);
	ctx_register(c);
	gl_log("wglCreateContext hdc=%p hwnd=%p %dx%d", hdc, c->hwnd, w, h);
	return (HGLRC)c;
}

HGLRC WINAPI wglCreateContextAttribsARB(HDC hdc, HGLRC share, const int *attr)
{
	HGLRC rc = wglCreateContext(hdc);
	(void)attr;
	(void)share;
	gl_log("wglCreateContextAttribsARB share=%p", share);
	return rc;
}

BOOL WINAPI wglDeleteContext(HGLRC rc)
{
	GlCtx *c = ctx_from_rc(rc);
	if (!c)
		return FALSE;
	if (g_cur == c)
		g_cur = NULL;
	swrast_free(&c->fb);
	share_release(c->share);
	ctx_unregister(c);
	free(c);
	return TRUE;
}

BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRC rc)
{
	GlCtx *c = ctx_from_rc(rc);
	LONG n;
	int w = 0, h = 0;
	if (!rc) {
		g_cur = NULL;
		return TRUE;
	}
	if (!c)
		return FALSE;
	c->hdc = hdc;
	c->hwnd = WindowFromDC(hdc);
	size_from_dc(c, &w, &h);
	ensure_fb(c, w, h);
	g_cur = c;
	n = InterlockedIncrement(&g_makecurrent_n);
	if (n <= 80)
		gl_log("wglMakeCurrent #%ld hdc=%p hwnd=%p %dx%d", n, hdc, c->hwnd, w, h);
	return TRUE;
}

BOOL WINAPI wglShareLists(HGLRC a, HGLRC b)
{
	(void)a;
	(void)b;
	gl_log("wglShareLists");
	return TRUE;
}

HDC WINAPI wglGetCurrentDC(void)
{
	return g_cur ? g_cur->hdc : NULL;
}

HGLRC WINAPI wglGetCurrentContext(void)
{
	return (HGLRC)g_cur;
}

PROC WINAPI wglGetProcAddress(LPCSTR name)
{
	PROC p;
	if (!name)
		return NULL;
	p = gl_lookup_proc(name);
	if (!p)
		gl_log("wglGetProcAddress miss %s", name);
	else
		gl_trace("wglGetProcAddress %s", name);
	return p;
}

PROC WINAPI wglGetDefaultProcAddress(LPCSTR name)
{
	return wglGetProcAddress(name);
}

BOOL WINAPI wglCopyContext(HGLRC a, HGLRC b, UINT mask)
{
	(void)a;
	(void)b;
	(void)mask;
	gl_ni("wglCopyContext");
	return FALSE;
}

HGLRC WINAPI wglCreateLayerContext(HDC hdc, int layer)
{
	(void)layer;
	return wglCreateContext(hdc);
}

BOOL WINAPI wglDescribeLayerPlane(HDC hdc, int fmt, int layer, UINT size, LPLAYERPLANEDESCRIPTOR pd)
{
	(void)hdc;
	(void)fmt;
	(void)layer;
	(void)size;
	if (pd)
		memset(pd, 0, sizeof(*pd));
	return FALSE;
}

int WINAPI wglGetLayerPaletteEntries(HDC a, int b, int c, int d, COLORREF *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return 0;
}

int WINAPI wglSetLayerPaletteEntries(HDC a, int b, int c, int d, const COLORREF *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return 0;
}

BOOL WINAPI wglRealizeLayerPalette(HDC a, int b, BOOL c)
{
	(void)a;
	(void)b;
	(void)c;
	return FALSE;
}

BOOL WINAPI wglSwapLayerBuffers(HDC hdc, UINT planes)
{
	(void)planes;
	return wglSwapBuffers(hdc);
}

DWORD WINAPI wglSwapMultipleBuffers(UINT n, const WGLSWAP *bufs)
{
	UINT i;
	for (i = 0; i < n && bufs; i++)
		wglSwapBuffers(bufs[i].hdc);
	return n;
}

BOOL WINAPI wglUseFontBitmapsA(HDC a, DWORD b, DWORD c, DWORD d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	gl_ni("wglUseFontBitmapsA");
	return FALSE;
}

BOOL WINAPI wglUseFontBitmapsW(HDC a, DWORD b, DWORD c, DWORD d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	return FALSE;
}

BOOL WINAPI wglUseFontOutlinesA(HDC a, DWORD b, DWORD c, DWORD d, FLOAT e, FLOAT f, int g,
				LPGLYPHMETRICSFLOAT h)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	(void)h;
	return FALSE;
}

BOOL WINAPI wglUseFontOutlinesW(HDC a, DWORD b, DWORD c, DWORD d, FLOAT e, FLOAT f, int g,
				LPGLYPHMETRICSFLOAT h)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	(void)h;
	return FALSE;
}

static int wgl_pfd_attrib(int attr)
{
	switch (attr) {
	case WGL_NUMBER_PIXEL_FORMATS_ARB:
		return 1;
	case WGL_DRAW_TO_WINDOW_ARB:
	case WGL_SUPPORT_OPENGL_ARB:
	case WGL_DOUBLE_BUFFER_ARB:
		return 1;
	case WGL_DRAW_TO_BITMAP_ARB:
	case WGL_NEED_PALETTE_ARB:
	case WGL_NEED_SYSTEM_PALETTE_ARB:
	case WGL_SWAP_LAYER_BUFFERS_ARB:
	case WGL_STEREO_ARB:
	case WGL_SUPPORT_GDI_ARB:
	case WGL_TRANSPARENT_ARB:
		return 0;
	case WGL_ACCELERATION_ARB:
		return WGL_FULL_ACCELERATION_ARB;
	case WGL_SWAP_METHOD_ARB:
		return WGL_SWAP_COPY_ARB;
	case WGL_PIXEL_TYPE_ARB:
		return WGL_TYPE_RGBA_ARB;
	case WGL_COLOR_BITS_ARB:
		return 32;
	case WGL_RED_BITS_ARB:
	case WGL_GREEN_BITS_ARB:
	case WGL_BLUE_BITS_ARB:
	case WGL_ALPHA_BITS_ARB:
		return 8;
	case WGL_RED_SHIFT_ARB:
		return 16;
	case WGL_GREEN_SHIFT_ARB:
		return 8;
	case WGL_BLUE_SHIFT_ARB:
		return 0;
	case WGL_ALPHA_SHIFT_ARB:
		return 24;
	case WGL_DEPTH_BITS_ARB:
		return 24;
	case WGL_STENCIL_BITS_ARB:
		return 8;
	case WGL_NUMBER_OVERLAYS_ARB:
	case WGL_NUMBER_UNDERLAYS_ARB:
	case WGL_AUX_BUFFERS_ARB:
	case WGL_ACCUM_BITS_ARB:
		return 0;
	default:
		return 0;
	}
}

BOOL WINAPI wglGetPixelFormatAttribivARB(HDC hdc, int fmt, int layer, UINT n, const int *attr,
					 int *vals)
{
	UINT i;
	(void)hdc;
	(void)layer;
	if (!attr || !vals)
		return FALSE;
	if (fmt == 0) {
		for (i = 0; i < n; i++)
			vals[i] = (attr[i] == WGL_NUMBER_PIXEL_FORMATS_ARB) ? 1 : 0;
		return TRUE;
	}
	for (i = 0; i < n; i++)
		vals[i] = wgl_pfd_attrib(attr[i]);
	return TRUE;
}

BOOL WINAPI wglGetPixelFormatAttribfvARB(HDC hdc, int fmt, int layer, UINT n, const int *attr,
					 FLOAT *vals)
{
	UINT i;
	if (!vals)
		return FALSE;
	for (i = 0; i < n; i++) {
		int v;
		if (!wglGetPixelFormatAttribivARB(hdc, fmt, layer, 1, attr + i, &v))
			return FALSE;
		vals[i] = (FLOAT)v;
	}
	return TRUE;
}

BOOL WINAPI wglChoosePixelFormatARB(HDC hdc, const int *ia, const FLOAT *fa, UINT nmax, int *fmts,
				    UINT *nout)
{
	(void)ia;
	(void)fa;
	if (fmts && nmax)
		fmts[0] = 1;
	if (nout)
		*nout = 1;
	return wglChoosePixelFormat(hdc, NULL) != 0;
}

const char *WINAPI wglGetExtensionsStringARB(HDC hdc)
{
	(void)hdc;
	return "WGL_ARB_create_context WGL_ARB_create_context_profile "
	       "WGL_ARB_pixel_format WGL_EXT_swap_control WGL_ARB_extensions_string";
}

const char *WINAPI wglGetExtensionsStringEXT(void)
{
	return wglGetExtensionsStringARB(NULL);
}

BOOL WINAPI wglSwapIntervalEXT(int interval)
{
	g_swap_interval = interval;
	return TRUE;
}

int WINAPI wglGetSwapIntervalEXT(void)
{
	return g_swap_interval;
}

BOOL WINAPI wglSwapBuffers(HDC hdc)
{
	GlCtx *c = cur();
	HWND hwnd = WindowFromDC(hdc);
	GlCtx *matched;
	SwRast r;
	int k, w, h;
	LONG n;

	if (hwnd) {
		matched = ctx_for_hwnd(hwnd);
		if (matched)
			c = matched;
	}
	if (!c || hwnd_area(hwnd) <= 4) {
		GlCtx *big = ctx_largest();
		if (big && (!c || big->fb.width * big->fb.height > c->fb.width * c->fb.height)) {
			c = big;
			if (hwnd_area(hwnd) <= 4)
				hwnd = big->hwnd;
		}
	}
	if (!c)
		return FALSE;
	swrast_flush();
	size_from_dc(c, &w, &h);
	ensure_fb(c, w, h);
	savestate_guard();
	for (k = 0; k < SAVESTATE_SLOTS; k++) {
		if (!(GetAsyncKeyState(VK_F5 + k) & 1))
			continue;
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
			if (savestate_load(k))
				gl_log("savestate restored slot %d in %.1f ms", k,
				       savestate_last_ms());
		} else if (savestate_save(k)) {
			/* See the same branch in d3d11_sw.c: a restored thread returns
			 * through the save it was taking, so this is where a working
			 * restore reports in. */
			if (savestate_last_was_restore())
				gl_log("savestate restored slot %d in %.1f ms, resumed "
				       "through the save it was taking",
				       k, savestate_last_ms());
			else
				gl_log("savestate saved slot %d, %.1f MB in %.1f ms", k,
				       savestate_last_mb(), savestate_last_ms());
		}
	}
	memset(&r, 0, sizeof(r));
	r.color = c->fb.color;
	r.width = c->fb.width;
	r.height = c->fb.height;
	r.hwnd = hwnd ? hwnd : c->hwnd;
	swrast_present(&r, r.hwnd);
	n = InterlockedIncrement(&g_present_n);
	if (n <= 5 || (n % 30) == 1)
		gl_log("SwapBuffers #%ld %dx%d hwnd=%p fbo=%u cur=%p draws=%ld tris=%ld skip=%ld vp=%dx%d",
		       n, r.width, r.height, r.hwnd, c->draw_fbo, cur(), g_frame_draws, g_frame_tris,
		       g_frame_skip, cur() ? cur()->vp[2] : 0, cur() ? cur()->vp[3] : 0);
	g_frame_draws = g_frame_tris = g_frame_skip = 0;
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)inst;
	if (reason == DLL_PROCESS_ATTACH) {
		ctx_lock_enter();
		ctx_lock_leave();
		gl_log("process attach (software OpenGL)");
	}
	else if (reason == DLL_PROCESS_DETACH)
		gl_log("process detach (%s) after %ld swaps",
		       reserved ? "process exiting" : "FreeLibrary", g_present_n);
	return TRUE;
}

/* ---- queries ---- */

const GLubyte *APIENTRY glGetString(GLenum name)
{
	switch (name) {
	case GL_VENDOR:
		return (const GLubyte *)"swrast";
	case GL_RENDERER:
		return (const GLubyte *)"d3d9_sw OpenGL";
	case GL_VERSION:
		return (const GLubyte *)"4.5.0";
	case GL_SHADING_LANGUAGE_VERSION:
		return (const GLubyte *)"4.50";
	case GL_EXTENSIONS:
		return (const GLubyte *)"GL_ARB_framebuffer_object GL_ARB_vertex_buffer_object "
					"GL_ARB_shader_objects GL_ARB_compute_shader "
					"GL_EXT_texture_compression_s3tc GL_ARB_map_buffer_range "
					"GL_ARB_uniform_buffer_object GL_ARB_sampler_objects "
					"GL_ARB_instanced_arrays GL_ARB_draw_instanced "
					"GL_ARB_texture_storage GL_ARB_sync "
					"GL_ARB_vertex_array_object GL_ARB_shading_language_100";
	default:
		return (const GLubyte *)"";
	}
}

const GLubyte *APIENTRY glGetStringi(GLenum name, GLuint i)
{
	if (name == GL_EXTENSIONS && i < GL_EXT_COUNT)
		return (const GLubyte *)k_exts[i];
	return (const GLubyte *)"";
}

GLenum APIENTRY glGetError(void)
{
	GlCtx *c = cur();
	GLenum e;
	if (!c)
		return GL_INVALID_OPERATION;
	e = c->err;
	c->err = 0;
	return e;
}

static void get_int(GLenum pname, GLint *p)
{
	GlCtx *c = cur();
	if (!p)
		return;
	*p = 0;
	if (!c)
		return;
	switch (pname) {
	case GL_MAJOR_VERSION:
		*p = 4;
		break;
	case GL_MINOR_VERSION:
		*p = 5;
		break;
	case GL_NUM_EXTENSIONS:
		*p = GL_EXT_COUNT;
		break;
	case GL_MAX_TEXTURE_SIZE:
	case GL_MAX_RENDERBUFFER_SIZE:
	case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
		*p = 16384;
		break;
	case GL_MAX_VERTEX_ATTRIBS:
		*p = GL_ATTRS;
		break;
	case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
		*p = 4096;
		break;
	case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
		*p = 2048;
		break;
	case GL_MAX_GEOMETRY_UNIFORM_COMPONENTS:
		*p = 2048;
		break;
	case GL_MAX_UNIFORM_BUFFER_BINDINGS:
		*p = GL_UBOS;
		break;
	case GL_MAX_DRAW_BUFFERS:
	case GL_MAX_COLOR_ATTACHMENTS:
		*p = 8;
		break;
	case GL_MAX_SAMPLES:
		*p = 1;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
		*p = 128;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
		*p = 4;
		break;
	case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS:
		*p = 4;
		break;
	case GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS:
		*p = 1024;
		break;
	case GL_MAX_TEXTURE_IMAGE_UNITS:
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
		*p = GL_TEXUNITS;
		break;
	case GL_FRAMEBUFFER_BINDING:
		*p = (GLint)c->draw_fbo;
		break;
	case GL_READ_FRAMEBUFFER_BINDING:
		*p = (GLint)c->read_fbo;
		break;
	case GL_CURRENT_PROGRAM:
		*p = (GLint)c->prog;
		break;
	case GL_VERTEX_ARRAY_BINDING:
		*p = (GLint)c->vao;
		break;
	case GL_ARRAY_BUFFER_BINDING:
		*p = (GLint)c->buf_array;
		break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING:
		*p = (GLint)c->buf_element;
		break;
	case GL_ACTIVE_TEXTURE:
		*p = (GLint)(GL_TEXTURE0 + c->texunit);
		break;
	case GL_TEXTURE_BINDING_2D:
		*p = (GLint)c->tex2d[c->texunit];
		break;
	case GL_VIEWPORT:
		p[0] = c->vp[0];
		p[1] = c->vp[1];
		p[2] = c->vp[2];
		p[3] = c->vp[3];
		break;
	case GL_SCISSOR_BOX:
		p[0] = c->scissor[0];
		p[1] = c->scissor[1];
		p[2] = c->scissor[2];
		p[3] = c->scissor[3];
		break;
	case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
		*p = 32;
		break;
	case GL_CONTEXT_PROFILE_MASK:
		*p = GL_CONTEXT_CORE_PROFILE_BIT;
		break;
	default:
		*p = 0;
		break;
	}
}

void APIENTRY glGetIntegerv(GLenum pname, GLint *params)
{
	if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
		get_int(pname, params);
		return;
	}
	get_int(pname, params);
}

void APIENTRY glGetIntegeri_v(GLenum pname, GLuint i, GLint *params)
{
	if (!params)
		return;
	if (pname == GL_MAX_COMPUTE_WORK_GROUP_COUNT)
		*params = 65535;
	else if (pname == GL_MAX_COMPUTE_WORK_GROUP_SIZE)
		*params = (i == 0) ? 1024 : 1024;
	else
		*params = 0;
}

void APIENTRY glGetFloatv(GLenum pname, GLfloat *params)
{
	GLint i = 0;
	if (!params)
		return;
	glGetIntegerv(pname, &i);
	params[0] = (GLfloat)i;
}

void APIENTRY glGetBooleanv(GLenum pname, GLboolean *params)
{
	GlCtx *c = cur();
	if (!params)
		return;
	*params = GL_FALSE;
	if (!c)
		return;
	switch (pname) {
	case GL_BLEND:
		*params = (GLboolean)c->en_blend;
		break;
	case GL_DEPTH_TEST:
		*params = (GLboolean)c->en_depth;
		break;
	case GL_CULL_FACE:
		*params = (GLboolean)c->en_cull;
		break;
	case GL_SCISSOR_TEST:
		*params = (GLboolean)c->en_scissor;
		break;
	default:
		*params = GL_FALSE;
		break;
	}
}

void APIENTRY glGetDoublev(GLenum pname, GLdouble *params)
{
	GLfloat f = 0;
	if (!params)
		return;
	glGetFloatv(pname, &f);
	params[0] = f;
}

static int cap_slot(GLenum cap, int **slot)
{
	GlCtx *c = cur();
	if (!c)
		return 0;
	switch (cap) {
	case GL_BLEND:
		*slot = &c->en_blend;
		return 1;
	case GL_DEPTH_TEST:
		*slot = &c->en_depth;
		return 1;
	case GL_CULL_FACE:
		*slot = &c->en_cull;
		return 1;
	case GL_SCISSOR_TEST:
		*slot = &c->en_scissor;
		return 1;
	case GL_TEXTURE_2D:
		*slot = &c->en_tex2d;
		return 1;
	default:
		return 0;
	}
}

void APIENTRY glEnable(GLenum cap)
{
	int *s;
	if (cap_slot(cap, &s))
		*s = 1;
}

void APIENTRY glDisable(GLenum cap)
{
	int *s;
	if (cap_slot(cap, &s))
		*s = 0;
}

GLboolean APIENTRY glIsEnabled(GLenum cap)
{
	int *s;
	if (!cap_slot(cap, &s))
		return GL_FALSE;
	return *s ? GL_TRUE : GL_FALSE;
}

void APIENTRY glEnablei(GLenum cap, GLuint i)
{
	(void)i;
	glEnable(cap);
}

void APIENTRY glDisablei(GLenum cap, GLuint i)
{
	(void)i;
	glDisable(cap);
}

void APIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->vp[0] = x;
	c->vp[1] = y;
	c->vp[2] = w;
	c->vp[3] = h;
}

void APIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->scissor[0] = x;
	c->scissor[1] = y;
	c->scissor[2] = w;
	c->scissor[3] = h;
}

void APIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->clear_c[0] = r;
	c->clear_c[1] = g;
	c->clear_c[2] = b;
	c->clear_c[3] = a;
}

void APIENTRY glClearDepth(GLclampd z)
{
	GlCtx *c = cur();
	if (c)
		c->clear_z = z;
}

void APIENTRY glClearDepthf(GLfloat z)
{
	glClearDepth(z);
}

void APIENTRY glClearStencil(GLint s)
{
	(void)s;
}

void APIENTRY glClear(GLbitfield mask)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	if (!c)
		return;
	bind_draw_target(c, &rt, &tex);
	if (!rt.color)
		return;
	if (mask & GL_COLOR_BUFFER_BIT)
		swrast_clear_color(&rt, pack_argb(c->clear_c[0], c->clear_c[1], c->clear_c[2],
						  c->clear_c[3]));
	if (mask & GL_DEPTH_BUFFER_BIT)
		swrast_clear_depth(&rt, (float)c->clear_z);
}

void APIENTRY glFinish(void)
{
	swrast_flush();
}

void APIENTRY glFlush(void)
{
	swrast_flush();
}

void APIENTRY glHint(GLenum t, GLenum m)
{
	(void)t;
	(void)m;
}

void APIENTRY glPixelStorei(GLenum pname, GLint param)
{
	GlCtx *c = cur();
	if (c && pname == GL_UNPACK_ALIGNMENT)
		c->unpack_align = param > 0 ? param : 4;
}

void APIENTRY glPixelStoref(GLenum pname, GLfloat param)
{
	glPixelStorei(pname, (GLint)param);
}

void APIENTRY glBlendFunc(GLenum s, GLenum d)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->blend_src = s;
	c->blend_dst = d;
}

void APIENTRY glBlendFuncSeparate(GLenum rgb_s, GLenum rgb_d, GLenum a_s, GLenum a_d)
{
	(void)a_s;
	(void)a_d;
	glBlendFunc(rgb_s, rgb_d);
}

void APIENTRY glBlendEquation(GLenum mode)
{
	GlCtx *c = cur();
	if (c)
		c->blend_op = mode;
}

void APIENTRY glBlendEquationSeparate(GLenum rgb, GLenum a)
{
	(void)a;
	glBlendEquation(rgb);
}

void APIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->color_mask[0] = r;
	c->color_mask[1] = g;
	c->color_mask[2] = b;
	c->color_mask[3] = a;
}

void APIENTRY glColorMaski(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	(void)i;
	glColorMask(r, g, b, a);
}

void APIENTRY glDepthFunc(GLenum f)
{
	GlCtx *c = cur();
	if (c)
		c->depth_func = f;
}

void APIENTRY glDepthMask(GLboolean f)
{
	GlCtx *c = cur();
	if (c)
		c->depth_mask = f;
}

void APIENTRY glDepthRange(GLclampd n, GLclampd f)
{
	(void)n;
	(void)f;
}

void APIENTRY glCullFace(GLenum m)
{
	GlCtx *c = cur();
	if (c)
		c->cull_face = m;
}

void APIENTRY glFrontFace(GLenum m)
{
	GlCtx *c = cur();
	if (c)
		c->front_face = m;
}

void APIENTRY glPolygonMode(GLenum face, GLenum mode)
{
	(void)face;
	(void)mode;
}

void APIENTRY glPolygonOffset(GLfloat f, GLfloat u)
{
	(void)f;
	(void)u;
}

void APIENTRY glLineWidth(GLfloat w)
{
	(void)w;
}

void APIENTRY glPointSize(GLfloat s)
{
	(void)s;
}

void APIENTRY glStencilFunc(GLenum f, GLint ref, GLuint mask)
{
	(void)f;
	(void)ref;
	(void)mask;
}

void APIENTRY glStencilOp(GLenum a, GLenum b, GLenum c)
{
	(void)a;
	(void)b;
	(void)c;
}

void APIENTRY glStencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask)
{
	(void)face;
	glStencilFunc(f, ref, mask);
}

void APIENTRY glStencilOpSeparate(GLenum face, GLenum a, GLenum b, GLenum c)
{
	(void)face;
	glStencilOp(a, b, c);
}

void APIENTRY glStencilMask(GLuint m)
{
	(void)m;
}

void APIENTRY glPolygonStipple(const GLubyte *m)
{
	(void)m;
}

void APIENTRY glReadBuffer(GLenum m)
{
	(void)m;
}

void APIENTRY glDrawBuffer(GLenum m)
{
	(void)m;
}

void APIENTRY glDrawBuffers(GLsizei n, const GLenum *bufs)
{
	(void)n;
	(void)bufs;
}

void APIENTRY glActiveTexture(GLenum t)
{
	GlCtx *c = cur();
	if (c && t >= GL_TEXTURE0 && t < GL_TEXTURE0 + GL_TEXUNITS)
		c->texunit = t - GL_TEXTURE0;
}

void APIENTRY glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
			   GLvoid *pixels)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	int i, j;
	(void)format;
	(void)type;
	if (!c || !pixels || w <= 0 || h <= 0)
		return;
	bind_draw_target(c, &rt, &tex);
	if (!rt.color)
		return;
	swrast_flush_if_pending(rt.color);
	for (j = 0; j < h; j++) {
		int sy = rt.height - (y + j + 1);
		unsigned char *dst = (unsigned char *)pixels + (size_t)j * w * 4;
		if (sy < 0 || sy >= rt.height)
			continue;
		for (i = 0; i < w; i++) {
			int sx = x + i;
			uint32_t p = (sx >= 0 && sx < rt.width) ? rt.color[sy * rt.width + sx] : 0;
			dst[i * 4 + 0] = (unsigned char)((p >> 16) & 0xff);
			dst[i * 4 + 1] = (unsigned char)((p >> 8) & 0xff);
			dst[i * 4 + 2] = (unsigned char)(p & 0xff);
			dst[i * 4 + 3] = (unsigned char)((p >> 24) & 0xff);
		}
	}
}

/* ---- textures ---- */

void APIENTRY glGenTextures(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c || n < 0)
		return;
	for (i = 0; i < n; i++) {
		GlTex **slot = (GlTex **)grow((void **)&c->share->tex, &c->share->ntex,
					     &c->share->ctex, sizeof(GlTex *));
		GlTex *t;
		if (!slot)
			return;
		t = (GlTex *)calloc(1, sizeof(*t));
		t->id = alloc_id(c->share);
		*slot = t;
		c->share->ntex++;
		if (ids)
			ids[i] = t->id;
	}
}

void APIENTRY glDeleteTextures(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++) {
		for (j = 0; j < c->share->ntex; j++) {
			if (c->share->tex[j] && c->share->tex[j]->id == ids[i]) {
				tex_free(c->share->tex[j]);
				c->share->tex[j] = c->share->tex[--c->share->ntex];
				break;
			}
		}
	}
}

void APIENTRY glBindTexture(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	GlTex *t;
	if (!c)
		return;
	if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_2D_ARRAY)
		c->tex2d[c->texunit] = id;
	t = tex_get(c->share, id);
	if (t)
		t->target = target;
}

GLboolean APIENTRY glIsTexture(GLuint id)
{
	GlCtx *c = cur();
	return (c && tex_get(c->share, id)) ? GL_TRUE : GL_FALSE;
}

static void tex_alloc(GlTex *t, int w, int h)
{
	size_t n;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (t->pixels && t->w == w && t->h == h)
		return;
	free(t->pixels);
	n = (size_t)w * (size_t)h;
	t->pixels = (uint32_t *)calloc(n, 4);
	t->w = w;
	t->h = h;
}

static void unpack_rgba(GlTex *t, GLenum format, GLenum type, const void *src, int unpack)
{
	int x, y;
	const unsigned char *s = (const unsigned char *)src;
	int bpp = 4;
	(void)unpack;
	if (!src || !t->pixels)
		return;
	if (type != GL_UNSIGNED_BYTE)
		return;
	if (format == GL_RGB || format == GL_BGR)
		bpp = 3;
	else if (format == GL_RED || format == GL_ALPHA || format == GL_LUMINANCE)
		bpp = 1;
	for (y = 0; y < t->h; y++) {
		const unsigned char *row = s + (size_t)y * t->w * bpp;
		uint32_t *d = t->pixels + (size_t)y * t->w;
		for (x = 0; x < t->w; x++) {
			const unsigned char *p = row + (size_t)x * bpp;
			unsigned r = 255, g = 255, b = 255, a = 255;
			if (format == GL_RGBA || format == 0x1908) {
				r = p[0];
				g = p[1];
				b = p[2];
				a = p[3];
			} else if (format == GL_BGRA) {
				b = p[0];
				g = p[1];
				r = p[2];
				a = p[3];
			} else if (format == GL_RGB) {
				r = p[0];
				g = p[1];
				b = p[2];
			} else if (format == GL_BGR) {
				b = p[0];
				g = p[1];
				r = p[2];
			} else if (bpp == 1) {
				r = g = b = p[0];
			}
			d[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	t->has = 1;
}

void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			   GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	(void)target;
	(void)internal;
	(void)border;
	if (!c || level != 0)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t)
		return;
	tex_alloc(t, w, h);
	if (pixels)
		unpack_rgba(t, format, type, pixels, c->unpack_align);
	gl_trace("TexImage2D %ux%u fmt=%x", w, h, format);
}

void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLsizei w,
			      GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	int x, y, bpp = 4;
	const unsigned char *s = (const unsigned char *)pixels;
	(void)target;
	if (!c || level != 0 || !pixels)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t || !t->pixels)
		return;
	if (format == GL_RGB || format == GL_BGR)
		bpp = 3;
	if (type != GL_UNSIGNED_BYTE)
		return;
	for (y = 0; y < h; y++) {
		int dy = yoff + y;
		if (dy < 0 || dy >= t->h)
			continue;
		for (x = 0; x < w; x++) {
			int dx = xoff + x;
			const unsigned char *p;
			unsigned r, g, b, a = 255;
			if (dx < 0 || dx >= t->w)
				continue;
			p = s + ((size_t)y * w + x) * bpp;
			if (bpp == 4) {
				if (format == GL_BGRA) {
					b = p[0];
					g = p[1];
					r = p[2];
					a = p[3];
				} else {
					r = p[0];
					g = p[1];
					b = p[2];
					a = p[3];
				}
			} else {
				r = p[0];
				g = p[1];
				b = p[2];
			}
			t->pixels[dy * t->w + dx] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	t->has = 1;
}

void APIENTRY glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			   GLsizei depth, GLint border, GLenum format, GLenum type,
			   const void *pixels)
{
	(void)depth;
	glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}

void APIENTRY glTexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
	(void)levels;
	glTexImage2D(target, 0, (GLint)internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void APIENTRY glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w,
				     GLsizei h, GLint border, GLsizei imageSize, const void *data)
{
	GlCtx *c = cur();
	GlTex *t;
	int dxt1, bx, by;
	(void)target;
	(void)border;
	(void)imageSize;
	if (!c || level != 0 || !data)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t)
		return;
	tex_alloc(t, w, h);
	dxt1 = (internal == 0x83F0 || internal == 0x83F1); /* DXT1 */
	for (by = 0; by < (h + 3) / 4; by++) {
		for (bx = 0; bx < (w + 3) / 4; bx++) {
			const unsigned char *blk =
				(const unsigned char *)data +
				((size_t)by * ((w + 3) / 4) + bx) * (dxt1 ? 8u : 16u);
			unsigned char rgba[16 * 4];
			int px, py;
			if (dxt1)
				bcdec_bc1(blk, rgba, 16);
			else if (internal == 0x83F2)
				bcdec_bc2(blk, rgba, 16);
			else
				bcdec_bc3(blk, rgba, 16);
			for (py = 0; py < 4; py++)
				for (px = 0; px < 4; px++) {
					int x = bx * 4 + px, y = by * 4 + py;
					unsigned char *s;
					if (x >= w || y >= h)
						continue;
					s = rgba + (py * 4 + px) * 4;
					t->pixels[y * w + x] =
						((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) |
						((uint32_t)s[1] << 8) | s[2];
				}
		}
	}
	t->has = 1;
}

void APIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	(void)target;
	(void)pname;
	(void)param;
}

void APIENTRY glTexParameterf(GLenum t, GLenum p, GLfloat v)
{
	glTexParameteri(t, p, (GLint)v);
}

void APIENTRY glTexParameteriv(GLenum t, GLenum p, const GLint *v)
{
	if (v)
		glTexParameteri(t, p, v[0]);
}

void APIENTRY glTexParameterfv(GLenum t, GLenum p, const GLfloat *v)
{
	if (v)
		glTexParameterf(t, p, v[0]);
}

void APIENTRY glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
	GlCtx *c = cur();
	GlTex *t;
	int i;
	(void)target;
	(void)level;
	(void)format;
	(void)type;
	if (!c || !pixels)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t || !t->pixels)
		return;
	for (i = 0; i < t->w * t->h; i++) {
		uint32_t p = t->pixels[i];
		unsigned char *d = (unsigned char *)pixels + (size_t)i * 4;
		d[0] = (unsigned char)((p >> 16) & 0xff);
		d[1] = (unsigned char)((p >> 8) & 0xff);
		d[2] = (unsigned char)(p & 0xff);
		d[3] = (unsigned char)((p >> 24) & 0xff);
	}
}

void APIENTRY glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
	GlCtx *c = cur();
	GlTex *t;
	(void)target;
	(void)level;
	if (!c || !params)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	*params = 0;
	if (!t)
		return;
	if (pname == GL_TEXTURE_WIDTH)
		*params = t->w;
	else if (pname == GL_TEXTURE_HEIGHT)
		*params = t->h;
	else if (pname == GL_TEXTURE_INTERNAL_FORMAT)
		*params = GL_RGBA8;
}

void APIENTRY glGetTexParameteriv(GLenum t, GLenum p, GLint *params)
{
	(void)t;
	(void)p;
	if (params)
		*params = 0;
}

void APIENTRY glGenerateMipmap(GLenum target)
{
	(void)target;
}

void APIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff, GLint x,
				  GLint y, GLsizei w, GLsizei h)
{
	GlCtx *c = cur();
	GlTex *t;
	SwRast rt;
	SwTex tex;
	int i, j;
	(void)target;
	(void)level;
	if (!c)
		return;
	t = tex_get(c->share, c->tex2d[c->texunit]);
	if (!t || !t->pixels)
		return;
	bind_draw_target(c, &rt, &tex);
	if (!rt.color)
		return;
	swrast_flush_if_pending(rt.color);
	for (j = 0; j < h; j++) {
		int sy = rt.height - (y + j + 1);
		int dy = yoff + j;
		if (sy < 0 || sy >= rt.height || dy < 0 || dy >= t->h)
			continue;
		for (i = 0; i < w; i++) {
			int sx = x + i, dx = xoff + i;
			if (sx < 0 || sx >= rt.width || dx < 0 || dx >= t->w)
				continue;
			t->pixels[dy * t->w + dx] = rt.color[sy * rt.width + sx];
		}
	}
}

/* ---- buffers / VAO ---- */

static GLuint *buf_binding(GlCtx *c, GLenum target)
{
	if (target == GL_ARRAY_BUFFER)
		return &c->buf_array;
	if (target == GL_ELEMENT_ARRAY_BUFFER)
		return &c->buf_element;
	if (target == GL_UNIFORM_BUFFER)
		return &c->buf_uniform[0];
	return &c->buf_array;
}

void APIENTRY glGenBuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlBuf **slot = (GlBuf **)grow((void **)&c->share->buf, &c->share->nbuf,
					     &c->share->cbuf, sizeof(GlBuf *));
		GlBuf *b;
		if (!slot)
			return;
		b = (GlBuf *)calloc(1, sizeof(*b));
		b->id = alloc_id(c->share);
		*slot = b;
		c->share->nbuf++;
		if (ids)
			ids[i] = b->id;
	}
}

void APIENTRY glDeleteBuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nbuf; j++)
			if (c->share->buf[j] && c->share->buf[j]->id == ids[i]) {
				buf_free(c->share->buf[j]);
				c->share->buf[j] = c->share->buf[--c->share->nbuf];
				break;
			}
}

void APIENTRY glBindBuffer(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	GLuint *b;
	GlVao *vao;
	if (!c)
		return;
	b = buf_binding(c, target);
	*b = id;
	if (target == GL_ELEMENT_ARRAY_BUFFER) {
		vao = vao_get(c->share, c->vao);
		if (vao)
			vao->ebo = id;
	}
}

void APIENTRY glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GlCtx *c = cur();
	GlBuf *b;
	(void)usage;
	if (!c || size < 0)
		return;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return;
	free(b->data);
	b->data = size ? (unsigned char *)calloc((size_t)size, 1) : NULL;
	b->size = (size_t)size;
	if (data && b->data)
		memcpy(b->data, data, (size_t)size);
}

void APIENTRY glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GlCtx *c = cur();
	GlBuf *b;
	if (!c || !data || size < 0)
		return;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b || !b->data)
		return;
	if ((size_t)offset + (size_t)size > b->size)
		return;
	memcpy(b->data + offset, data, (size_t)size);
}

void *APIENTRY glMapBuffer(GLenum target, GLenum access)
{
	GlCtx *c = cur();
	GlBuf *b;
	(void)access;
	if (!c)
		return NULL;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return NULL;
	b->mapped = 1;
	return b->data;
}

void *APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield acc)
{
	unsigned char *p = (unsigned char *)glMapBuffer(target, 0);
	(void)acc;
	(void)length;
	return p ? p + offset : NULL;
}

GLboolean APIENTRY glUnmapBuffer(GLenum target)
{
	GlCtx *c = cur();
	GlBuf *b;
	if (!c)
		return GL_FALSE;
	b = buf_get(c->share, *buf_binding(c, target));
	if (!b)
		return GL_FALSE;
	b->mapped = 0;
	return GL_TRUE;
}

void APIENTRY glBindBufferBase(GLenum target, GLuint index, GLuint id)
{
	GlCtx *c = cur();
	if (!c)
		return;
	if (target == GL_UNIFORM_BUFFER && index < GL_UBOS)
		c->buf_uniform[index] = id;
	else
		glBindBuffer(target, id);
}

void APIENTRY glBindBufferRange(GLenum target, GLuint index, GLuint id, GLintptr off, GLsizeiptr sz)
{
	(void)off;
	(void)sz;
	glBindBufferBase(target, index, id);
}

void APIENTRY glCopyBufferSubData(GLenum src, GLenum dst, GLintptr r, GLintptr w, GLsizeiptr n)
{
	GlCtx *c = cur();
	GlBuf *a, *b;
	if (!c)
		return;
	a = buf_get(c->share, *buf_binding(c, src));
	b = buf_get(c->share, *buf_binding(c, dst));
	if (!a || !b || !a->data || !b->data)
		return;
	if ((size_t)r + (size_t)n > a->size || (size_t)w + (size_t)n > b->size)
		return;
	memcpy(b->data + w, a->data + r, (size_t)n);
}

void APIENTRY glGenVertexArrays(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlVao **slot = (GlVao **)grow((void **)&c->share->vao, &c->share->nvao,
					     &c->share->cvao, sizeof(GlVao *));
		GlVao *v;
		if (!slot)
			return;
		v = (GlVao *)calloc(1, sizeof(*v));
		v->id = alloc_id(c->share);
		*slot = v;
		c->share->nvao++;
		if (ids)
			ids[i] = v->id;
	}
}

void APIENTRY glDeleteVertexArrays(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nvao; j++)
			if (c->share->vao[j] && c->share->vao[j]->id == ids[i]) {
				free(c->share->vao[j]);
				c->share->vao[j] = c->share->vao[--c->share->nvao];
				break;
			}
}

void APIENTRY glBindVertexArray(GLuint id)
{
	GlCtx *c = cur();
	if (c)
		c->vao = id;
}

void APIENTRY glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
				    const void *ptr)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (!v)
		return;
	v->attr[i].size = size;
	v->attr[i].type = type;
	v->attr[i].normalized = norm;
	v->attr[i].stride = stride;
	v->attr[i].offset = (GLsizeiptr)(uintptr_t)ptr;
	v->attr[i].buf = c->buf_array;
}

void APIENTRY glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride,
				     const void *ptr)
{
	glVertexAttribPointer(i, size, type, GL_FALSE, stride, ptr);
}

void APIENTRY glEnableVertexAttribArray(GLuint i)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].enabled = 1;
}

void APIENTRY glDisableVertexAttribArray(GLuint i)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].enabled = 0;
}

void APIENTRY glVertexAttribDivisor(GLuint i, GLuint d)
{
	GlCtx *c = cur();
	GlVao *v;
	if (!c || i >= GL_ATTRS)
		return;
	v = vao_get(c->share, c->vao);
	if (v)
		v->attr[i].divisor = d;
}

void APIENTRY glBindAttribLocation(GLuint prog, GLuint index, const GLchar *name)
{
	(void)prog;
	(void)index;
	(void)name;
}

GLint APIENTRY glGetAttribLocation(GLuint prog, const GLchar *name)
{
	(void)prog;
	if (!name)
		return -1;
	if (strstr(name, "Position") || strstr(name, "position") || strcmp(name, "in_Position") == 0)
		return 0;
	if (strstr(name, "Coord") || strstr(name, "uv") || strstr(name, "Tex"))
		return 1;
	if (strstr(name, "Color") || strstr(name, "colour"))
		return 2;
	return 0;
}

/* ---- shaders: accept, do not execute ---- */

GLuint APIENTRY glCreateShader(GLenum type)
{
	GlCtx *c = cur();
	GlShader **slot;
	GlShader *s;
	if (!c)
		return 0;
	slot = (GlShader **)grow((void **)&c->share->sh, &c->share->nsh, &c->share->csh,
				 sizeof(GlShader *));
	if (!slot)
		return 0;
	s = (GlShader *)calloc(1, sizeof(*s));
	s->id = alloc_id(c->share);
	s->type = type;
	*slot = s;
	c->share->nsh++;
	gl_log("CreateShader type=%x id=%u", type, s->id);
	return s->id;
}

void APIENTRY glShaderSource(GLuint id, GLsizei count, const GLchar *const *str, const GLint *len)
{
	GlCtx *c = cur();
	GlShader *s;
	int i;
	size_t n = 0;
	if (!c || !str)
		return;
	s = sh_get(c->share, id);
	if (!s)
		return;
	for (i = 0; i < count; i++) {
		size_t L = len && len[i] >= 0 ? (size_t)len[i] : (str[i] ? strlen(str[i]) : 0);
		n += L;
	}
	free(s->src);
	s->src = (char *)malloc(n + 1);
	if (!s->src)
		return;
	n = 0;
	for (i = 0; i < count; i++) {
		size_t L = len && len[i] >= 0 ? (size_t)len[i] : (str[i] ? strlen(str[i]) : 0);
		if (str[i] && L) {
			memcpy(s->src + n, str[i], L);
			n += L;
		}
	}
	s->src[n] = 0;
}

void APIENTRY glCompileShader(GLuint id)
{
	(void)id;
}

void APIENTRY glDeleteShader(GLuint id)
{
	GlCtx *c = cur();
	int j;
	if (!c)
		return;
	for (j = 0; j < c->share->nsh; j++)
		if (c->share->sh[j] && c->share->sh[j]->id == id) {
			free(c->share->sh[j]->src);
			free(c->share->sh[j]);
			c->share->sh[j] = c->share->sh[--c->share->nsh];
			break;
		}
}

GLuint APIENTRY glCreateProgram(void)
{
	GlCtx *c = cur();
	GlProg **slot;
	GlProg *p;
	if (!c)
		return 0;
	slot = (GlProg **)grow((void **)&c->share->prog, &c->share->nprog, &c->share->cprog,
			      sizeof(GlProg *));
	if (!slot)
		return 0;
	p = (GlProg *)calloc(1, sizeof(*p));
	p->id = alloc_id(c->share);
	*slot = p;
	c->share->nprog++;
	return p->id;
}

void APIENTRY glAttachShader(GLuint prog, GLuint sh)
{
	(void)prog;
	(void)sh;
}

void APIENTRY glDetachShader(GLuint prog, GLuint sh)
{
	(void)prog;
	(void)sh;
}

void APIENTRY glLinkProgram(GLuint id)
{
	GlCtx *c = cur();
	GlProg *p = c ? prog_get(c->share, id) : NULL;
	if (p)
		p->linked = 1;
}

void APIENTRY glUseProgram(GLuint id)
{
	GlCtx *c = cur();
	if (c)
		c->prog = id;
}

void APIENTRY glDeleteProgram(GLuint id)
{
	GlCtx *c = cur();
	int j;
	if (!c)
		return;
	for (j = 0; j < c->share->nprog; j++)
		if (c->share->prog[j] && c->share->prog[j]->id == id) {
			free(c->share->prog[j]);
			c->share->prog[j] = c->share->prog[--c->share->nprog];
			break;
		}
}

void APIENTRY glGetShaderiv(GLuint id, GLenum pname, GLint *params)
{
	(void)id;
	if (!params)
		return;
	if (pname == GL_COMPILE_STATUS)
		*params = GL_TRUE;
	else if (pname == GL_INFO_LOG_LENGTH)
		*params = 1;
	else
		*params = 0;
}

void APIENTRY glGetProgramiv(GLuint id, GLenum pname, GLint *params)
{
	(void)id;
	if (!params)
		return;
	if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS)
		*params = GL_TRUE;
	else if (pname == GL_INFO_LOG_LENGTH)
		*params = 1;
	else if (pname == GL_ACTIVE_UNIFORMS || pname == GL_ACTIVE_ATTRIBUTES)
		*params = 0;
	else
		*params = 0;
}

void APIENTRY glGetShaderInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
	(void)id;
	if (len)
		*len = 0;
	if (log && n > 0)
		log[0] = 0;
}

void APIENTRY glGetProgramInfoLog(GLuint id, GLsizei n, GLsizei *len, GLchar *log)
{
	glGetShaderInfoLog(id, n, len, log);
}

GLint APIENTRY glGetUniformLocation(GLuint prog, const GLchar *name)
{
	(void)prog;
	return name ? (GLint)(strlen(name) % 240 + 1) : -1;
}

GLuint APIENTRY glGetUniformBlockIndex(GLuint prog, const GLchar *name)
{
	(void)prog;
	(void)name;
	return 0;
}

void APIENTRY glUniformBlockBinding(GLuint p, GLuint i, GLuint b)
{
	(void)p;
	(void)i;
	(void)b;
}

void APIENTRY glBindFragDataLocation(GLuint p, GLuint c, const GLchar *n)
{
	(void)p;
	(void)c;
	(void)n;
}

void APIENTRY glUniform1i(GLint loc, GLint v)
{
	(void)loc;
	(void)v;
}
void APIENTRY glUniform1f(GLint loc, GLfloat v)
{
	(void)loc;
	(void)v;
}
void APIENTRY glUniform2f(GLint loc, GLfloat a, GLfloat b)
{
	(void)loc;
	(void)a;
	(void)b;
}
void APIENTRY glUniform3f(GLint loc, GLfloat a, GLfloat b, GLfloat c)
{
	(void)loc;
	(void)a;
	(void)b;
	(void)c;
}
void APIENTRY glUniform4f(GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
	(void)loc;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
}
void APIENTRY glUniform1iv(GLint loc, GLsizei n, const GLint *v)
{
	(void)loc;
	(void)n;
	(void)v;
}
void APIENTRY glUniform1fv(GLint loc, GLsizei n, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)v;
}
void APIENTRY glUniform2fv(GLint loc, GLsizei n, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)v;
}
void APIENTRY glUniform3fv(GLint loc, GLsizei n, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)v;
}
void APIENTRY glUniform4fv(GLint loc, GLsizei n, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)v;
}
void APIENTRY glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)t;
	(void)v;
}
void APIENTRY glUniformMatrix3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)t;
	(void)v;
}

void APIENTRY glUniformMatrix4x3fv(GLint loc, GLsizei n, GLboolean t, const GLfloat *v)
{
	(void)loc;
	(void)n;
	(void)t;
	(void)v;
}

void APIENTRY glTransformFeedbackVaryings(GLuint program, GLsizei count, const GLchar *const *varyings,
					  GLenum bufferMode)
{
	(void)program;
	(void)count;
	(void)varyings;
	(void)bufferMode;
}

void APIENTRY glBeginTransformFeedback(GLenum primitiveMode)
{
	(void)primitiveMode;
}

void APIENTRY glEndTransformFeedback(void)
{
}

/* ---- FBO ---- */

void APIENTRY glGenFramebuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlFbo **slot = (GlFbo **)grow((void **)&c->share->fbo, &c->share->nfbo,
					     &c->share->cfbo, sizeof(GlFbo *));
		GlFbo *f;
		if (!slot)
			return;
		f = (GlFbo *)calloc(1, sizeof(*f));
		f->id = alloc_id(c->share);
		*slot = f;
		c->share->nfbo++;
		if (ids)
			ids[i] = f->id;
	}
}

void APIENTRY glDeleteFramebuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nfbo; j++)
			if (c->share->fbo[j] && c->share->fbo[j]->id == ids[i]) {
				free(c->share->fbo[j]);
				c->share->fbo[j] = c->share->fbo[--c->share->nfbo];
				break;
			}
}

void APIENTRY glBindFramebuffer(GLenum target, GLuint id)
{
	GlCtx *c = cur();
	if (!c)
		return;
	if (target == GL_READ_FRAMEBUFFER)
		c->read_fbo = id;
	else if (target == GL_DRAW_FRAMEBUFFER)
		c->draw_fbo = id;
	else
		c->draw_fbo = c->read_fbo = id;
}

void APIENTRY glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex,
				     GLint level)
{
	GlCtx *c = cur();
	GlFbo *f;
	(void)target;
	(void)level;
	if (!c)
		return;
	f = fbo_get(c->share, c->draw_fbo);
	if (!f)
		return;
	if (att == GL_DEPTH_ATTACHMENT || att == GL_DEPTH_STENCIL_ATTACHMENT) {
		f->depth = tex;
		f->depth_tgt = textarget;
	} else {
		f->color = tex;
		f->color_tgt = textarget;
	}
}

void APIENTRY glFramebufferTexture(GLenum target, GLenum att, GLuint tex, GLint level)
{
	glFramebufferTexture2D(target, att, GL_TEXTURE_2D, tex, level);
}

void APIENTRY glFramebufferRenderbuffer(GLenum target, GLenum att, GLenum rbt, GLuint rb)
{
	(void)rbt;
	glFramebufferTexture2D(target, att, GL_TEXTURE_2D, rb, 0);
}

GLenum APIENTRY glCheckFramebufferStatus(GLenum target)
{
	(void)target;
	return GL_FRAMEBUFFER_COMPLETE;
}

void APIENTRY glGenRenderbuffers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlRbo **slot = (GlRbo **)grow((void **)&c->share->rbo, &c->share->nrbo,
					     &c->share->crbo, sizeof(GlRbo *));
		GlRbo *r;
		if (!slot)
			return;
		r = (GlRbo *)calloc(1, sizeof(*r));
		r->id = alloc_id(c->share);
		*slot = r;
		c->share->nrbo++;
		if (ids)
			ids[i] = r->id;
	}
}

void APIENTRY glDeleteRenderbuffers(GLsizei n, const GLuint *ids)
{
	GlCtx *c = cur();
	int i, j;
	if (!c || !ids)
		return;
	for (i = 0; i < n; i++)
		for (j = 0; j < c->share->nrbo; j++)
			if (c->share->rbo[j] && c->share->rbo[j]->id == ids[i]) {
				free(c->share->rbo[j]->color);
				free(c->share->rbo[j]->depth);
				free(c->share->rbo[j]);
				c->share->rbo[j] = c->share->rbo[--c->share->nrbo];
				break;
			}
}

void APIENTRY glBindRenderbuffer(GLenum target, GLuint id)
{
	(void)target;
	(void)id;
}

void APIENTRY glRenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
	(void)target;
	(void)internal;
	(void)w;
	(void)h;
}

void APIENTRY glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
				GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
	GlCtx *c = cur();
	SwRast src, dst;
	SwTex tex;
	int x, y, sw, sh, dw, dh;
	(void)filter;
	if (!c || !(mask & GL_COLOR_BUFFER_BIT))
		return;
	bind_read_target(c, &src);
	bind_draw_target(c, &dst, &tex);
	if (!src.color || !dst.color)
		return;
	if (sx1 < sx0) {
		GLint t = sx0;
		sx0 = sx1;
		sx1 = t;
	}
	if (sy1 < sy0) {
		GLint t = sy0;
		sy0 = sy1;
		sy1 = t;
	}
	if (dx1 < dx0) {
		GLint t = dx0;
		dx0 = dx1;
		dx1 = t;
	}
	if (dy1 < dy0) {
		GLint t = dy0;
		dy0 = dy1;
		dy1 = t;
	}
	/* GL origin is bottom-left; our color buffers are top-down. */
	sy0 = src.height - sy0;
	sy1 = src.height - sy1;
	{
		GLint t = sy0;
		sy0 = sy1;
		sy1 = t;
	}
	dy0 = dst.height - dy0;
	dy1 = dst.height - dy1;
	{
		GLint t = dy0;
		dy0 = dy1;
		dy1 = t;
	}
	sw = sx1 - sx0;
	sh = sy1 - sy0;
	dw = dx1 - dx0;
	dh = dy1 - dy0;
	if (sw < 1 || sh < 1 || dw < 1 || dh < 1)
		return;
	for (y = 0; y < dh; y++) {
		int sy = sy0 + (int)((int64_t)y * sh / dh);
		int dy = dy0 + y;
		if (sy < 0 || sy >= src.height || dy < 0 || dy >= dst.height)
			continue;
		for (x = 0; x < dw; x++) {
			int sx = sx0 + (int)((int64_t)x * sw / dw);
			int dx = dx0 + x;
			if (sx < 0 || sx >= src.width || dx < 0 || dx >= dst.width)
				continue;
			dst.color[dy * dst.width + dx] = src.color[sy * src.width + sx];
		}
	}
}

/* ---- draw ---- */

void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, 0, NULL, (unsigned)first, 0);
}

void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, type, indices, 0, 0);
}

void APIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
				  const void *indices)
{
	(void)start;
	(void)end;
	glDrawElements(mode, count, type, indices);
}

void APIENTRY glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei prim)
{
	GLsizei i;
	for (i = 0; i < prim; i++)
		glDrawArrays(mode, first, count);
}

void APIENTRY glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx,
				      GLsizei prim)
{
	GLsizei i;
	for (i = 0; i < prim; i++)
		glDrawElements(mode, count, type, idx);
}

void APIENTRY glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
				       GLint basevertex)
{
	GlCtx *c = cur();
	if (c)
		draw_tris(c, mode, (unsigned)count, type, indices, 0, basevertex);
}

void APIENTRY glDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type,
						const void *indices, GLsizei instancecount,
						GLint basevertex)
{
	GLsizei i;
	for (i = 0; i < instancecount; i++)
		glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

void APIENTRY glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
				  const void *const *indices, GLsizei drawcount)
{
	GLsizei i;
	if (!count || !indices)
		return;
	for (i = 0; i < drawcount; i++)
		glDrawElements(mode, count[i], type, indices[i]);
}

void APIENTRY glMultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count, GLenum type,
					    const void *const *indices, GLsizei drawcount,
					    const GLint *basevertex)
{
	GLsizei i;
	if (!count || !indices)
		return;
	for (i = 0; i < drawcount; i++)
		glDrawElementsBaseVertex(mode, count[i], type, indices[i],
					 basevertex ? basevertex[i] : 0);
}

void APIENTRY glBegin(GLenum mode)
{
	GlCtx *c = cur();
	if (!c)
		return;
	c->in_begin = 1;
	c->begin_mode = mode;
	c->nimm = 0;
}

static void imm_push(GlCtx *c, float x, float y, float z)
{
	SwVert *v;
	if (!c || c->nimm >= GL_IMM_MAX)
		return;
	v = &c->imm[c->nimm++];
	v->x = x;
	v->y = y;
	v->z = z;
	v->rhw = 1;
	v->u = c->cur_u;
	v->v = c->cur_v;
	v->color = c->cur_color;
}

void APIENTRY glVertex2f(GLfloat x, GLfloat y)
{
	GlCtx *c = cur();
	if (c)
		imm_push(c, x, y, 0);
}

void APIENTRY glVertex2fv(const GLfloat *v)
{
	if (v)
		glVertex2f(v[0], v[1]);
}

void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
	GlCtx *c = cur();
	if (c)
		imm_push(c, x, y, z);
}

void APIENTRY glTexCoord2f(GLfloat s, GLfloat t)
{
	GlCtx *c = cur();
	if (c) {
		c->cur_u = s;
		c->cur_v = t;
	}
}

void APIENTRY glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	GlCtx *c = cur();
	if (c)
		c->cur_color = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void APIENTRY glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	glColor4ub((GLubyte)(r * 255), (GLubyte)(g * 255), (GLubyte)(b * 255), (GLubyte)(a * 255));
}

void APIENTRY glEnd(void)
{
	GlCtx *c = cur();
	SwRast rt;
	SwTex tex;
	SwState st;
	SwTri *batch;
	GlTex *gt;
	int i, ntri, vp_w, vp_h;
	if (!c || !c->in_begin)
		return;
	c->in_begin = 0;
	bind_draw_target(c, &rt, &tex);
	if (!rt.color || c->nimm < 3)
		return;
	vp_w = rt.width;
	vp_h = rt.height;
	for (i = 0; i < c->nimm; i++) {
		SwVert *v = &c->imm[i];
		float x = v->x, y = v->y;
		if (x <= 2.0f && x >= -2.0f && y <= 2.0f && y >= -2.0f) {
			v->x = (x * 0.5f + 0.5f) * (float)vp_w;
			v->y = (0.5f - y * 0.5f) * (float)vp_h;
		} else
			v->y = (float)vp_h - y;
	}
	if (c->begin_mode == GL_QUADS)
		ntri = (c->nimm / 4) * 2;
	else if (c->begin_mode == GL_TRIANGLES)
		ntri = c->nimm / 3;
	else if (c->begin_mode == GL_TRIANGLE_STRIP)
		ntri = c->nimm - 2;
	else
		ntri = 0;
	if (ntri <= 0)
		return;
	batch = (SwTri *)malloc((size_t)ntri * sizeof(SwTri));
	if (!batch)
		return;
	if (c->begin_mode == GL_QUADS) {
		int q, t = 0;
		for (q = 0; q + 3 < c->nimm; q += 4) {
			batch[t].a = c->imm[q];
			batch[t].b = c->imm[q + 1];
			batch[t].c = c->imm[q + 2];
			t++;
			batch[t].a = c->imm[q];
			batch[t].b = c->imm[q + 2];
			batch[t].c = c->imm[q + 3];
			t++;
		}
		ntri = t;
	} else if (c->begin_mode == GL_TRIANGLES) {
		for (i = 0; i < ntri; i++) {
			batch[i].a = c->imm[i * 3];
			batch[i].b = c->imm[i * 3 + 1];
			batch[i].c = c->imm[i * 3 + 2];
		}
	} else {
		for (i = 0; i < ntri; i++) {
			batch[i].a = c->imm[i];
			batch[i].b = c->imm[i + 1];
			batch[i].c = c->imm[i + 2];
		}
	}
	fill_state(c, &st, rt.height);
	gt = bound_tex2d(c);
	if (gt && gt->pixels) {
		tex.pixels = gt->pixels;
		tex.width = gt->w;
		tex.height = gt->h;
	}
	swrast_triangles(&rt, batch, ntri, tex.pixels ? &tex : NULL, &st);
	free(batch);
}

void APIENTRY glDispatchCompute(GLuint x, GLuint y, GLuint z)
{
	gl_ni("glDispatchCompute");
	(void)x;
	(void)y;
	(void)z;
}

void APIENTRY glBindImageTexture(GLuint u, GLuint t, GLint l, GLboolean layered, GLint layer,
				 GLenum acc, GLenum fmt)
{
	(void)u;
	(void)t;
	(void)l;
	(void)layered;
	(void)layer;
	(void)acc;
	(void)fmt;
}

void APIENTRY glMemoryBarrier(GLbitfield b)
{
	(void)b;
}

GLsync APIENTRY glFenceSync(GLenum cond, GLbitfield flags)
{
	(void)cond;
	(void)flags;
	return (GLsync)(uintptr_t)1;
}

GLenum APIENTRY glClientWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
	(void)s;
	(void)f;
	(void)t;
	return GL_ALREADY_SIGNALED;
}

void APIENTRY glDeleteSync(GLsync s)
{
	(void)s;
}

void APIENTRY glWaitSync(GLsync s, GLbitfield f, GLuint64 t)
{
	(void)s;
	(void)f;
	(void)t;
}

void APIENTRY glGenSamplers(GLsizei n, GLuint *ids)
{
	GlCtx *c = cur();
	int i;
	if (!c)
		return;
	for (i = 0; i < n; i++) {
		GlSamp **slot = (GlSamp **)grow((void **)&c->share->samp, &c->share->nsamp,
					       &c->share->csamp, sizeof(GlSamp *));
		GlSamp *s;
		if (!slot)
			return;
		s = (GlSamp *)calloc(1, sizeof(*s));
		s->id = alloc_id(c->share);
		*slot = s;
		c->share->nsamp++;
		if (ids)
			ids[i] = s->id;
	}
}

void APIENTRY glDeleteSamplers(GLsizei n, const GLuint *ids)
{
	(void)n;
	(void)ids;
}

void APIENTRY glBindSampler(GLuint unit, GLuint id)
{
	GlCtx *c = cur();
	if (c && unit < GL_TEXUNITS)
		c->sampler[unit] = id;
}

void APIENTRY glSamplerParameteri(GLuint s, GLenum p, GLint v)
{
	(void)s;
	(void)p;
	(void)v;
}

void APIENTRY glSamplerParameterf(GLuint s, GLenum p, GLfloat v)
{
	glSamplerParameteri(s, p, (GLint)v);
}
