#include "rbo_platform.h"

void rbo_platform_init(void)
{
	rbo_mem_init();
	rbo_fs_init();
	rbo_input_init();
	/* Audio (ASND/DSP) waits until main has a valid XFB + VAT. */
	rbo_movie_init();
	rbo_pac_init();
	rbo_img_init();
	rbo_chara_init();
	rbo_assets_init();
	rbo_scene_init();
	/* rbo_gx_init() needs rmode/fifo from main — call later when migrating. */
}
