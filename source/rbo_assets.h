#ifndef RBO_ASSETS_H
#define RBO_ASSETS_H

/*
 * Scene asset swap scaffolding:
 *   load UI TPL → use → free → load next
 * Wraps sd_assets / rbo_fs + rbo_mem flush rules.
 */

#include <gctypes.h>
#include <ogc/tpl.h>

typedef enum {
	RBO_ASSET_PACK_NONE = 0,
	RBO_ASSET_PACK_TITLE,   /* TITLE.TPL — caution/logo/intro/title */
	RBO_ASSET_PACK_LOBBY,   /* LOBBY.TPL */
	RBO_ASSET_PACK_STSEL,   /* STSEL.TPL */
	RBO_ASSET_PACK_STAGE01  /* STAGE01.TPL — exclusive in PLAY */
} RboAssetPack;

typedef struct {
	RboAssetPack pack;
	void *bytes;
	u32 size;
	TPLFile tpl;
	int tpl_open;
} RboAssetSlot;

void rbo_assets_init(void);

/* Unload current UI/stage slot (safe if empty). */
void rbo_assets_unload(RboAssetSlot *slot);

/*
 * Load pack into slot (frees previous). Returns 1 on success.
 * Applies DCFlushRange before TPL_OpenTPLFromMemory.
 */
int rbo_assets_load_pack(RboAssetSlot *slot, const char *vol, RboAssetPack pack);

TPLFile *rbo_assets_tpl(RboAssetSlot *slot);

#endif /* RBO_ASSETS_H */
