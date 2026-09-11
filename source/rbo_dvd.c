#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/dvd.h>
#include <ogc/mutex.h>

#include "rbo_dvd.h"

#define FST_MAX       (1024 * 1024)
#define BOUNCE_SIZE   32768
#define GC_MAGIC      0xC2339F3Du
#define BB2_OFF       0x420

static dvdcmdblk s_blk ATTRIBUTE_ALIGN(32);
static u8 s_bounce[BOUNCE_SIZE] ATTRIBUTE_ALIGN(32);
static u8 s_boot32[32] ATTRIBUTE_ALIGN(32);
static u8 s_bb2[32] ATTRIBUTE_ALIGN(32);
static mutex_t s_mtx;
static int s_mtx_ok;
static int s_ready;

static u8 *s_fst;
static u32 s_fst_len;
static u32 s_nents;
static const char *s_strings;

static int s_step;
static s32 s_last_n;
static s32 s_mount_rc;
static u32 s_magic;
static u32 s_fst_off_dbg;
static u32 s_fst_len_dbg;
static int s_title_ok;
static char s_diag[48];

enum {
	DVD_STEP_OK = 0,
	DVD_STEP_MOUNT = 1,
	DVD_STEP_READ0 = 2,
	DVD_STEP_MAGIC = 3,
	DVD_STEP_BB2 = 4,
	DVD_STEP_FST = 5,
	DVD_STEP_PARSE = 6,
	DVD_STEP_TITLE = 7
};

static u32 be32(const void *p)
{
	const u8 *b = (const u8 *)p;

	return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static u32 align32(u32 n)
{
	return (n + 31u) & ~31u;
}

static int icmp(const char *a, const char *b)
{
	for (;;) {
		unsigned ca = (unsigned char)*a++;
		unsigned cb = (unsigned char)*b++;

		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca != cb)
			return (int)ca - (int)cb;
		if (!ca)
			return 0;
	}
}

static int fst_is_dir(u32 i)
{
	return (be32(s_fst + i * 12) >> 24) == 1;
}

static u32 fst_nameoff(u32 i)
{
	return be32(s_fst + i * 12) & 0xFFFFFFu;
}

static u32 fst_w1(u32 i)
{
	return be32(s_fst + i * 12 + 4);
}

static u32 fst_w2(u32 i)
{
	return be32(s_fst + i * 12 + 8);
}

static const char *fst_name(u32 i)
{
	u32 off = fst_nameoff(i);

	if (!s_strings || (u8 *)s_strings + off >= s_fst + s_fst_len)
		return "";
	return s_strings + off;
}

static u32 fst_next(u32 i)
{
	if (fst_is_dir(i))
		return fst_w2(i);
	return i + 1;
}

static int lookup_one(const char *path, u32 *off, u32 *sz)
{
	u32 dir = 0;
	const char *p = path;
	char comp[128];

	if (!s_ready || !path || !off || !sz)
		return -1;

	while (*p == '/' || *p == '\\')
		p++;
	if (!*p)
		return -1;

	while (*p) {
		int n = 0;
		u32 i, end, found;

		while (*p && *p != '/' && *p != '\\' && n < (int)sizeof(comp) - 1)
			comp[n++] = *p++;
		comp[n] = 0;
		while (*p == '/' || *p == '\\')
			p++;
		if (!n)
			continue;
		if (!fst_is_dir(dir))
			return -1;

		i = dir + 1;
		end = fst_next(dir);
		found = (u32)-1;
		while (i < end && i < s_nents) {
			if (icmp(fst_name(i), comp) == 0) {
				found = i;
				break;
			}
			i = fst_next(i);
		}
		if (found == (u32)-1)
			return -1;
		dir = found;
	}

	if (fst_is_dir(dir))
		return -1;
	*off = fst_w1(dir);
	*sz = fst_w2(dir);
	return 0;
}

static int dvd_read_ok(s32 n, u32 len)
{
	return n >= (s32)len;
}

static void dvd_diag_build(void)
{
	snprintf(s_diag, sizeof(s_diag), "DVD STEP %d MNT %d RD %d T %d",
		 s_step, (int)s_mount_rc, (int)s_last_n, s_title_ok);
}

int rbo_dvd_last_step(void)
{
	return s_step;
}

s32 rbo_dvd_last_n(void)
{
	return s_last_n;
}

s32 rbo_dvd_last_mount(void)
{
	return s_mount_rc;
}

u32 rbo_dvd_last_magic(void)
{
	return s_magic;
}

u32 rbo_dvd_last_fst_off(void)
{
	return s_fst_off_dbg;
}

u32 rbo_dvd_last_fst_len(void)
{
	return s_fst_len_dbg;
}

const char *rbo_dvd_last_diag(void)
{
	return s_diag;
}

int rbo_dvd_ready(void)
{
	return s_ready;
}

int rbo_dvd_lookup(const char *path, u32 *disc_off, u32 *size)
{
	if (lookup_one(path, disc_off, size) == 0)
		return 0;
	if (path && (path[0] == '/' || path[0] == '\\') &&
	    lookup_one(path + 1, disc_off, size) == 0)
		return 0;
	if (path && ((path[0] == 'R' || path[0] == 'r') &&
		     (path[1] == 'B' || path[1] == 'b') &&
		     (path[2] == 'O' || path[2] == 'o') &&
		     path[3] == '/') &&
	    lookup_one(path + 4, disc_off, size) == 0)
		return 0;
	return -1;
}

int rbo_dvd_has_boot_pack(void)
{
	static const char *const paths[] = {
		"RBO/ASSETS/TITLE.TPL",
		"ASSETS/TITLE.TPL",
		"TITLE.TPL",
		NULL
	};
	u32 off, sz;
	int i;

	s_title_ok = 0;
	if (!s_ready)
		return 0;
	for (i = 0; paths[i]; i++) {
		if (rbo_dvd_lookup(paths[i], &off, &sz) == 0 && sz > 64) {
			s_title_ok = 1;
			return 1;
		}
	}
	s_step = DVD_STEP_TITLE;
	dvd_diag_build();
	return 0;
}

int rbo_dvd_read(u32 disc_off, void *buf, u32 len)
{
	u8 *out = (u8 *)buf;
	u32 left = len;

	if (!buf)
		return -1;
	if (len == 0)
		return 0;
	if (!s_mtx_ok)
		return -1;

	LWP_MutexLock(s_mtx);
	while (left) {
		u32 aoff = disc_off & ~31u;
		u32 skip = disc_off - aoff;
		u32 want = left + skip;
		u32 rlen = align32(want);
		s32 n;
		u32 take;

		if (((u32)out & 31u) == 0 && skip == 0 && rlen == left) {
			DCInvalidateRange(out, rlen);
			n = DVD_ReadPrio(&s_blk, out, rlen, (s64)aoff, 2);
			s_last_n = n;
			if (!dvd_read_ok(n, rlen)) {
				LWP_MutexUnlock(s_mtx);
				return -1;
			}
			break;
		}

		if (rlen > BOUNCE_SIZE)
			rlen = BOUNCE_SIZE;
		DCInvalidateRange(s_bounce, rlen);
		n = DVD_ReadPrio(&s_blk, s_bounce, rlen, (s64)aoff, 2);
		s_last_n = n;
		if (!dvd_read_ok(n, rlen)) {
			LWP_MutexUnlock(s_mtx);
			return -1;
		}
		take = rlen - skip;
		if (take > left)
			take = left;
		memcpy(out, s_bounce + skip, take);
		disc_off += take;
		out += take;
		left -= take;
	}
	LWP_MutexUnlock(s_mtx);
	return 0;
}

int rbo_dvd_mount(void)
{
	u32 fst_off, fst_len, nread;
	s32 n;

	if (s_ready)
		return 0;

	s_step = DVD_STEP_MOUNT;
	s_last_n = 0;
	s_mount_rc = 0;
	s_magic = 0;
	s_fst_off_dbg = 0;
	s_fst_len_dbg = 0;
	s_title_ok = 0;
	dvd_diag_build();

	DVD_Init();
	if (!s_mtx_ok) {
		if (LWP_MutexInit(&s_mtx, 0) != 0)
			return -1;
		s_mtx_ok = 1;
	}

	/* libogc's GC FAT-on-DVD path does this after Init. ReadPrio
	 * without a mount returns -1 on Dolphin after IPL. */
	s_mount_rc = DVD_Mount();
	dvd_diag_build();

	DCInvalidateRange(s_boot32, sizeof(s_boot32));
	n = DVD_ReadPrio(&s_blk, s_boot32, sizeof(s_boot32), 0, 2);
	s_last_n = n;
	if (!dvd_read_ok(n, sizeof(s_boot32))) {
		s_step = DVD_STEP_READ0;
		dvd_diag_build();
		return -1;
	}
	s_magic = be32(s_boot32 + 0x1C);
	if (s_magic != GC_MAGIC) {
		s_step = DVD_STEP_MAGIC;
		dvd_diag_build();
		return -1;
	}

	DCInvalidateRange(s_bb2, sizeof(s_bb2));
	n = DVD_ReadPrio(&s_blk, s_bb2, sizeof(s_bb2), BB2_OFF, 2);
	s_last_n = n;
	if (!dvd_read_ok(n, sizeof(s_bb2))) {
		s_step = DVD_STEP_BB2;
		dvd_diag_build();
		return -1;
	}
	fst_off = be32(s_bb2 + 4);
	fst_len = be32(s_bb2 + 8);
	s_fst_off_dbg = fst_off;
	s_fst_len_dbg = fst_len;
	if (fst_off < 0x2440u || fst_len < 12u || fst_len > FST_MAX) {
		s_step = DVD_STEP_FST;
		dvd_diag_build();
		return -1;
	}

	nread = align32(fst_len);
	s_fst = memalign(32, nread);
	if (!s_fst) {
		s_step = DVD_STEP_FST;
		dvd_diag_build();
		return -1;
	}
	DCInvalidateRange(s_fst, nread);
	n = DVD_ReadPrio(&s_blk, s_fst, nread, (s64)fst_off, 2);
	s_last_n = n;
	if (!dvd_read_ok(n, nread)) {
		free(s_fst);
		s_fst = NULL;
		s_step = DVD_STEP_FST;
		dvd_diag_build();
		return -1;
	}
	s_fst_len = fst_len;
	s_nents = be32(s_fst + 8);
	if (s_nents < 1 || s_nents > fst_len / 12u) {
		free(s_fst);
		s_fst = NULL;
		s_step = DVD_STEP_PARSE;
		dvd_diag_build();
		return -1;
	}
	s_strings = (const char *)(s_fst + s_nents * 12);
	if ((const u8 *)s_strings >= s_fst + fst_len) {
		free(s_fst);
		s_fst = NULL;
		s_strings = NULL;
		s_step = DVD_STEP_PARSE;
		dvd_diag_build();
		return -1;
	}
	s_ready = 1;
	s_step = DVD_STEP_OK;
	dvd_diag_build();
	return 0;
}

void rbo_dvd_unmount(void)
{
	s_ready = 0;
	s_nents = 0;
	s_fst_len = 0;
	s_strings = NULL;
	free(s_fst);
	s_fst = NULL;
}
