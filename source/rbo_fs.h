#ifndef RBO_FS_H
#define RBO_FS_H

/*
 * Thin file I/O funnel over sd_assets (PC CreateFileA / ReadFile).
 * Registry / PathCombine install discovery is intentionally omitted —
 * all content lives on SD under sdc:/RBO/...
 */

#include <gctypes.h>
#include "sd_assets.h"

/* Aliases so future code can say rbo_fs_* without depending on sd_ name. */
#define rbo_fs_mount       sd_assets_mount
#define rbo_fs_unmount     sd_assets_unmount
#define rbo_fs_load        sd_assets_load
#define rbo_fs_save        sd_assets_save
#define rbo_fs_load_title  sd_assets_load_title
#define rbo_fs_load_lobby  sd_assets_load_lobby
#define rbo_fs_load_stsel  sd_assets_load_stsel
#define rbo_fs_load_stage01 sd_assets_load_stage01

/* Optional init hook ( presently a no-op ). */
void rbo_fs_init(void);

#endif /* RBO_FS_H */
