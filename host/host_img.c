#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>

#include "windows.h"
#include "ddraw.h"
#include "host.h"

/* Atlas load: IMG via CreateFile, else a TPL in RBO/ASSETS.
 * TPL is parsed by hand. libogc TPL_CloseTPLFile free()s file offsets. */

static DWORD rd_le32(const unsigned char *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) |
	       ((DWORD)p[3] << 24);
}

static unsigned short rd_le16(const unsigned char *p)
{
	return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned short rgb565_from_1555(unsigned short p)
{
	unsigned r = (p >> 10) & 0x1f;
	unsigned g = (p >> 5) & 0x1f;
	unsigned b = p & 0x1f;

	return (unsigned short)((r << 11) | (g << 6) | b);
}

static unsigned short pix_from_img(const unsigned char *src, DWORD fmt)
{
	if (fmt == 2) {
		unsigned r = src[0];
		unsigned g = src[1];
		unsigned b = src[2];
		unsigned a = src[3];

		return a ? (unsigned short)(((r & 0xf8) << 8) |
					    ((g & 0xfc) << 3) | (b >> 3))
			 : 0;
	}
	if (fmt == 0) {
		unsigned short p = rd_le16(src);

		return (p & 0x8000) ? rgb565_from_1555(p) : 0;
	}
	return rd_le16(src);
}

/* Decode as we read. A 4.2 MB file buffer plus a 2 MB surface would not
 * leave room for SYSTEM.Img after Title.Img. */
void *host_img_load_path(const char *path)
{
	HANDLE h;
	DWORD sz, ver, w, ht, fmt, bpp, npix, done, got;
	unsigned char hdr[20];
	unsigned char chunk[0x8000];
	LPDIRECTDRAWSURFACE7 s;
	unsigned short *px;
	DWORD sw, sh;

	if (!path)
		return NULL;
	h = CreateFileA(path, 0x80000000, 1, NULL, 3, 0x80, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return NULL;
	sz = GetFileSize(h, NULL);
	if (sz < 20 || sz > 8 * 1024 * 1024) {
		CloseHandle(h);
		return NULL;
	}
	got = 0;
	if (!ReadFile(h, hdr, 20, &got, NULL) || got < 20) {
		CloseHandle(h);
		return NULL;
	}
	ver = rd_le32(hdr + 4);
	fmt = rd_le32(hdr + 8);
	w = rd_le32(hdr + 12);
	ht = rd_le32(hdr + 16);
	bpp = (fmt == 2) ? 4 : 2;
	npix = w * ht;
	if (ver < 6 || ver > 7 || w == 0 || ht == 0 || w > 2048 || ht > 2048 ||
	    sz < 20 + npix * bpp) {
		CloseHandle(h);
		return NULL;
	}
	s = host_dd_create_rgb565(w, ht);
	if (!s || !host_surf_rgb565(s, &px, &sw, &sh) || sw != w || sh != ht) {
		if (s)
			host_surf_release(s);
		CloseHandle(h);
		return NULL;
	}
	host_log("IMG %ux%u", (unsigned)w, (unsigned)ht);
	host_halt("IMG");
	done = 0;
	while (done < npix) {
		DWORD want = (0x8000u / bpp);
		DWORD remain = npix - done;
		DWORD bytes;
		DWORD i;

		if (want > remain)
			want = remain;
		bytes = want * bpp;
		got = 0;
		if (!ReadFile(h, chunk, bytes, &got, NULL) || got < bytes)
			break;
		for (i = 0; i < want; i++)
			px[done + i] = pix_from_img(chunk + i * bpp, fmt);
		done += want;
		host_pump_escape();
		if ((done & 0x1ffffu) == 0) {
			host_log("IMG %uk/%uk", (unsigned)(done / 1024u),
				 (unsigned)(npix / 1024u));
			if (!host_flip_live())
				host_overlay_pump();
		}
	}
	CloseHandle(h);
	if (done < npix) {
		host_log("IMG short");
		host_surf_release(s);
		return NULL;
	}
	return s;
}

#define TPL_MAGIC 0x0020af30u

static DWORD tpl_tex_bytes(u32 fmt, DWORD w, DWORD h)
{
	DWORD tiles = ((w + 3) >> 2) * ((h + 3) >> 2);

	if (fmt == GX_TF_CMPR)
		return ((w + 7) >> 3) * ((h + 7) >> 3) * 32;
	if (fmt == GX_TF_RGBA8)
		return tiles * 64;
	if (fmt == GX_TF_RGB565 || fmt == GX_TF_RGB5A3)
		return tiles * 32;
	return 0;
}

static void untile_rgb565(unsigned short *dst, const unsigned char *src,
			  DWORD w, DWORD h)
{
	DWORD x, y, tx, ty;
	const unsigned short *p = (const unsigned short *)src;

	for (y = 0; y < h; y += 4) {
		for (x = 0; x < w; x += 4) {
			for (ty = 0; ty < 4; ty++) {
				for (tx = 0; tx < 4; tx++) {
					DWORD sx = x + tx;
					DWORD sy = y + ty;

					if (sx < w && sy < h)
						dst[sy * w + sx] = *p;
					p++;
				}
			}
		}
	}
}

static unsigned short rgb5a3_to_565(unsigned short p)
{
	unsigned r, g, b, a;

	if (p & 0x8000) {
		r = (p >> 10) & 0x1f;
		g = (p >> 5) & 0x1f;
		b = p & 0x1f;
		return (unsigned short)((r << 11) | (g << 6) | b);
	}
	a = (p >> 12) & 7;
	if (!a)
		return 0;
	r = (p >> 8) & 0xf;
	g = (p >> 4) & 0xf;
	b = p & 0xf;
	return (unsigned short)((r << 12) | (g << 7) | (b << 1));
}

static void untile_rgb5a3_to_565(unsigned short *dst, const unsigned char *src,
				 DWORD w, DWORD h)
{
	DWORD x, y, tx, ty;

	for (y = 0; y < h; y += 4) {
		for (x = 0; x < w; x += 4) {
			for (ty = 0; ty < 4; ty++) {
				for (tx = 0; tx < 4; tx++) {
					DWORD sx = x + tx;
					DWORD sy = y + ty;
					unsigned short p = (unsigned short)((src[0] << 8) | src[1]);

					src += 2;
					if (sx < w && sy < h)
						dst[sy * w + sx] = rgb5a3_to_565(p);
				}
			}
		}
	}
}

static void untile_rgba8_to_565(unsigned short *dst, const unsigned char *src,
				DWORD w, DWORD h)
{
	DWORD x, y, tx, ty, i;
	const unsigned char *ar;
	const unsigned char *gb;

	for (y = 0; y < h; y += 4) {
		for (x = 0; x < w; x += 4) {
			ar = src;
			gb = src + 32;
			src += 64;
			i = 0;
			for (ty = 0; ty < 4; ty++) {
				for (tx = 0; tx < 4; tx++, i++) {
					DWORD sx = x + tx;
					DWORD sy = y + ty;
					unsigned a, r, g, b;

					if (sx >= w || sy >= h)
						continue;
					a = ar[i * 2];
					r = ar[i * 2 + 1];
					g = gb[i * 2];
					b = gb[i * 2 + 1];
					dst[sy * w + sx] =
						a ? (unsigned short)(((r & 0xf8) << 8) |
								     ((g & 0xfc) << 3) |
								     (b >> 3))
						  : 0;
				}
			}
		}
	}
}

static unsigned short lerp565(unsigned short a, unsigned short b, int wa, int wb,
			      int d)
{
	int r, g, bl;

	r = (((a >> 11) & 31) * wa + ((b >> 11) & 31) * wb) / d;
	g = (((a >> 5) & 63) * wa + ((b >> 5) & 63) * wb) / d;
	bl = ((a & 31) * wa + (b & 31) * wb) / d;
	return (unsigned short)((r << 11) | (g << 5) | bl);
}

static void dxt1_block(unsigned short *dst, DWORD w, DWORD h, DWORD bx, DWORD by,
		       const unsigned char *src)
{
	unsigned short pal[4];
	unsigned short c0, c1;
	DWORD px, py;

	c0 = (unsigned short)((src[0] << 8) | src[1]);
	c1 = (unsigned short)((src[2] << 8) | src[3]);
	pal[0] = c0;
	pal[1] = c1;
	if (c0 > c1) {
		pal[2] = lerp565(c0, c1, 2, 1, 3);
		pal[3] = lerp565(c0, c1, 1, 2, 3);
	} else {
		pal[2] = lerp565(c0, c1, 1, 1, 2);
		pal[3] = 0;
	}
	for (py = 0; py < 4; py++) {
		unsigned char row = src[4 + py];

		for (px = 0; px < 4; px++) {
			DWORD x = bx + px;
			DWORD y = by + py;

			if (x < w && y < h)
				dst[y * w + x] = pal[(row >> (px * 2)) & 3];
		}
	}
}

/* GX_TF_CMPR: 8x8 tiles of four DXT1 4x4 blocks, (0,0) (4,0) (0,4) (4,4). */
static void untile_cmpr_to_565(unsigned short *dst, const unsigned char *src,
			       DWORD w, DWORD h)
{
	DWORD x, y;

	for (y = 0; y < h; y += 8) {
		for (x = 0; x < w; x += 8) {
			dxt1_block(dst, w, h, x, y, src);
			dxt1_block(dst, w, h, x + 4, y, src + 8);
			dxt1_block(dst, w, h, x, y + 4, src + 16);
			dxt1_block(dst, w, h, x + 4, y + 4, src + 24);
			src += 32;
		}
	}
}

static int rd_be32_fp(FILE *fp, u32 *out)
{
	unsigned char b[4];

	if (fread(b, 1, 4, fp) != 4)
		return 0;
	*out = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
	return 1;
}

static int rd_be16_fp(FILE *fp, u16 *out)
{
	unsigned char b[2];

	if (fread(b, 1, 2, fp) != 2)
		return 0;
	*out = (u16)(((unsigned)b[0] << 8) | b[1]);
	return 1;
}

void *host_tpl_load(const char *tpl_file, int tex_id)
{
	char path[320];
	FILE *fp;
	u32 magic, ntex, img_off, pal_off, fmt, data_off, nbytes;
	u16 tw, th;
	unsigned char *tiled;
	unsigned short *px;
	DWORD w, h;
	LPDIRECTDRAWSURFACE7 s;

	if (!tpl_file || tex_id < 0)
		return NULL;
	snprintf(path, sizeof(path), "%s:/RBO/ASSETS/%s", host_sd_vol(), tpl_file);
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (!rd_be32_fp(fp, &magic) || magic != TPL_MAGIC ||
	    !rd_be32_fp(fp, &ntex) || (u32)tex_id >= ntex) {
		fclose(fp);
		return NULL;
	}
	if (fseek(fp, 0x0c + tex_id * 8, SEEK_SET) != 0 ||
	    !rd_be32_fp(fp, &img_off) || !rd_be32_fp(fp, &pal_off)) {
		fclose(fp);
		return NULL;
	}
	(void)pal_off;
	if (fseek(fp, (long)img_off, SEEK_SET) != 0 ||
	    !rd_be16_fp(fp, &th) || !rd_be16_fp(fp, &tw) ||
	    !rd_be32_fp(fp, &fmt) || !rd_be32_fp(fp, &data_off)) {
		fclose(fp);
		return NULL;
	}
	nbytes = tpl_tex_bytes(fmt, tw, th);
	if (!nbytes || tw > 1024 || th > 1024) {
		fclose(fp);
		return NULL;
	}
	tiled = (unsigned char *)memalign(32, nbytes);
	if (!tiled) {
		fclose(fp);
		return NULL;
	}
	if (fseek(fp, (long)data_off, SEEK_SET) != 0) {
		free(tiled);
		fclose(fp);
		return NULL;
	}
	{
		DWORD off = 0;

		while (off < nbytes) {
			DWORD chunk = nbytes - off;
			size_t got;

			if (chunk > 0x8000)
				chunk = 0x8000;
			got = fread(tiled + off, 1, chunk, fp);
			if (got == 0)
				break;
			off += (DWORD)got;
			host_pump_escape();
		}
		if (off != nbytes) {
			free(tiled);
			fclose(fp);
			return NULL;
		}
	}
	fclose(fp);

	s = host_dd_create_rgb565(tw, th);
	if (!s || !host_surf_rgb565(s, &px, &w, &h)) {
		free(tiled);
		return NULL;
	}
	if (fmt == GX_TF_CMPR)
		untile_cmpr_to_565(px, tiled, w, h);
	else if (fmt == GX_TF_RGBA8)
		untile_rgba8_to_565(px, tiled, w, h);
	else if (fmt == GX_TF_RGB5A3)
		untile_rgb5a3_to_565(px, tiled, w, h);
	else
		untile_rgb565(px, tiled, w, h);
	free(tiled);
	host_log("TPL %s #%d %ux%u", tpl_file, tex_id, (unsigned)w, (unsigned)h);
	return s;
}

void *host_load_atlas(const char *img_pc, const char *tpl_file, int tex_id)
{
	void *s;

	s = host_img_load_path(img_pc);
	if (s) {
		host_log("IMG ok");
		return s;
	}
	s = host_tpl_load(tpl_file, tex_id);
	if (s)
		return s;
	host_log("ATLAS miss");
	return NULL;
}

void *host_load_title_tex(void)
{
	return host_load_atlas(".\\Data\\CG\\Title\\Title.Img", "TITLE.TPL", 3);
}

void *host_load_system_tex(void)
{
	/* GameMain HUD atlas. STAGE01.TPL tex 21 is the wrong sheet. */
	return host_img_load_path(".\\Data\\CG\\GameMain\\SYSTEM.Img");
}

void *host_load_lobby_tex(void)
{
	return host_load_atlas(".\\Data\\CG\\CharaSel\\Lobby.Img", NULL, -1);
}

void *host_load_stsel_tex(void)
{
	return host_load_atlas(".\\Data\\CG\\StageSelect\\st_select.img",
			       "STSEL.TPL", 0);
}

void *host_load_stsel_chip(void)
{
	return host_load_atlas(".\\Data\\CG\\StageSelect\\st_sel_map.img",
			       "STSEL.TPL", 1);
}
