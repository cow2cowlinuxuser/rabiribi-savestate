#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <gccore.h>
#include <ogc/cache.h>
#include <fat.h>
#include <sdcard/gcsd.h>

#include "sd_assets.h"
#include "rbo_dvd.h"

/* Prefer Slot A, then B, then SD2SP2. Dolphin's SD image is Slot A (sda:);
 * hardware SD2SP2 still mounts as sdc: after empty slots fail. A volume
 * only wins if TITLE.TPL is actually there (empty FAT images do not). */
static const DISC_INTERFACE *const s_ifaces[3] = {
	&__io_gcsda, &__io_gcsdb, &__io_gcsd2
};
static const char *const s_vols[3] = { "sda", "sdb", "sdc" };

static char s_diag[40] = "SDA - SDB - SDC -";

/* Keep BGM fed while giant TPL fread blocks the main loop. */
static void (*s_load_poll)(void);
#define SD_LOAD_CHUNK  (64 * 1024)

void sd_assets_set_load_poll(void (*fn)(void))
{
	s_load_poll = fn;
}

static int vol_has_boot_pack(const char *vol)
{
	static const char *const paths[] = {
		"RBO/ASSETS/TITLE.TPL",
		"ASSETS/TITLE.TPL",
		"TITLE.TPL",
		NULL
	};
	char full[160];
	FILE *fp;
	int i;

	for (i = 0; paths[i]; i++) {
		snprintf(full, sizeof(full), "%s:/%s", vol, paths[i]);
		fp = fopen(full, "rb");
		if (fp) {
			fclose(fp);
			return 1;
		}
	}
	return 0;
}

const char *sd_assets_try_slot(int slot)
{
	const DISC_INTERFACE *iface;
	const char *vol;
	char *diag;

	if (slot < 0 || slot > 2)
		return NULL;
	iface = s_ifaces[slot];
	vol = s_vols[slot];
	diag = s_diag;
	if (!iface)
		return NULL;
	if (iface->startup && !iface->startup())
		return NULL;
	if (iface->isInserted && !iface->isInserted()) {
		if (iface->shutdown)
			iface->shutdown();
		return NULL;
	}
	if (!fatMountSimple(vol, iface)) {
		if (iface->shutdown)
			iface->shutdown();
		return NULL;
	}
	if (!vol_has_boot_pack(vol)) {
		fatUnmount(vol);
		if (iface->shutdown)
			iface->shutdown();
		return NULL;
	}
	(void)diag;
	return vol;
}

const char *sd_assets_mount_fat(int slot)
{
	const DISC_INTERFACE *iface;
	const char *vol;

	if (slot < 0 || slot > 2)
		return NULL;
	iface = s_ifaces[slot];
	vol = s_vols[slot];
	if (!iface)
		return NULL;
	if (iface->startup && !iface->startup())
		return NULL;
	if (iface->isInserted && !iface->isInserted()) {
		if (iface->shutdown)
			iface->shutdown();
		return NULL;
	}
	if (!fatMountSimple(vol, iface)) {
		if (iface->shutdown)
			iface->shutdown();
		return NULL;
	}
	return vol;
}

const char *sd_assets_mount(void)
{
	int i;
	char code[3] = { '-', '-', '-' };

	for (i = 0; i < 3; i++) {
		const DISC_INTERFACE *iface = s_ifaces[i];
		const char *vol = s_vols[i];

		if (!iface) {
			code[i] = '-';
			continue;
		}
		if (iface->startup && !iface->startup()) {
			code[i] = 'S';
			continue;
		}
		if (iface->isInserted && !iface->isInserted()) {
			if (iface->shutdown)
				iface->shutdown();
			code[i] = 'N';
			continue;
		}
		if (!fatMountSimple(vol, iface)) {
			if (iface->shutdown)
				iface->shutdown();
			code[i] = 'F';
			continue;
		}
		if (!vol_has_boot_pack(vol)) {
			fatUnmount(vol);
			if (iface->shutdown)
				iface->shutdown();
			code[i] = 'P';
			continue;
		}
		code[i] = 'K';
		snprintf(s_diag, sizeof(s_diag), "SDA %c SDB %c SDC %c",
			 code[0], code[1], code[2]);
		return vol;
	}
	snprintf(s_diag, sizeof(s_diag), "SDA %c SDB %c SDC %c",
		 code[0], code[1], code[2]);
	if (rbo_dvd_mount() == 0 && rbo_dvd_has_boot_pack())
		return SD_ASSETS_DVD_VOL;
	return NULL;
}

void sd_assets_unmount(const char *vol)
{
	if (!vol || !vol[0])
		return;
	if (strcmp(vol, SD_ASSETS_DVD_VOL) == 0)
		rbo_dvd_unmount();
	else
		fatUnmount(vol);
}

const char *sd_assets_last_diag(void)
{
	return s_diag;
}

static void *dvd_load(const char *path, u32 *out_size)
{
	u32 off, size, alloc, got;
	void *buf;

	if (rbo_dvd_lookup(path, &off, &size) != 0 || size == 0)
		return NULL;
	alloc = (size + 31u) & ~31u;
	buf = memalign(32, alloc);
	if (!buf)
		return NULL;

	got = 0;
	while (got < size) {
		u32 want = size - got;

		if (want > SD_LOAD_CHUNK)
			want = SD_LOAD_CHUNK;
		if (rbo_dvd_read(off + got, (u8 *)buf + got, want) != 0) {
			free(buf);
			return NULL;
		}
		got += want;
		if (s_load_poll)
			s_load_poll();
	}
	if (alloc > size)
		memset((u8 *)buf + size, 0, alloc - size);
	DCFlushRange(buf, alloc);
	if (out_size)
		*out_size = size;
	return buf;
}

void *sd_assets_load(const char *vol, const char *path, u32 *out_size)
{
	char full[160];
	FILE *fp;
	long len;
	void *buf;
	size_t got;

	if (out_size)
		*out_size = 0;
	if (!vol || !path)
		return NULL;

	if (strcmp(vol, SD_ASSETS_DVD_VOL) == 0)
		return dvd_load(path, out_size);

	snprintf(full, sizeof(full), "%s:/%s", vol, path);
	fp = fopen(full, "rb");
	if (!fp)
		return NULL;

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	len = ftell(fp);
	if (len <= 0) {
		fclose(fp);
		return NULL;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return NULL;
	}

	buf = memalign(32, (size_t)len);
	if (!buf) {
		fclose(fp);
		return NULL;
	}

	got = 0;
	while (got < (size_t)len) {
		size_t want = (size_t)len - got;
		size_t n;

		if (want > SD_LOAD_CHUNK)
			want = SD_LOAD_CHUNK;
		n = fread((u8 *)buf + got, 1, want, fp);
		if (n == 0)
			break;
		got += n;
		if (s_load_poll)
			s_load_poll();
	}
	fclose(fp);
	if (got != (size_t)len) {
		free(buf);
		return NULL;
	}

	DCFlushRange(buf, (u32)len);
	if (out_size)
		*out_size = (u32)len;
	return buf;
}

void *sd_assets_load_stage01(const char *vol, u32 *out_size)
{
	static const char *paths[] = {
		"RBO/ASSETS/STAGE01.TPL",
		"rbo/assets/stage01.tpl",
		"ASSETS/STAGE01.TPL",
		"STAGE01.TPL",
		NULL
	};
	int i;
	void *buf;

	for (i = 0; paths[i]; i++) {
		buf = sd_assets_load(vol, paths[i], out_size);
		if (buf)
			return buf;
	}
	return NULL;
}

int sd_assets_save(const char *vol, const char *path, const void *data, u32 size)
{
	char full[160];
	FILE *fp;
	size_t wrote;

	if (!vol || !path || !data || !size)
		return -1;
	if (strcmp(vol, SD_ASSETS_DVD_VOL) == 0)
		return -1;

	snprintf(full, sizeof(full), "%s:/%s", vol, path);
	fp = fopen(full, "wb");
	if (!fp)
		return -2;

	wrote = fwrite(data, 1, (size_t)size, fp);
	fclose(fp);
	return (wrote == (size_t)size) ? 0 : -3;
}

static void *load_first(const char *vol, const char **paths, u32 *out_size)
{
	int i;
	void *buf;

	for (i = 0; paths[i]; i++) {
		buf = sd_assets_load(vol, paths[i], out_size);
		if (buf)
			return buf;
	}
	return NULL;
}

void *sd_assets_load_title(const char *vol, u32 *out_size)
{
	static const char *paths[] = {
		"RBO/ASSETS/TITLE.TPL",
		"rbo/assets/title.tpl",
		"ASSETS/TITLE.TPL",
		"TITLE.TPL",
		NULL
	};
	return load_first(vol, paths, out_size);
}

void *sd_assets_load_lobby(const char *vol, u32 *out_size)
{
	static const char *paths[] = {
		"RBO/ASSETS/LOBBY.TPL",
		"rbo/assets/lobby.tpl",
		"ASSETS/LOBBY.TPL",
		"LOBBY.TPL",
		NULL
	};
	return load_first(vol, paths, out_size);
}

void *sd_assets_load_stsel(const char *vol, u32 *out_size)
{
	static const char *paths[] = {
		"RBO/ASSETS/STSEL.TPL",
		"rbo/assets/stsel.tpl",
		"ASSETS/STSEL.TPL",
		"STSEL.TPL",
		NULL
	};
	return load_first(vol, paths, out_size);
}
