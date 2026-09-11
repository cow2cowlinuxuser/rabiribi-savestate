#ifndef RBO_DVD_H
#define RBO_DVD_H

#include <gctypes.h>

/* Mount the inserted GCM and parse its FST. 0 = ready. */
int rbo_dvd_mount(void);
void rbo_dvd_unmount(void);

int rbo_dvd_ready(void);
int rbo_dvd_has_boot_pack(void);

/* Last mount attempt (glyph-safe). */
const char *rbo_dvd_last_diag(void);
s32 rbo_dvd_last_n(void);
u32 rbo_dvd_last_magic(void);
u32 rbo_dvd_last_fst_off(void);
u32 rbo_dvd_last_fst_len(void);
int rbo_dvd_last_step(void);
s32 rbo_dvd_last_mount(void);

/* Case-insensitive FST lookup. Also tries without a leading RBO/. */
int rbo_dvd_lookup(const char *path, u32 *disc_off, u32 *size);

/* Read `len` bytes at a disc offset. Handles 32-byte DVD alignment. 0 = ok. */
int rbo_dvd_read(u32 disc_off, void *buf, u32 len);

#endif
