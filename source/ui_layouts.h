/* UI atlas slice layouts — from EXE .data + ui_sprite_dests.csv
 * Sheet size for Title/Lobby/StSel/System atlases: 1024x1024.
 */
#ifndef UI_LAYOUTS_H
#define UI_LAYOUTS_H

#include <gctypes.h>

#define UI_ATLAS_W 1024
#define UI_ATLAS_H 1024

typedef struct {
	f32 sx, sy, sw, sh;
	f32 dx, dy, dw, dh;
} UiBlit;

/* Title.Img — FUN_00477ab0 (ornaments skipped: already in logo strip) */
static const UiBlit k_title_logo =
	{ 0, 0, 544, 176, 48, 24, 544, 176 };
static const UiBlit k_title_banner =
	{ 672, 0, 352, 64, 144, 416, 352, 64 };

/* Boot_logo.Img — three splash logos (FUN_00404400), bank 0x0048A000 */
static const UiBlit k_boot_logos[3] = {
	{ 0, 0, 512, 168, 64, 156, 512, 168 },   /* French-Bread */
	{ 0, 344, 512, 168, 64, 156, 512, 168 }, /* Shunpu-tei */
	{ 0, 168, 512, 176, 64, 152, 512, 176 }, /* Gravity */
};
#define BOOT_LOGO_SHEET 512
#define BOOT_LOGO_COUNT 3


/* Menu sprite bank 0x004A09F0 — index == PC menu id (FUN_004774c0).
 * Id 0/8/9 empty in JP sheet (Eng text); 7=Ranking, 2=Exit. */
static const UiBlit k_title_menu_ranking =
	{ 0, 368, 208, 32, 216, 272, 208, 32 };
static const UiBlit k_title_menu_exit =
	{ 0, 240, 208, 32, 216, 304, 208, 32 };
/* Start row Y=240 — text fallback (no atlas art). */
#define TITLE_MENU_START_X 216.0f
#define TITLE_MENU_START_Y 240.0f
#define TITLE_MENU_ROW_H   32.0f

/* StSel — st_select.img + st_sel_map.img */
static const UiBlit k_stsel_banner =
	{ 0, 0, 304, 64, -4, -4, 304, 64 };
static const UiBlit k_stsel_hbar =
	{ 304, 0, 320, 12, 0, 9, 640, 12 }; /* stretch */
static const UiBlit k_stsel_hbar_bot =
	{ 304, 0, 320, 12, 0, 454, 640, 12 };
/* Map/chip: ST_SEL_CHIP01 512x512; stage1 icon dest ~263,120 */
static const UiBlit k_stsel_chip =
	{ 0, 0, 512, 512, 263, 120, 96, 96 };

/* Lobby banner — bank 0x0048A230[0] */
static const UiBlit k_lobby_banner =
	{ 0, 0, 496, 80, 72, 368, 496, 80 };

/* SYSTEM.Img — FUN_00470730 / DAT_0049d178 (P1–P3 frames at Y=16) */
#define HUD_PARTY_SLOTS 3
static const UiBlit k_sys_hp_frame =
	{ 6, 4, 216, 40, 0, 16, 216, 40 };
/* Empty chrome: bank 0x0049D150 (lighter P2 UV). */
static const UiBlit k_sys_hp_frame_empty =
	{ 230, 4, 216, 40, 0, 16, 216, 40 };
static const f32 k_sys_slot_x[HUD_PARTY_SLOTS] = { 0.0f, 213.0f, 426.0f };
#define HUD_SLOT_Y 16.0f
#define HUD_SLOT_W 216.0f
#define HUD_SLOT_H 40.0f
/* "Empty..." strip — bank 0x0049DCF0[0] */
static const UiBlit k_sys_empty_label =
	{ 816, 976, 208, 16, 0, 0, 120, 12 };
static const UiBlit k_sys_name_bar =
	{ 80, 64, 176, 16, 18, 462, 176, 16 };
/* Fill bars: src from bank 0x0049D1E0 / 0x0049D230; dest relative to slot */
static const UiBlit k_sys_hp_fill =
	{ 1, 57, 78, 6, 22, 24, 78, 6 };
static const UiBlit k_sys_sp_fill =
	{ 1, 65, 78, 6, 22, 32, 78, 6 };
static const UiBlit k_sys_bar_empty =
	{ 1, 73, 78, 6, 22, 24, 78, 6 };
/* Radar chrome — bank 0x0049DCB8 (264x56). Centered under slot 1. */
static const UiBlit k_sys_radar =
	{ 124, 252, 264, 56, 188, 58, 264, 56 };
#define HUD_RADAR_PAD 8.0f
/* Follow-character bars: DAT_0049e400 elem0 = (+46, +56) from actor origin.
 * Y is up the sprite (screen Y decreases). */
#define HUD_FOLLOW_HP_OX  (-32.0f) /* 46 - 78 */
#define HUD_FOLLOW_HP_OY  (-56.0f)
#define HUD_FOLLOW_SP_OX  (-32.0f)
#define HUD_FOLLOW_SP_OY  (-48.0f)
/* Top bar (FUN_0046d5e0) — glyph dests, 640x480 */
#define HUD_TOP_STAGE_X  168.0f
#define HUD_TOP_STAGE_Y    2.0f
#define HUD_TOP_TIME_X   400.0f
#define HUD_TOP_TIME_Y     2.0f
#define HUD_TOP_ITEM_X   528.0f
#define HUD_TOP_ITEM_Y     2.0f
#define HUD_MEM_X        508.0f
#define HUD_MEM_Y        434.0f
/* Pause chrome — bank 0x0049FFD0[0]; PC also uses centered menu sprites. */
static const UiBlit k_sys_pause_panel =
	{ 656, 832, 272, 96, 184, 160, 272, 96 };

#endif /* UI_LAYOUTS_H */
