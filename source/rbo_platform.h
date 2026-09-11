#ifndef RBO_PLATFORM_H
#define RBO_PLATFORM_H

/*
 * Public GC port surface — include this when migrating off main.c monolith.
 *
 * See docs/PORT_SKELETON.md for PC→GC map and stub vs live status.
 * Soft reset / Swiss: escape.h (not wrapped here).
 */

#include "rbo_scene.h"
#include "rbo_gx.h"
#include "rbo_input.h"
#include "rbo_audio.h"
#include "rbo_movie.h"
#include "rbo_fs.h"
#include "rbo_pac.h"
#include "rbo_img.h"
#include "rbo_chara.h"
#include "rbo_assets.h"
#include "rbo_mem.h"
#include "rbo_dma.h"

/* Optional one-shot init of stub modules (does not mount SD or GX). */
void rbo_platform_init(void);

#endif /* RBO_PLATFORM_H */
