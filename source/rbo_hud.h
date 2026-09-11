#ifndef RBO_HUD_H
#define RBO_HUD_H

/*
 * PLAY battle HUD — SYSTEM.Img slices + glyphs.
 * PC: FUN_00470730 (party slots), FUN_0046f570 (follow bars),
 *     FUN_0046bd80 (radar), FUN_0046d5e0 (top bar).
 */

#include <gctypes.h>
#include <ogc/tpl.h>

#define RBO_HUD_BLIP_MAX 24
#define RBO_HUD_BLIP_PLAYER 0
#define RBO_HUD_BLIP_ENEMY  1
#define RBO_HUD_BLIP_NPC    2

typedef struct {
	f32 world_x;
	u8 kind;
} RboHudBlip;

typedef struct {
	const char *name;
	const char *class_name;
	int level;
	int hp, hp_max, sp, sp_max;
	int occupied; /* 0 empty */
} RboHudSlot;

typedef struct {
	RboHudSlot slot[3];
	u32 play_timer; /* frames at 60Hz */
	int area;       /* 1-based STAGE AREA */
	int stage_clear;
	f32 novice_sx, novice_sy;
	f32 cam_x, lock_lo, lock_hi;
	u32 mem_pct, mem_free_k;
	const RboHudBlip *blips;
	int n_blips;
	const char *note; /* optional FULL-path debug; NULL to skip */
	const char *note2;
} RboHudView;

typedef void (*RboHudTextBegin)(void);
typedef void (*RboHudTextFn)(f32 x, f32 y, const char *s, f32 px);
typedef void (*RboHudTextEnd)(void);

void rbo_hud_draw(const RboHudView *v, GXTexObj *system,
		  RboHudTextBegin text_begin, RboHudTextFn text_fn,
		  RboHudTextEnd text_end);

#endif /* RBO_HUD_H */
