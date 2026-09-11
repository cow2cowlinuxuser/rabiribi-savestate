#include <stdlib.h>
#include <string.h>

#include "rbo_assets.h"
#include "rbo_fs.h"
#include "rbo_mem.h"

void rbo_assets_init(void)
{
}

void rbo_assets_unload(RboAssetSlot *slot)
{
	if (!slot)
		return;
	if (slot->tpl_open) {
		/* TPL_CloseTPLFile if needed — memory-backed close is a no-op pattern. */
		slot->tpl_open = 0;
	}
	if (slot->bytes) {
		free(slot->bytes);
		slot->bytes = NULL;
	}
	slot->size = 0;
	slot->pack = RBO_ASSET_PACK_NONE;
	memset(&slot->tpl, 0, sizeof(slot->tpl));
}

static void *load_pack_bytes(const char *vol, RboAssetPack pack, u32 *out_size)
{
	switch (pack) {
	case RBO_ASSET_PACK_TITLE:
		return rbo_fs_load_title(vol, out_size);
	case RBO_ASSET_PACK_LOBBY:
		return rbo_fs_load_lobby(vol, out_size);
	case RBO_ASSET_PACK_STSEL:
		return rbo_fs_load_stsel(vol, out_size);
	case RBO_ASSET_PACK_STAGE01:
		return rbo_fs_load_stage01(vol, out_size);
	default:
		if (out_size)
			*out_size = 0;
		return NULL;
	}
}

int rbo_assets_load_pack(RboAssetSlot *slot, const char *vol, RboAssetPack pack)
{
	void *bytes;
	u32 size = 0;

	if (!slot || !vol || pack == RBO_ASSET_PACK_NONE)
		return 0;

	rbo_assets_unload(slot);

	bytes = load_pack_bytes(vol, pack, &size);
	if (!bytes || size == 0)
		return 0;

	rbo_mem_flush(bytes, size);

	slot->bytes = bytes;
	slot->size = size;
	slot->pack = pack;

	/* Same pattern as main.c — open from flushed SD buffer. */
	TPL_OpenTPLFromMemory(&slot->tpl, bytes, size);
	slot->tpl_open = 1;
	return 1;
}

TPLFile *rbo_assets_tpl(RboAssetSlot *slot)
{
	if (!slot || !slot->tpl_open)
		return NULL;
	return &slot->tpl;
}
