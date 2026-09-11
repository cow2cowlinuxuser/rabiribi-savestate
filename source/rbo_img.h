#ifndef RBO_IMG_H
#define RBO_IMG_H

/*
 * IMG loader drop-in (PC FUN_00476ed0 / slice FUN_00476e70).
 *
 * Runtime currently uses preconverted TPL from PacNyx (TITLE/LOBBY/STSEL/STAGE01).
 * This module is the future native .Img → tex path if we skip offline convert.
 */

#include <gctypes.h>
#include <ogc/tpl.h>

typedef struct {
	void *pixels; /* owned, 32-aligned; or NULL if TPL-backed */
	u16 width;
	u16 height;
	GXTexObj tex;
	int has_tex;
} RboImg;

void rbo_img_init(void);

/* Stub: always fails — use rbo_assets / sd_assets TPL loaders instead. */
int rbo_img_load_path(const char *vol, const char *path, RboImg *out);
void rbo_img_free(RboImg *img);

#endif /* RBO_IMG_H */
