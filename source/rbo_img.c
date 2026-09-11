#include <string.h>

#include "rbo_img.h"

void rbo_img_init(void)
{
}

int rbo_img_load_path(const char *vol, const char *path, RboImg *out)
{
	(void)vol;
	(void)path;
	if (out)
		memset(out, 0, sizeof(*out));
	return 0;
}

void rbo_img_free(RboImg *img)
{
	if (!img)
		return;
	/* pixels ownership TBD when loader is real */
	memset(img, 0, sizeof(*img));
}
