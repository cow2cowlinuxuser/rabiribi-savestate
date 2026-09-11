#ifndef SD_ASSETS_H
#define SD_ASSETS_H

#include <gctypes.h>

/* Volume name when assets come from the inserted GCM FST (not FAT). */
#define SD_ASSETS_DVD_VOL "dvd"

/* Prefer Slot A, then B, then SD2SP2, then the GCM FST.
 * Returns "sda"/"sdb"/"sdc"/"dvd" or NULL. */
const char *sd_assets_mount(void);

/* One EXI slot: 0=sda, 1=sdb, 2=sdc. No DVD. Returns vol or NULL.
 * try_slot also requires TITLE.TPL. mount_fat only needs a FAT volume. */
const char *sd_assets_try_slot(int slot);
const char *sd_assets_mount_fat(int slot);

/* Optional poll during large fread (e.g. rbo_audio_poll so BGM survives STAGE01). */
void sd_assets_set_load_poll(void (*fn)(void));

/* Read entire file into 32-byte-aligned heap buffer. Caller frees with free(). */
void *sd_assets_load(const char *vol, const char *path, u32 *out_size);

/* Write buffer to path (creates parent dirs not required — path must exist or FatFs create). */
int sd_assets_save(const char *vol, const char *path, const void *data, u32 size);

/* Try known STAGE01.TPL locations on the mounted volume. */
void *sd_assets_load_stage01(const char *vol, u32 *out_size);

/* Boot pack: caution / logo / intro / title (TITLE.TPL). */
void *sd_assets_load_title(const char *vol, u32 *out_size);

/* Lobby + create plates (LOBBY.TPL). */
void *sd_assets_load_lobby(const char *vol, u32 *out_size);

/* Stage select plate (STSEL.TPL). */
void *sd_assets_load_stsel(const char *vol, u32 *out_size);

void sd_assets_unmount(const char *vol);

/* Glyph-safe probe string, e.g. "SDA N SDB N SDC N". */
const char *sd_assets_last_diag(void);

#endif
