/*---------------------------------------------------------------------------------
  STAGE01 SD asset load test — slim DOL + giant STAGE01.TPL on SD

  Authentic boot / hub flow:
    CAUTION → LOGO → INTRO(stub) → TITLE → LOAD → LOBBY → STSEL → PLAY
  DOL holds code/anim tables only. Textures load from:
    sda:/RBO/ASSETS/TITLE.TPL   (Dolphin Slot A) or sdc:/RBO/... (SD2SP2)
    sda:/RBO/ASSETS/LOBBY.TPL
    sda:/RBO/ASSETS/STSEL.TPL
    sda:/RBO/ASSETS/STAGE01.TPL
  One UI TPL resident at a time (except PLAY owns STAGE01 only).
  SOUND/: BGM + UI SE WAVs (ASND). START/RESET → Swiss (escape.h).

  Controls (PAD 0):
    Boot scenes — A skips / advances
    TITLE       — D-Pad select, A confirm (Start / Ranking stub / Exit→Swiss)
    LOBBY       — D-Pad / A through slot → ready → depart
    STSEL       — D-Pad select STAGE 1 LEAN vs STAGE 1 FULL; A loads that path
      LEAN        — mined section rail only (safe MEM)
      FULL        — STAGE01.FOB + HOST_D spawn VM
      In stage:
      D-Pad L/R   — walk Novice (X only; Y is spawn/floor, no vertical)
      A           — attack; kill despawns enemy (pool recycle)
      B           — exit to stage select (unload STAGE01 + FOB); closes pause
      Y           — debug: all sprites 1x / 1.5x / 2x (default 1x = DAT dest)
      START       — pause menu (Lobby / Title / Exit→Swiss)
      RESET       — Swiss; START→Swiss outside PLAY
      Sections    — clear pack raises lock_hi (contiguous walk, no teleport)
---------------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/tpl.h>
#include <ogc/cache.h>

#include "textures.h"
#include "escape.h"
#include "sd_assets.h"
#include "rbo_dvd.h"
/* Port skeleton surface (PC→GC modules). See docs/RE_FINDINGS.md + SCENE_HANDLING.md */
#include "rbo_platform.h"
#include "ui_layouts.h"
#include "gc_pad_icons.h"
#include "stage01_spawns.h"
#include "rbo_fob.h"
#include "rbo_spawn.h"
#include "novice_anims.h"
#include "poring_anims.h"
#include "equrips_anims.h"
#include "rocker_anims.h"
#include "creamy_anims.h"
#include "spore_anims.h"
#include "fabre_anims.h"
#include "lunatic_anims.h"
#include "willow_anims.h"
#include "chon_chon_anims.h"
#include "npc_all_anims.h"
#include "stage01_npcs.h"
#include "rbo_hud.h"

/* Retire a transfer the previous DOL left in flight, before libogc's untimed
 * waits in __exi_init / __si_init reach it. Without this, booting after a DOL
 * that died mid-SI or mid-EXI hangs before VIDEO_Init and shows only green. */
ESCAPE_SYS_PREINIT();

#define DEFAULT_FIFO_SIZE (256 * 1024)
#define MAX_DRAW_PARTS 32
#define WALK_FRAME_TICKS 6
#define NUM_BG_PLATES 5
/* PC BG01_*_01 is 2048px wide; StageMain HOST_C.1 locks use those pixel X
 * values directly (2048, 4096, …, 10240). World units == PC plate pixels. */
#define BG_W 2048
#define BG_H 512
#define STAGE_W (NUM_BG_PLATES * BG_W) /* 10240 */
#define SCREEN_W 640
#define SCREEN_H 480
/* Console bake (e.g. 640x320) stretches onto BG_W-wide quads. */
#define MEM1_BUDGET (24 * 1024 * 1024)
#define NUM_ENEMY_TYPES 9
#define NUM_ENEMIES ENEMY_POOL
#define ENEMY_HP_HITS 1 /* one solid hit kills (flow test) */
#define PLATE_A2_INDEX 2 /* bg01_03 — only plate with mid/near overlays */
#define A5_SIZE 256
/* Ground plates + actors scroll 1:1 with cam. Overlays drift for depth. */
#define PARALLAX_MID  0.78f
#define PARALLAX_NEAR 1.18f
#define PARALLAX_A5   1.28f
/* Walkable X only. PC HOST_C.1 Y half is always 0; DATA rec[9] is a
 * spawn depth offset (negative = further back / smaller screen Y).
 * Sprite compositor (FUN_0044ec20 / FUN_004330e0) adds 448 after the
 * 3D project — that is the screen ground origin, not 400. */
#define FLOOR_Y       448.0f
#define FLOOR_X_PAD   40.0f
/* HAN2RBO dest_w/dest_h are already 640x480 pixel sizes. Part x_scale/
 * y_scale is walk/combat squash. PC actor extra scale is identity (1.0).
 * 1.5 was a proto visibility fudge. Y still cycles 1 / 1.5 / 2 for A/B. */
#define CHAR_SCALE_DEFAULT 1.0f
/* Novice basic attack stub (ATTACK_A startup → ATTACK_B contact). */
#define ATK_TOTAL       22
#define ATK_ACTIVE_START 7
#define ATK_ACTIVE_END   14
#define ATK_REACH        78.0f
#define ATK_HALF_H      48.0f
#define HIT_STUN_FRAMES   18
#define HIT_KNOCK         14.0f
#define AGGRO_RANGE      420.0f /* EventCall-ish chase when novice enters band */
#define AGGRO_SPEED        1.15f
#define CAUTION_FRAMES (60 * 4) /* ~4s at 60Hz, A skips */
#define LOGO_FRAMES    (60 * 3) /* per studio logo (~PC 0xC0) */
#define INTRO_FRAMES   (60 * 3)
#define LOAD_FRAMES    (60 * 1)
#define MENU_COUNT 3
#define SAVE_SLOTS 3
#define SAVE_MAGIC 0x52424F31u /* RBO1 */
#define SAVE_PATH "RBO/SAVE.DAT"

enum {
	SCENE_CAUTION = RBO_SCENE_CAUTION,
	SCENE_LOGO = RBO_SCENE_LOGO,
	SCENE_INTRO = RBO_SCENE_INTRO,
	SCENE_TITLE = RBO_SCENE_TITLE,
	SCENE_LOAD = RBO_SCENE_LOAD,
	SCENE_LOBBY = RBO_SCENE_LOBBY,
	SCENE_STSEL = RBO_SCENE_STSEL,
	SCENE_PLAY = RBO_SCENE_PLAY
};

enum {
	MENU_START = 0,
	MENU_RANKING,
	MENU_EXIT
};

enum {
	LOBBY_GREET = 0,
	LOBBY_SLOTS,
	LOBBY_CREATE,
	LOBBY_READY,
	LOBBY_DEPART
};

/* PC pause menu (FUN_004748f0): skip overlay FUN_00474870; returns 2/3/4. */
enum {
	PAUSE_LOBBY = 0, /* RETRY (RETURN LOBBY) → FUN_0043c170 case 2 */
	PAUSE_TITLE,     /* RETURN TITLE → case 3 + FUN_004087a0 party reset */
	PAUSE_EXIT,      /* EXIT GAME → case 4 → Swiss */
	PAUSE_COUNT
};

enum {
	DEPART_ADVENTURE = 0,
	DEPART_STATUS,
	DEPART_EXIT,
	DEPART_COUNT
};

/* STSEL → PLAY path. FULL allocates STAGE01.FOB only after confirm. */
enum {
	PLAY_LEAN = 0, /* mined section rail */
	PLAY_FULL,     /* + FOB VM (experimental; MEM risk) */
	PLAY_MODE_COUNT
};

#define FOB_STAGE01_PATH "RBO/SCRIPTS/STAGE01.FOB"
/* StageMain phase slot @12098 in retail STAGE01.FOB bytecode. */
#define FOB_PHASE_SLOT 12098u
#define FOB_INIT_BUDGET  80000u
#define FOB_FRAME_BUDGET 12000u

typedef struct {
	u32 magic;
	u8 occupied[SAVE_SLOTS];
	u8 class_id[SAVE_SLOTS]; /* 0=Novice */
	u8 level[SAVE_SLOTS];
	char name[SAVE_SLOTS][12];
	u16 hp[SAVE_SLOTS];
	u16 sp[SAVE_SLOTS];
	u16 hp_max[SAVE_SLOTS];
	u16 sp_max[SAVE_SLOTS];
} SaveFile;

typedef struct {
	s16 src_x, src_y, src_w, src_h;
	s16 dest_x, dest_y, dest_w, dest_h;
	s16 origin_x, origin_y;
	s8 layer;
	u8 flip_h;
	u8 sheet;
	f32 x_scale, y_scale, rotation;
} SpritePart;

typedef struct {
	const char *name;
	u8 count;
	const SpritePart *parts;
} SpritePose;

typedef struct {
	f32 x, y;
	f32 facing;
	f32 scale;
	int pose;
	int manual_pose;
	int walk_frame;
	int walk_timer;
	int moving;
} Actor;

typedef struct {
	Actor base;
	u8 type;
	u8 alive;
	u8 hp;
	s16 hit_timer;
} EnemyActor;

typedef struct {
	Actor base;
	u8 alive;
} SceneryNpc;

typedef struct {
	const SpritePose *poses;
	int pose_count;
	int walk_poses[8];
	int walk_count;
	int idle;
	GXTexObj *sheets;
	int nsheets;
	int sheet_w;
	int sheet_h;
} CharDef;

static void *frameBuffer[2] = {NULL, NULL};
static GXRModeObj *rmode;
static GXTexObj texBgPlate[NUM_BG_PLATES];
static GXTexObj texA2Mid;
static GXTexObj texA2Near;
static GXTexObj texA5;
static GXTexObj texPoring[PORING_SHEET_COUNT];
static GXTexObj texEqurips[EQURIPS_SHEET_COUNT];
static GXTexObj texRocker[ROCKER_SHEET_COUNT];
static GXTexObj texCreamy[CREAMY_SHEET_COUNT];
static GXTexObj texSpore[SPORE_SHEET_COUNT];
static GXTexObj texFabre[FABRE_SHEET_COUNT];
static GXTexObj texLunatic[LUNATIC_SHEET_COUNT];
static GXTexObj texWillow[WILLOW_SHEET_COUNT];
static GXTexObj texChonChon[CHON_CHON_SHEET_COUNT];
static GXTexObj texNovice[NOVICE_SHEET_COUNT];
static GXTexObj texNpc[NPC_ALL_SHEET_COUNT];
static GXTexObj texSystem;
static GXTexObj texCaution;
static GXTexObj texBootLogos;
static GXTexObj texIntro;
static GXTexObj texTitleAtlas;
static GXTexObj texLobby;
static GXTexObj texCreate;
static GXTexObj texStsel;
static GXTexObj texStselChip;
static GXTexObj texPadIcons;
static int g_logo_variant; /* 0..BOOT_LOGO_COUNT-1 during SCENE_LOGO */
static void *g_tpl_data = NULL;
static u32 g_tpl_size = 0;
static TPLFile g_tpl;
static void *g_ui_data = NULL;
static u32 g_ui_size = 0;
static TPLFile g_ui_tpl;
static const char *g_sd_vol = NULL;
static int g_scene = SCENE_CAUTION;
static int g_scene_timer = 0;
static int g_stage_ready = 0;
static int g_menu_sel = MENU_START;
static int g_menu_flash = 0;
static int g_lobby_mode = LOBBY_GREET;
static int g_slot_sel = 0;
static int g_depart_sel = DEPART_ADVENTURE;
static int g_ranking_flash = 0;
static int g_active_slot = 0;
static SaveFile g_save;
static u32 g_play_timer = 0;
static Actor novice;
static int novice_atk_a;
static int novice_atk_b;
static int g_atk_timer;
static int g_atk_hit; /* already applied hit this swing */
static int g_hit_flash_t;
static f32 g_hit_flash_x, g_hit_flash_y;
static int g_paused;
static int g_pause_sel;
static int g_section;
static int g_stage_clear;
static f32 g_lock_lo;
static f32 g_lock_hi;
static f32 g_ground_y;
static const StagePlan *g_stage;
static int g_stsel_sel; /* PLAY_LEAN / PLAY_FULL */
static int g_play_mode;
static RboFob g_fob;
static int g_fob_loaded;
static int g_fob_init_rc;
static int g_fob_frame_rc;
static int g_fob_err_flash; /* STSEL: show load failure briefly */

/* Keep g_scene and rbo_scene in lockstep (see docs/SCENE_HANDLING.md). */
static void scene_goto(int id)
{
	g_scene = id;
	rbo_scene_goto((RboSceneId)id);

	/* BGM follows hub scenes (CD OGG → SD SOUND/bgm). */
	switch (id) {
	case SCENE_LOGO:
		rbo_audio_play_bgm("RBO/SOUND/bgm/logo.ogg", 1);
		break;
	case SCENE_TITLE:
		rbo_audio_play_bgm("RBO/SOUND/bgm/title.ogg", 1);
		break;
	case SCENE_LOAD:
	case SCENE_LOBBY:
		rbo_audio_play_bgm("RBO/SOUND/bgm/bar.ogg", 1);
		break;
	case SCENE_STSEL:
		rbo_audio_play_bgm("RBO/SOUND/bgm/stsel.ogg", 1);
		break;
	case SCENE_PLAY:
		rbo_audio_play_bgm("RBO/SOUND/bgm/stage01.ogg", 1);
		break;
	default:
		break;
	}
}

static void quit_to_swiss(void)
{
	rbo_audio_shutdown();
	escape_exit();
}
static EnemyActor enemies[NUM_ENEMIES];
static SceneryNpc scenery[NPC_SCENERY_MAX];
static f32 g_char_scale = CHAR_SCALE_DEFAULT;
static CharDef g_chardefs[NUM_ENEMY_TYPES];
static CharDef g_npcdef;
static int novice_walk_poses[8];
static int novice_walk_count;
static int novice_idle;
static u32 g_arena_free_now;

enum {
	GLYPH_0 = 0, GLYPH_1, GLYPH_2, GLYPH_3, GLYPH_4,
	GLYPH_5, GLYPH_6, GLYPH_7, GLYPH_8, GLYPH_9,
	GLYPH_DOT, GLYPH_K, GLYPH_B, GLYPH_M, GLYPH_N, GLYPH_V,
	GLYPH_S, GLYPH_D, GLYPH_E, GLYPH_R,
	GLYPH_A, GLYPH_P, GLYPH_T,
	GLYPH_G, GLYPH_O, GLYPH_I, GLYPH_X, GLYPH_C, GLYPH_L, GLYPH_U, GLYPH_F, GLYPH_Y, GLYPH_W, GLYPH_H,
	GLYPH_GT,
	GLYPH_SLASH, GLYPH_PCT, GLYPH_SPC,
	GLYPH_MINUS, GLYPH_APOS, GLYPH_QUOTE,
	GLYPH_COUNT
};

static const u8 s_glyphs[GLYPH_COUNT][7] = {
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
	{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
	{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
	{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
	{0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
	{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
	{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
	{0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, /* S */
	{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
	{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
	{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, /* G */
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
	{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
	{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
	{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
	{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
	{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
	{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
	{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
	{0x08,0x0C,0x0E,0x0F,0x0E,0x0C,0x08}, /* > */
	{0x01,0x02,0x04,0x04,0x08,0x10,0x10}, /* / */
	{0x19,0x19,0x02,0x04,0x08,0x13,0x13}, /* % */
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* sp */
	{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
	{0x04,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
	{0x0A,0x0A,0x14,0x00,0x00,0x00,0x00}, /* " */
};

static int glyph_for(char c)
{
	if (c >= '0' && c <= '9') return GLYPH_0 + (c - '0');
	if (c == '.') return GLYPH_DOT;
	if (c == 'K' || c == 'k') return GLYPH_K;
	if (c == 'B' || c == 'b') return GLYPH_B;
	if (c == 'M' || c == 'm') return GLYPH_M;
	if (c == 'N' || c == 'n') return GLYPH_N;
	if (c == 'V' || c == 'v') return GLYPH_V;
	if (c == 'S' || c == 's') return GLYPH_S;
	if (c == 'D' || c == 'd') return GLYPH_D;
	if (c == 'E' || c == 'e') return GLYPH_E;
	if (c == 'R' || c == 'r') return GLYPH_R;
	if (c == 'A' || c == 'a') return GLYPH_A;
	if (c == 'P' || c == 'p') return GLYPH_P;
	if (c == 'T' || c == 't') return GLYPH_T;
	if (c == 'G' || c == 'g') return GLYPH_G;
	if (c == 'O' || c == 'o') return GLYPH_O;
	if (c == 'I' || c == 'i') return GLYPH_I;
	if (c == 'X' || c == 'x') return GLYPH_X;
	if (c == 'C' || c == 'c') return GLYPH_C;
	if (c == 'L' || c == 'l') return GLYPH_L;
	if (c == 'U' || c == 'u') return GLYPH_U;
	if (c == 'F' || c == 'f') return GLYPH_F;
	if (c == 'Y' || c == 'y') return GLYPH_Y;
	if (c == 'W' || c == 'w') return GLYPH_W;
	if (c == 'H' || c == 'h') return GLYPH_H;
	if (c == '>') return GLYPH_GT;
	if (c == '/') return GLYPH_SLASH;
	if (c == '%') return GLYPH_PCT;
	if (c == '-') return GLYPH_MINUS;
	if (c == '\'') return GLYPH_APOS;
	if (c == '"') return GLYPH_QUOTE;
	return GLYPH_SPC;
}

static int find_pose(const SpritePose *table, int count, const char *name)
{
	int i;

	for (i = 0; i < count; i++) {
		if (strcmp(table[i].name, name) == 0)
			return i;
	}
	return -1;
}

static void build_walk_cycle(CharDef *cd, const char **names)
{
	int i, idx;

	cd->walk_count = 0;
	for (i = 0; names[i]; i++) {
		idx = find_pose(cd->poses, cd->pose_count, names[i]);
		if (idx >= 0 && cd->walk_count < 8)
			cd->walk_poses[cd->walk_count++] = idx;
	}
	cd->idle = find_pose(cd->poses, cd->pose_count, "BASE");
	if (cd->idle < 0)
		cd->idle = 0;
}

static void init_char_defs(void)
{
	static const char *walk_std[] = {
		"BASE", "PATTERN001", "PATTERN002", "PATTERN003", "PATTERN004", NULL
	};
	static const char *walk_spore[] = {
		"BASE", "PATTERN001", "PATTERN002", "PATTERN008", "PATTERN009", NULL
	};

	g_chardefs[0].poses = (const SpritePose *)(const void *)poring_poses;
	g_chardefs[0].pose_count = PORING_POSE_COUNT;
	g_chardefs[0].sheets = texPoring;
	g_chardefs[0].nsheets = PORING_SHEET_COUNT;
	g_chardefs[0].sheet_w = PORING_SHEET_W;
	g_chardefs[0].sheet_h = PORING_SHEET_H;
	build_walk_cycle(&g_chardefs[0], walk_std);

	g_chardefs[1].poses = (const SpritePose *)(const void *)equrips_poses;
	g_chardefs[1].pose_count = EQURIPS_POSE_COUNT;
	g_chardefs[1].sheets = texEqurips;
	g_chardefs[1].nsheets = EQURIPS_SHEET_COUNT;
	g_chardefs[1].sheet_w = EQURIPS_SHEET_W;
	g_chardefs[1].sheet_h = EQURIPS_SHEET_H;
	build_walk_cycle(&g_chardefs[1], walk_std);

	g_chardefs[2].poses = (const SpritePose *)(const void *)rocker_poses;
	g_chardefs[2].pose_count = ROCKER_POSE_COUNT;
	g_chardefs[2].sheets = texRocker;
	g_chardefs[2].nsheets = ROCKER_SHEET_COUNT;
	g_chardefs[2].sheet_w = ROCKER_SHEET_W;
	g_chardefs[2].sheet_h = ROCKER_SHEET_H;
	build_walk_cycle(&g_chardefs[2], walk_std);

	g_chardefs[3].poses = (const SpritePose *)(const void *)creamy_poses;
	g_chardefs[3].pose_count = CREAMY_POSE_COUNT;
	g_chardefs[3].sheets = texCreamy;
	g_chardefs[3].nsheets = CREAMY_SHEET_COUNT;
	g_chardefs[3].sheet_w = CREAMY_SHEET_W;
	g_chardefs[3].sheet_h = CREAMY_SHEET_H;
	build_walk_cycle(&g_chardefs[3], walk_std);

	g_chardefs[4].poses = (const SpritePose *)(const void *)spore_poses;
	g_chardefs[4].pose_count = SPORE_POSE_COUNT;
	g_chardefs[4].sheets = texSpore;
	g_chardefs[4].nsheets = SPORE_SHEET_COUNT;
	g_chardefs[4].sheet_w = SPORE_SHEET_W;
	g_chardefs[4].sheet_h = SPORE_SHEET_H;
	build_walk_cycle(&g_chardefs[4], walk_spore);

	g_chardefs[5].poses = (const SpritePose *)(const void *)fabre_poses;
	g_chardefs[5].pose_count = FABRE_POSE_COUNT;
	g_chardefs[5].sheets = texFabre;
	g_chardefs[5].nsheets = FABRE_SHEET_COUNT;
	g_chardefs[5].sheet_w = FABRE_SHEET_W;
	g_chardefs[5].sheet_h = FABRE_SHEET_H;
	build_walk_cycle(&g_chardefs[5], walk_std);

	g_chardefs[6].poses = (const SpritePose *)(const void *)lunatic_poses;
	g_chardefs[6].pose_count = LUNATIC_POSE_COUNT;
	g_chardefs[6].sheets = texLunatic;
	g_chardefs[6].nsheets = LUNATIC_SHEET_COUNT;
	g_chardefs[6].sheet_w = LUNATIC_SHEET_W;
	g_chardefs[6].sheet_h = LUNATIC_SHEET_H;
	build_walk_cycle(&g_chardefs[6], walk_std);

	g_chardefs[7].poses = (const SpritePose *)(const void *)willow_poses;
	g_chardefs[7].pose_count = WILLOW_POSE_COUNT;
	g_chardefs[7].sheets = texWillow;
	g_chardefs[7].nsheets = WILLOW_SHEET_COUNT;
	g_chardefs[7].sheet_w = WILLOW_SHEET_W;
	g_chardefs[7].sheet_h = WILLOW_SHEET_H;
	build_walk_cycle(&g_chardefs[7], walk_std);

	g_chardefs[8].poses = (const SpritePose *)(const void *)chon_chon_poses;
	g_chardefs[8].pose_count = CHON_CHON_POSE_COUNT;
	g_chardefs[8].sheets = texChonChon;
	g_chardefs[8].nsheets = CHON_CHON_SHEET_COUNT;
	g_chardefs[8].sheet_w = CHON_CHON_SHEET_W;
	g_chardefs[8].sheet_h = CHON_CHON_SHEET_H;
	build_walk_cycle(&g_chardefs[8], walk_std);

	novice_walk_count = 0;
	{
		int i, idx;

		for (i = 0; walk_std[i]; i++) {
			idx = find_pose((const SpritePose *)(const void *)novice_poses,
					NOVICE_POSE_COUNT, walk_std[i]);
			if (idx >= 0 && novice_walk_count < 8)
				novice_walk_poses[novice_walk_count++] = idx;
		}
	}
	novice_idle = find_pose((const SpritePose *)(const void *)novice_poses,
				NOVICE_POSE_COUNT, "BASE");
	if (novice_idle < 0)
		novice_idle = 0;
	novice_atk_a = find_pose((const SpritePose *)(const void *)novice_poses,
				 NOVICE_POSE_COUNT, "ATTACK_A");
	novice_atk_b = find_pose((const SpritePose *)(const void *)novice_poses,
				 NOVICE_POSE_COUNT, "ATTACK_B");
	if (novice_atk_a < 0)
		novice_atk_a = novice_idle;
	if (novice_atk_b < 0)
		novice_atk_b = novice_atk_a;

	g_npcdef.poses = (const SpritePose *)(const void *)npc_all_poses;
	g_npcdef.pose_count = NPC_ALL_POSE_COUNT;
	g_npcdef.sheets = texNpc;
	g_npcdef.nsheets = NPC_ALL_SHEET_COUNT;
	g_npcdef.sheet_w = NPC_ALL_SHEET_W;
	g_npcdef.sheet_h = NPC_ALL_SHEET_H;
	g_npcdef.idle = 0;
	g_npcdef.walk_count = 0;
}

static void sample_mem(void)
{
	u32 lo = (u32)SYS_GetArena1Lo();
	u32 hi = (u32)SYS_GetArena1Hi();

	g_arena_free_now = (hi > lo) ? (hi - lo) : 0;
}

static void drawTexturedQuadUV(f32 x, f32 y, f32 w, f32 h,
			       f32 u0, f32 v0, f32 u1, f32 v1)
{
	rbo_gx_blit_rect(x, y, w, h, u0, v0, u1, v1);
}

static void drawSolidQuad(f32 x, f32 y, f32 w, f32 h)
{
	/* Always emit TEX0 with POS — matches working pre-stub FIFO layout.
	 * VtxDesc stays POS+TEX0 for the whole frame (see main loop). */
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
		GX_Position2f32(x, y);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x + w, y);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x + w, y + h);
		GX_TexCoord2f32(0, 0);
		GX_Position2f32(x, y + h);
		GX_TexCoord2f32(0, 0);
	GX_End();
}

static void drawGlyph(f32 x, f32 y, int gi, f32 px)
{
	int row, col;
	const u8 *rows = s_glyphs[gi];

	for (row = 0; row < 7; row++) {
		u8 bits = rows[row];
		for (col = 0; col < 5; col++) {
			if (bits & (1u << (4 - col)))
				drawSolidQuad(x + (f32)col * px, y + (f32)row * px, px, px);
		}
	}
}

static void drawHudText(f32 x, f32 y, const char *s, f32 px)
{
	f32 cx = x;

	while (*s) {
		drawGlyph(cx, y, glyph_for(*s), px);
		cx += px * 6.0f;
		s++;
	}
}

static int plate_visible(f32 screen_x, f32 w)
{
	return screen_x + w >= -4.0f && screen_x <= (f32)SCREEN_W + 4.0f;
}

static void drawStageBg(f32 cam_x)
{
	Mtx identity;
	int i;
	f32 screen_x;
	f32 a2_base = (f32)(PLATE_A2_INDEX * BG_W);

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);

	/* Plate order: a0=bg01_01 … a4=bg01_05. Each spans BG_W (=2048) world X. */
	for (i = 0; i < NUM_BG_PLATES; i++) {
		screen_x = (f32)(i * BG_W) - cam_x;
		if (!plate_visible(screen_x, (f32)BG_W))
			continue;

		GX_LoadTexObj(&texBgPlate[i], GX_TEXMAP0);
		drawTexturedQuadUV(screen_x, 0.0f, (f32)BG_W, (f32)SCREEN_H,
				   0.0f, 0.0f, 1.0f, 1.0f);
	}

	/* PC only authors layered mid/near on bg01_03 — do NOT tile stage-wide. */
	screen_x = a2_base - cam_x * PARALLAX_MID;
	if (plate_visible(screen_x, (f32)BG_W)) {
		GX_LoadTexObj(&texA2Mid, GX_TEXMAP0);
		drawTexturedQuadUV(screen_x, 0.0f, (f32)BG_W, (f32)SCREEN_H,
				   0.0f, 0.0f, 1.0f, 1.0f);
	}
	screen_x = a2_base - cam_x * PARALLAX_NEAR;
	if (plate_visible(screen_x, (f32)BG_W)) {
		GX_LoadTexObj(&texA2Near, GX_TEXMAP0);
		drawTexturedQuadUV(screen_x, 0.0f, (f32)BG_W, (f32)SCREEN_H,
				   0.0f, 0.0f, 1.0f, 1.0f);
	}
}

static void drawA5Deco(f32 cam_x)
{
	f32 stage_x = (f32)(PLATE_A2_INDEX * BG_W + BG_W - A5_SIZE);
	f32 screen_x = stage_x - cam_x * PARALLAX_A5;

	if (!plate_visible(screen_x, (f32)A5_SIZE))
		return;

	GX_LoadTexObj(&texA5, GX_TEXMAP0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	drawTexturedQuadUV(screen_x, 0.0f, (f32)A5_SIZE, (f32)A5_SIZE,
			   0.0f, 0.0f, 1.0f, 1.0f);
}

static void drawSpritePart(const SpritePart *p, f32 char_x, f32 char_y,
			   f32 scale, f32 facing,
			   GXTexObj *sheets, int nsheets,
			   int sheet_w, int sheet_h)
{
	Mtx m, t, r, s, tmp;
	f32 dw, dh, sx = p->x_scale, sy = p->y_scale;
	f32 u0, v0, u1, v1, ang, ox, oy;
	int sh = (int)p->sheet;

	if (p->src_w <= 0 || p->src_h <= 0)
		return;
	if (sh < 0 || sh >= nsheets)
		sh = 0;

	dw = (f32)((p->dest_w > 0) ? p->dest_w : p->src_w);
	dh = (f32)((p->dest_h > 0) ? p->dest_h : p->src_h);
	if (sx < 0.0f) sx = -sx;
	if (sy < 0.0f) sy = -sy;

	ox = (f32)p->origin_x;
	oy = (f32)p->origin_y;
	ang = p->rotation;
	while (ang > 180.0f) ang -= 360.0f;
	while (ang < -180.0f) ang += 360.0f;
	if (facing < 0.0f) ang = -ang;

	guMtxTrans(m, -ox, -oy, 0.0f);
	guMtxScale(s, sx, sy, 1.0f);
	guMtxConcat(s, m, tmp); guMtxCopy(tmp, m);
	if (fabsf(ang) > 0.5f) {
		guMtxRotDeg(r, 'Z', ang);
		guMtxConcat(r, m, tmp); guMtxCopy(tmp, m);
	}
	guMtxTrans(t, ox, oy, 0.0f);
	guMtxConcat(t, m, tmp); guMtxCopy(tmp, m);
	guMtxTrans(t, (f32)p->dest_x, (f32)p->dest_y, 0.0f);
	guMtxConcat(t, m, tmp); guMtxCopy(tmp, m);
	guMtxScale(s, facing * scale, scale, 1.0f);
	guMtxConcat(s, m, tmp); guMtxCopy(tmp, m);
	guMtxTrans(t, char_x, char_y, 0.0f);
	guMtxConcat(t, m, tmp); guMtxCopy(tmp, m);
	GX_LoadPosMtxImm(m, GX_PNMTX0);

	GX_LoadTexObj(&sheets[sh], GX_TEXMAP0);

	u0 = (f32)p->src_x / (f32)sheet_w;
	v0 = (f32)p->src_y / (f32)sheet_h;
	u1 = (f32)(p->src_x + p->src_w) / (f32)sheet_w;
	v1 = (f32)(p->src_y + p->src_h) / (f32)sheet_h;
	if (p->flip_h) {
		f32 u = u0;
		u0 = u1;
		u1 = u;
	}
	drawTexturedQuadUV(0.0f, 0.0f, dw, dh, u0, v0, u1, v1);
}

static void drawCharacterPose(const CharDef *cd, int pose_index,
			      f32 origin_x, f32 origin_y,
			      f32 scale, f32 facing)
{
	const SpritePose *pose;
	int i, drawn;

	if (pose_index < 0 || pose_index >= cd->pose_count)
		return;
	pose = &cd->poses[pose_index];
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	drawn = 0;
	for (i = 0; i < pose->count && drawn < MAX_DRAW_PARTS; i++) {
		drawSpritePart(&pose->parts[i], origin_x, origin_y, scale, facing,
			       cd->sheets, cd->nsheets, cd->sheet_w, cd->sheet_h);
		drawn++;
	}
}

static void drawNovicePose(f32 origin_x, f32 origin_y, f32 scale,
			   int pose_index, f32 facing)
{
	const SpritePose *poses = (const SpritePose *)(const void *)novice_poses;
	CharDef cd;

	cd.poses = poses;
	cd.pose_count = NOVICE_POSE_COUNT;
	cd.sheets = texNovice;
	cd.nsheets = NOVICE_SHEET_COUNT;
	cd.sheet_w = NOVICE_SHEET_W;
	cd.sheet_h = NOVICE_SHEET_H;
	drawCharacterPose(&cd, pose_index, origin_x, origin_y, scale, facing);
}

static void update_walk(Actor *a, const int *cycle, int cycle_n, int idle)
{
	if (a->manual_pose)
		return;
	if (a->moving && cycle_n > 1) {
		a->walk_timer++;
		if (a->walk_timer >= WALK_FRAME_TICKS) {
			a->walk_timer = 0;
			a->walk_frame++;
			if (a->walk_frame >= cycle_n)
				a->walk_frame = 1;
			if (a->walk_frame < 1)
				a->walk_frame = 1;
		}
		a->pose = cycle[a->walk_frame];
	} else {
		a->walk_timer = 0;
		a->walk_frame = 0;
		a->pose = idle;
	}
}

static void clamp_to_stage_x(Actor *a)
{
	if (a->x < FLOOR_X_PAD)
		a->x = FLOOR_X_PAD;
	if (a->x > (f32)STAGE_W - FLOOR_X_PAD)
		a->x = (f32)STAGE_W - FLOOR_X_PAD;
}

static int attack_active(void)
{
	int elapsed;

	if (g_atk_timer <= 0)
		return 0;
	elapsed = ATK_TOTAL - g_atk_timer;
	return (elapsed >= ATK_ACTIVE_START && elapsed <= ATK_ACTIVE_END);
}

static int count_alive_enemies(void)
{
	int i, n = 0;

	for (i = 0; i < NUM_ENEMIES; i++) {
		if (enemies[i].alive)
			n++;
	}
	return n;
}

static void apply_char_scale(f32 s)
{
	int i;

	g_char_scale = s;
	novice.scale = s;
	for (i = 0; i < NUM_ENEMIES; i++)
		enemies[i].base.scale = s;
	for (i = 0; i < NPC_SCENERY_MAX; i++)
		scenery[i].base.scale = s;
}

static void clear_enemy_pool(void)
{
	int i;

	for (i = 0; i < NUM_ENEMIES; i++) {
		enemies[i].alive = 0;
		enemies[i].hp = 0;
		enemies[i].hit_timer = 0;
	}
}

/* Fill pool from spawns tagged with this section (recycle slots). */
static void spawn_section(int section, f32 ground_y)
{
	int i, slot = 0;

	clear_enemy_pool();
	if (!g_stage || section < 0 || section >= g_stage->n_sections)
		return;
	if (g_stage->sections[section].flags & STAGE_SEC_F_NO_SPAWN)
		return;
	if (g_stage->sections[section].kind == STAGE_SEC_CLEAR)
		return;

	for (i = 0; i < g_stage->n_spawns && slot < NUM_ENEMIES; i++) {
		const StageSpawn *sp = &g_stage->spawns[i];
		u8 t;

		if (sp->section != (u8)section)
			continue;
		t = sp->type;
		if (t >= NUM_ENEMY_TYPES)
			t = 0;
		enemies[slot].type = t;
		enemies[slot].alive = 1;
		enemies[slot].hp = ENEMY_HP_HITS;
		enemies[slot].hit_timer = 0;
		enemies[slot].base.x = sp->x;
		enemies[slot].base.y = ground_y + sp->y_off;
		enemies[slot].base.facing = sp->facing;
		enemies[slot].base.scale = g_char_scale;
		enemies[slot].base.pose = g_chardefs[t].idle;
		enemies[slot].base.manual_pose = 0;
		enemies[slot].base.walk_frame = 0;
		enemies[slot].base.walk_timer = slot % WALK_FRAME_TICKS;
		enemies[slot].base.moving = 0;
		clamp_to_stage_x(&enemies[slot].base);
		slot++;
	}
}

static void clear_scenery_pool(void)
{
	int i;

	for (i = 0; i < NPC_SCENERY_MAX; i++)
		scenery[i].alive = 0;
}

static void spawn_scenery(f32 ground_y)
{
	int i, n;

	clear_scenery_pool();
	n = STAGE01_NPC_ROWS;
	if (n > NPC_SCENERY_MAX)
		n = NPC_SCENERY_MAX;
	for (i = 0; i < n; i++) {
		const StageNpcRow *row = &k_stage01_npcs[i];
		int pose = (int)row->pose;

		if (pose < 0 || pose >= g_npcdef.pose_count)
			pose = 0;
		scenery[i].alive = 1;
		scenery[i].base.x = row->x;
		scenery[i].base.y = ground_y + row->y_off;
		scenery[i].base.facing = row->facing;
		scenery[i].base.scale = g_char_scale;
		scenery[i].base.pose = pose;
		scenery[i].base.manual_pose = 1;
		scenery[i].base.walk_frame = 0;
		scenery[i].base.walk_timer = 0;
		scenery[i].base.moving = 0;
		if (scenery[i].base.x < FLOOR_X_PAD)
			scenery[i].base.x = FLOOR_X_PAD;
		if (scenery[i].base.x > (f32)STAGE_W - FLOOR_X_PAD)
			scenery[i].base.x = (f32)STAGE_W - FLOOR_X_PAD;
	}
}

static int near_x(f32 a, f32 b)
{
	f32 d = a - b;
	return (d > -2.0f && d < 2.0f);
}

static int fob_place_enemy(int type, f32 x, f32 y, f32 facing)
{
	int i;

	if (type < 0 || type >= NUM_ENEMY_TYPES)
		return 0;
	for (i = 0; i < NUM_ENEMIES; i++) {
		if (enemies[i].alive && enemies[i].type == (u8)type &&
		    near_x(enemies[i].base.x, x))
			return 0;
	}
	for (i = 0; i < NUM_ENEMIES; i++) {
		if (enemies[i].alive)
			continue;
		enemies[i].type = (u8)type;
		enemies[i].alive = 1;
		enemies[i].hp = ENEMY_HP_HITS;
		enemies[i].hit_timer = 0;
		enemies[i].base.x = x;
		enemies[i].base.y = y;
		enemies[i].base.facing = facing;
		enemies[i].base.scale = g_char_scale;
		enemies[i].base.pose = g_chardefs[type].idle;
		enemies[i].base.manual_pose = 0;
		enemies[i].base.walk_frame = 0;
		enemies[i].base.walk_timer = i % WALK_FRAME_TICKS;
		enemies[i].base.moving = 0;
		clamp_to_stage_x(&enemies[i].base);
		return 1;
	}
	return 0;
}

static int fob_place_npc(int pose, f32 x, f32 y, f32 facing)
{
	int i;

	if (pose < 0 || pose >= g_npcdef.pose_count)
		pose = 0;
	for (i = 0; i < NPC_SCENERY_MAX; i++) {
		if (scenery[i].alive && scenery[i].base.pose == pose &&
		    near_x(scenery[i].base.x, x))
			return 0;
	}
	for (i = 0; i < NPC_SCENERY_MAX; i++) {
		if (scenery[i].alive)
			continue;
		scenery[i].alive = 1;
		scenery[i].base.x = x;
		scenery[i].base.y = y;
		scenery[i].base.facing = facing;
		scenery[i].base.scale = g_char_scale;
		scenery[i].base.pose = pose;
		scenery[i].base.manual_pose = 1;
		scenery[i].base.walk_frame = 0;
		scenery[i].base.walk_timer = 0;
		scenery[i].base.moving = 0;
		if (scenery[i].base.x < FLOOR_X_PAD)
			scenery[i].base.x = FLOOR_X_PAD;
		if (scenery[i].base.x > (f32)STAGE_W - FLOOR_X_PAD)
			scenery[i].base.x = (f32)STAGE_W - FLOOR_X_PAD;
		return 1;
	}
	return 0;
}

/* HOST_D cmd-13 → resident sheets. Unknown char_id / pose is dropped. */
static void fob_on_spawn(const RboSpawnRec *rec, void *user)
{
	RboCharaBind bind;
	f32 x, y, facing, gy;
	int placed;

	(void)user;
	if (!rec)
		return;
	if (!rbo_chara_lookup(rec->char_id, rec->pose, &bind)) {
		rbo_spawn_count_drop(rec->char_id, rec->pose);
		return;
	}
	gy = (g_ground_y > 1.0f) ? g_ground_y : FLOOR_Y;
	x = (f32)rec->x;
	y = gy + (f32)rec->y;
	facing = (rec->facing < 0) ? -1.0f : 1.0f;
	placed = 0;
	if (bind.kind == RBO_BIND_NPC)
		placed = fob_place_npc(bind.npc_pose, x, y, facing);
	else if (bind.kind == RBO_BIND_ENEMY)
		placed = fob_place_enemy(bind.enemy_type, x, y, facing);
	if (placed)
		rbo_spawn_count_place();
}

/* HOST_C.1 is a right-hand gate on one contiguous strip — never teleport.
 * Raise lock_hi only. Never move lock_lo past the player. */
static void apply_gate_lock(f32 want_lo, f32 want_hi)
{
	if (want_lo < FLOOR_X_PAD)
		want_lo = FLOOR_X_PAD;
	if (want_hi > (f32)STAGE_W - FLOOR_X_PAD)
		want_hi = (f32)STAGE_W - FLOOR_X_PAD;
	if (want_hi < want_lo)
		want_hi = want_lo;

	if (want_hi > g_lock_hi)
		g_lock_hi = want_hi;
	if (want_lo <= novice.x)
		g_lock_lo = want_lo;
	if (g_lock_hi < g_lock_lo)
		g_lock_hi = g_lock_lo;
}

static void apply_section_locks(int section)
{
	const StageSection *sec;

	if (!g_stage || section < 0 || section >= g_stage->n_sections) {
		apply_gate_lock(FLOOR_X_PAD, (f32)STAGE_W - FLOOR_X_PAD);
		return;
	}
	sec = &g_stage->sections[section];
	apply_gate_lock(sec->lock_lo, sec->lock_hi);
}

/* Enter section: HOST_C.1-style lo/hi + spawn (or CLEAR). */
static void section_run_enter_cues(const StageSection *sec)
{
	if (!sec)
		return;
	/* StageInit clear.wav / clear-koushin — soft-fail if not on SD yet. */
	if (sec->flags & STAGE_SEC_F_CLEAR_BGM) {
		if (rbo_audio_play_bgm("RBO/SOUND/bgm/clear.ogg", 0) != 0)
			(void)rbo_audio_play_bgm("RBO/SOUND/bgm/stage01.ogg", 1);
	}
}

static void enter_section(int section, f32 ground_y)
{
	const StageSection *sec;

	if (!g_stage || section < 0 || section >= g_stage->n_sections) {
		g_stage_clear = 1;
		clear_enemy_pool();
		apply_gate_lock(FLOOR_X_PAD, (f32)STAGE_W - FLOOR_X_PAD);
		return;
	}

	g_section = section;
	sec = &g_stage->sections[section];
	apply_section_locks(section);
	section_run_enter_cues(sec);

	if (sec->kind == STAGE_SEC_CLEAR || (sec->flags & STAGE_SEC_F_NO_SPAWN)) {
		g_stage_clear = 1;
		clear_enemy_pool();
		return;
	}

	g_stage_clear = 0;
	/* FULL actors come from HOST_D DATA; do not wipe that pool. */
	if (g_play_mode != PLAY_FULL)
		spawn_section(section, ground_y);
}

/* Universal rail: clear combat → next section (field / midboss / boss / clear). */
static int count_section_spawns(int section)
{
	int i, n = 0;

	if (!g_stage)
		return 0;
	for (i = 0; i < g_stage->n_spawns; i++) {
		if (g_stage->spawns[i].section == (u8)section)
			n++;
	}
	return n;
}

static void advance_section_if_clear(void)
{
	const StageSection *sec;

	if (g_stage_clear || g_paused || !g_stage)
		return;
	if (g_section < 0 || g_section >= g_stage->n_sections)
		return;

	sec = &g_stage->sections[g_section];
	if (sec->kind == STAGE_SEC_CLEAR)
		return;
	if (count_alive_enemies() > 0)
		return;
	/* Town / empty packs: do not skip the plate in one frame. Walk to the
	 * current hi gate, then raise lock_hi (contiguous strip). */
	if (count_section_spawns(g_section) == 0) {
		if (novice.x < g_lock_hi - 80.0f)
			return;
	}

	enter_section(g_section + 1, g_ground_y);
}

static void try_attack_hits(void)
{
	f32 box_l, box_r, hy0, hy1;
	int i;

	if (g_atk_hit || !attack_active())
		return;

	box_l = (novice.facing > 0.0f) ? novice.x : novice.x - ATK_REACH;
	box_r = (novice.facing > 0.0f) ? novice.x + ATK_REACH : novice.x;
	hy0 = novice.y - ATK_HALF_H;
	hy1 = novice.y + ATK_HALF_H;

	for (i = 0; i < NUM_ENEMIES; i++) {
		Actor *a = &enemies[i].base;

		if (!enemies[i].alive || enemies[i].hit_timer > 0)
			continue;
		if (a->x < box_l || a->x > box_r || a->y < hy0 || a->y > hy1)
			continue;

		g_atk_hit = 1;
		g_hit_flash_t = 10;
		g_hit_flash_x = a->x;
		g_hit_flash_y = a->y - 40.0f;

		if (enemies[i].hp > 0)
			enemies[i].hp--;
		if (enemies[i].hp == 0) {
			/* Despawn — frees pool slot for later waves. */
			enemies[i].alive = 0;
			enemies[i].hit_timer = 0;
			a->moving = 0;
			break;
		}

		enemies[i].hit_timer = HIT_STUN_FRAMES;
		a->x += novice.facing * HIT_KNOCK;
		a->moving = 0;
		a->manual_pose = 1;
		a->pose = g_chardefs[enemies[i].type].idle;
		clamp_to_stage_x(a);
		break;
	}
}

static void init_actors(f32 ground_y)
{
	g_ground_y = ground_y;
	novice.x = 280.0f;
	novice.y = ground_y;
	novice.facing = 1.0f;
	apply_char_scale(CHAR_SCALE_DEFAULT);
	novice.pose = novice_idle;
	novice.manual_pose = 0;
	novice.walk_frame = 0;
	novice.walk_timer = 0;
	novice.moving = 0;
	clamp_to_stage_x(&novice);
	g_atk_timer = 0;
	g_atk_hit = 0;
	g_hit_flash_t = 0;
	g_stage = &k_stage01_plan;
	g_stage_clear = 0;
	g_lock_lo = FLOOR_X_PAD;
	g_lock_hi = FLOOR_X_PAD;
	if (g_play_mode == PLAY_FULL && g_fob_loaded) {
		/* HOST_D already filled pools during StageInit / will on MainCallback. */
		g_section = 0;
		apply_section_locks(0);
	} else {
		enter_section(0, ground_y);
		spawn_scenery(ground_y);
	}
}

static void save_defaults(void)
{
	int i;

	memset(&g_save, 0, sizeof(g_save));
	g_save.magic = SAVE_MAGIC;
	for (i = 0; i < SAVE_SLOTS; i++) {
		g_save.level[i] = 1;
		g_save.hp_max[i] = 1464;
		g_save.sp_max[i] = 96;
		g_save.hp[i] = g_save.hp_max[i];
		g_save.sp[i] = g_save.sp_max[i];
		snprintf(g_save.name[i], sizeof(g_save.name[i]), "DIE");
	}
}

static void save_load_from_sd(void)
{
	u32 sz = 0;
	void *buf;

	save_defaults();
	if (!g_sd_vol)
		return;
	buf = sd_assets_load(g_sd_vol, SAVE_PATH, &sz);
	if (!buf || sz < sizeof(SaveFile)) {
		if (buf)
			free(buf);
		return;
	}
	memcpy(&g_save, buf, sizeof(SaveFile));
	free(buf);
	if (g_save.magic != SAVE_MAGIC)
		save_defaults();
}

static void save_write_to_sd(void)
{
	if (!g_sd_vol)
		return;
	sd_assets_save(g_sd_vol, SAVE_PATH, &g_save, (u32)sizeof(g_save));
}

static void stamp_new_character(int slot)
{
	if (slot < 0 || slot >= SAVE_SLOTS)
		return;
	g_save.occupied[slot] = 1;
	g_save.class_id[slot] = 0;
	g_save.level[slot] = 1;
	g_save.hp_max[slot] = 1464;
	g_save.sp_max[slot] = 96;
	g_save.hp[slot] = g_save.hp_max[slot];
	g_save.sp[slot] = g_save.sp_max[slot];
	snprintf(g_save.name[slot], sizeof(g_save.name[slot]), "DIE");
	g_active_slot = slot;
	save_write_to_sd();
}

static void unload_ui_textures(void)
{
	if (g_ui_data) {
		free(g_ui_data);
		g_ui_data = NULL;
		g_ui_size = 0;
	}
}

static void unload_stage_textures(void)
{
	if (g_tpl_data) {
		free(g_tpl_data);
		g_tpl_data = NULL;
		g_tpl_size = 0;
	}
	g_stage_ready = 0;
}

static int load_title_textures(void)
{
	if (!g_sd_vol) {
		g_sd_vol = sd_assets_mount();
		if (g_sd_vol)
			rbo_audio_set_vol(g_sd_vol);
		if (!g_sd_vol)
			return 1;
	}

	unload_ui_textures();
	g_ui_data = sd_assets_load_title(g_sd_vol, &g_ui_size);
	if (!g_ui_data || g_ui_size < 64)
		return 3;

	TPL_OpenTPLFromMemory(&g_ui_tpl, g_ui_data, g_ui_size);
	TPL_GetTexture(&g_ui_tpl, 0, &texCaution);    /* cscreen */
	TPL_GetTexture(&g_ui_tpl, 1, &texBootLogos);  /* Boot_logo.Img atlas */
	TPL_GetTexture(&g_ui_tpl, 2, &texIntro);      /* iscreen */
	TPL_GetTexture(&g_ui_tpl, 3, &texTitleAtlas);
	TPL_GetTexture(&g_ui_tpl, 4, &texPadIcons);   /* gc_pad_icons */
	return 0;
}

static int load_lobby_textures(void)
{
	unload_ui_textures();
	g_ui_data = sd_assets_load_lobby(g_sd_vol, &g_ui_size);
	if (!g_ui_data || g_ui_size < 64)
		return 4;

	TPL_OpenTPLFromMemory(&g_ui_tpl, g_ui_data, g_ui_size);
	TPL_GetTexture(&g_ui_tpl, 0, &texLobby);
	TPL_GetTexture(&g_ui_tpl, 1, &texCreate);
	TPL_GetTexture(&g_ui_tpl, 2, &texPadIcons); /* gc_pad_icons */
	return 0;
}

static int load_stsel_textures(void)
{
	unload_ui_textures();
	g_ui_data = sd_assets_load_stsel(g_sd_vol, &g_ui_size);
	if (!g_ui_data || g_ui_size < 64)
		return 5;

	TPL_OpenTPLFromMemory(&g_ui_tpl, g_ui_data, g_ui_size);
	TPL_GetTexture(&g_ui_tpl, 0, &texStsel);
	TPL_GetTexture(&g_ui_tpl, 1, &texStselChip);
	TPL_GetTexture(&g_ui_tpl, 2, &texPadIcons); /* gc_pad_icons */
	return 0;
}

static int load_stage_textures(void)
{
	unload_stage_textures();
	g_tpl_data = sd_assets_load_stage01(g_sd_vol, &g_tpl_size);
	if (!g_tpl_data || g_tpl_size < 64)
		return 2;

	TPL_OpenTPLFromMemory(&g_tpl, g_tpl_data, g_tpl_size);

	TPL_GetTexture(&g_tpl, st1_a0, &texBgPlate[0]);
	TPL_GetTexture(&g_tpl, st1_a1, &texBgPlate[1]);
	TPL_GetTexture(&g_tpl, st1_a2_far, &texBgPlate[2]);
	TPL_GetTexture(&g_tpl, st1_a3, &texBgPlate[3]);
	TPL_GetTexture(&g_tpl, st1_a4, &texBgPlate[4]);
	TPL_GetTexture(&g_tpl, st1_a2_mid, &texA2Mid);
	TPL_GetTexture(&g_tpl, st1_a2_near, &texA2Near);
	TPL_GetTexture(&g_tpl, st1_a5, &texA5);

	TPL_GetTexture(&g_tpl, poring_sheet, &texPoring[0]);
	TPL_GetTexture(&g_tpl, equrips_sheet, &texEqurips[0]);
	TPL_GetTexture(&g_tpl, rocker_sheet, &texRocker[0]);
	TPL_GetTexture(&g_tpl, creamy_s0, &texCreamy[0]);
	TPL_GetTexture(&g_tpl, creamy_s1, &texCreamy[1]);
	TPL_GetTexture(&g_tpl, spore_s0, &texSpore[0]);
	TPL_GetTexture(&g_tpl, spore_s1, &texSpore[1]);
	TPL_GetTexture(&g_tpl, fabre_sheet, &texFabre[0]);
	TPL_GetTexture(&g_tpl, lunatic_sheet, &texLunatic[0]);
	TPL_GetTexture(&g_tpl, willow_sheet, &texWillow[0]);
	TPL_GetTexture(&g_tpl, chon_chon_sheet, &texChonChon[0]);
	TPL_GetTexture(&g_tpl, novice_m_s0, &texNovice[0]);
	TPL_GetTexture(&g_tpl, novice_m_s1, &texNovice[1]);
	TPL_GetTexture(&g_tpl, system_atlas, &texSystem);
	TPL_GetTexture(&g_tpl, npc_all_sheet, &texNpc[0]);
	return 0;
}

static void drawFullscreenTex(GXTexObj *tex)
{
	/* Proven FIFO path lives in rbo_gx_blit_fullscreen. */
	rbo_gx_blit_fullscreen(tex);
}

static void drawUiBlit(GXTexObj *tex, const UiBlit *b, f32 sheet_w, f32 sheet_h)
{
	if (!tex || !b)
		return;
	rbo_gx_blit_src(tex, b->dx, b->dy, b->dw, b->dh,
			b->sx, b->sy, b->sw, b->sh, sheet_w, sheet_h);
}

static void drawPadIcon(int id, f32 x, f32 y, f32 scale)
{
	const UiBlit *src;
	f32 dw, dh;

	if (id < 0 || id >= RBO_PAD_ICON_COUNT || scale <= 0.0f)
		return;
	src = &k_pad_icons[id];
	dw = src->sw * scale;
	dh = src->sh * scale;
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	rbo_gx_blit_src(&texPadIcons, x, y, dw, dh,
			src->sx, src->sy, src->sw, src->sh,
			(f32)GC_PAD_SHEET_W, (f32)GC_PAD_SHEET_H);
}

/* Icon + label; returns x after the prompt (for chaining). Uses small icons. */
static f32 drawPadPrompt(int id, f32 x, f32 y, const char *label, f32 text_px)
{
	int sm = rbo_pad_icon_small(id);
	const UiBlit *src;
	f32 scale, iw, ih, text_h;

	if (sm < 0 || sm >= RBO_PAD_ICON_COUNT)
		sm = id;
	src = &k_pad_icons[sm];
	text_h = text_px * 8.0f;
	scale = text_h / src->sh;
	if (scale > 1.2f)
		scale = 1.2f;
	if (scale < 0.6f)
		scale = 0.6f;
	iw = src->sw * scale;
	ih = src->sh * scale;
	drawPadIcon(sm, x, y + (text_h - ih) * 0.5f, scale);

	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	if (label && label[0]) {
		drawHudText(x + iw + 6.0f, y, label, text_px);
		return x + iw + 6.0f + (f32)strlen(label) * text_px * 6.0f + 16.0f;
	}
	return x + iw + 8.0f;
}

static void drawTitleAtlas(void)
{
	drawUiBlit(&texTitleAtlas, &k_title_logo, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
	drawUiBlit(&texTitleAtlas, &k_title_banner, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
}

static void drawBootLogoVariant(int variant)
{
	if (variant < 0 || variant >= BOOT_LOGO_COUNT)
		variant = 0;
	drawUiBlit(&texBootLogos, &k_boot_logos[variant],
		   (f32)BOOT_LOGO_SHEET, (f32)BOOT_LOGO_SHEET);
}

static void drawLobbyAtlas(void)
{
	drawUiBlit(&texLobby, &k_lobby_banner, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
}

static void drawStselAtlas(void)
{
	drawUiBlit(&texStsel, &k_stsel_banner, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
	drawUiBlit(&texStsel, &k_stsel_hbar, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
	drawUiBlit(&texStsel, &k_stsel_hbar_bot, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
	drawUiBlit(&texStselChip, &k_stsel_chip, 512.0f, 512.0f);
}

/* Bottom-left skip hint for caution / logos / intro (GC A glyph). */
static void drawPressAHud(void)
{
	Mtx identity;
	f32 x = 24.0f;
	f32 y = 448.0f;

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 230, 120, 255});
	drawHudText(x, y, "PRESS", 2.0f);
	drawPadPrompt(RBO_PAD_ICON_A, x + 5.0f * 2.0f * 6.0f + 6.0f, y, NULL, 2.0f);
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

static void drawTitleMenu(void)
{
	Mtx identity;
	int blink = (g_menu_flash / 12) & 1;
	f32 y0 = TITLE_MENU_START_Y;
	f32 row = TITLE_MENU_ROW_H;

	/* Ranking / Exit from Title.Img; Start Game is Eng text (empty sprite). */
	drawUiBlit(&texTitleAtlas, &k_title_menu_ranking,
		   (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);
	drawUiBlit(&texTitleAtlas, &k_title_menu_exit,
		   (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

	/* Selection bar behind active row. */
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){40, 70, 140, 160});
	drawSolidQuad(TITLE_MENU_START_X - 8.0f,
		      y0 + (f32)g_menu_sel * row - 2.0f, 224.0f, 28.0f);

	GX_SetChanMatColor(GX_COLOR0A0,
			   (g_menu_sel == MENU_START && blink)
				   ? (GXColor){255, 240, 120, 255}
				   : (GXColor){240, 240, 255, 255});
	drawHudText(TITLE_MENU_START_X + 8.0f, y0 + 6.0f, "START GAME", 2.0f);

	if (g_ranking_flash > 0) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 200});
		drawSolidQuad(160.0f, 200.0f, 320.0f, 40.0f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 230, 120, 255});
		drawHudText(190.0f, 212.0f, "RANKING STUB", 2.0f);
	}

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){160, 170, 190, 255});
	drawPadPrompt(RBO_PAD_ICON_A, 44.0f, y0 + row * 3.0f + 12.0f, "SELECT", 1.5f);

	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

static void drawPanelTextStart(void)
{
	Mtx identity;

	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetNumTexGens(0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
}

static void drawPanelTextEnd(void)
{
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
}

static void drawLoadHud(void)
{
	drawPanelTextStart();
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 255});
	drawSolidQuad(0.0f, 0.0f, (f32)SCREEN_W, (f32)SCREEN_H);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 230, 120, 255});
	drawHudText(200.0f, 220.0f, "LOADING DATA", 3.0f);
	drawPanelTextEnd();
}

static void drawLobbyHud(void)
{
	char line[40];
	int i;
	int blink = (g_menu_flash / 12) & 1;
	static const char *depart_labels[DEPART_COUNT] = {
		"START ADVENTURE",
		"STATUS",
		"EXIT"
	};

	drawPanelTextStart();
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 170});
	drawSolidQuad(24.0f, 340.0f, 592.0f, 120.0f);

	if (g_lobby_mode == LOBBY_GREET) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawHudText(40.0f, 360.0f, "WELCOME  HOLD A TO JOIN", 2.0f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawHudText(40.0f, 400.0f, "PRESS", 2.0f);
		drawPadPrompt(RBO_PAD_ICON_A, 40.0f + 5.0f * 2.0f * 6.0f + 4.0f,
			      400.0f, NULL, 2.0f);
	} else if (g_lobby_mode == LOBBY_SLOTS) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawHudText(40.0f, 350.0f, "SELECT CHARACTER SLOT", 2.0f);
		for (i = 0; i < SAVE_SLOTS; i++) {
			f32 y = 380.0f + (f32)i * 22.0f;
			if (g_save.occupied[i])
				snprintf(line, sizeof(line), "SLOT %d  %s LV%d",
					 i + 1, g_save.name[i], g_save.level[i]);
			else
				snprintf(line, sizeof(line), "SLOT %d  EMPTY", i + 1);
			if (i == g_slot_sel) {
				GX_SetChanMatColor(GX_COLOR0A0,
						   blink ? (GXColor){255, 240, 120, 255}
							 : (GXColor){255, 200, 80, 255});
				drawHudText(40.0f, y, ">", 2.0f);
				drawHudText(64.0f, y, line, 2.0f);
			} else {
				GX_SetChanMatColor(GX_COLOR0A0, (GXColor){200, 200, 210, 255});
				drawHudText(64.0f, y, line, 2.0f);
			}
		}
	} else if (g_lobby_mode == LOBBY_CREATE) {
		f32 px;
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawHudText(40.0f, 360.0f, "NEW GAME  NOVICE LV1", 2.0f);
		px = drawPadPrompt(RBO_PAD_ICON_A, 40.0f, 400.0f, "CREATE", 2.0f);
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawPadPrompt(RBO_PAD_ICON_B, px, 400.0f, "BACK", 2.0f);
	} else if (g_lobby_mode == LOBBY_READY) {
		f32 px;
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		snprintf(line, sizeof(line), "READY  %s LV%d",
			 g_save.name[g_active_slot], g_save.level[g_active_slot]);
		drawHudText(40.0f, 360.0f, line, 2.0f);
		GX_SetChanMatColor(GX_COLOR0A0,
				   blink ? (GXColor){255, 240, 120, 255}
					 : (GXColor){200, 200, 210, 255});
		px = drawPadPrompt(RBO_PAD_ICON_A, 40.0f, 400.0f, "YES", 2.0f);
		GX_SetChanMatColor(GX_COLOR0A0,
				   blink ? (GXColor){255, 240, 120, 255}
					 : (GXColor){200, 200, 210, 255});
		drawPadPrompt(RBO_PAD_ICON_B, px, 400.0f, "NO", 2.0f);
	} else if (g_lobby_mode == LOBBY_DEPART) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
		drawHudText(40.0f, 350.0f, "DEPART MENU", 2.0f);
		for (i = 0; i < DEPART_COUNT; i++) {
			f32 y = 380.0f + (f32)i * 22.0f;
			if (i == g_depart_sel) {
				GX_SetChanMatColor(GX_COLOR0A0,
						   blink ? (GXColor){255, 240, 120, 255}
							 : (GXColor){255, 200, 80, 255});
				drawHudText(40.0f, y, ">", 2.0f);
				drawHudText(64.0f, y, depart_labels[i], 2.0f);
			} else {
				GX_SetChanMatColor(GX_COLOR0A0, (GXColor){200, 200, 210, 255});
				drawHudText(64.0f, y, depart_labels[i], 2.0f);
			}
		}
	}
	drawPanelTextEnd();
}

static void drawStselHud(void)
{
	drawPanelTextStart();
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 160});
	drawSolidQuad(360.0f, 40.0f, 260.0f, 200.0f);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 200, 80, 255});
	drawHudText(376.0f, 56.0f, "STAGE SELECT", 2.0f);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
	drawHudText(376.0f, 88.0f, "PRONTERA PLAINS", 1.6f);

	/* Path picker — LEAN keeps mined rail; FULL mallocs STAGE01.FOB. */
	{
		int i;
		static const char *labels[PLAY_MODE_COUNT] = {
			"STAGE 1 LEAN",
			"STAGE 1 FULL"
		};
		for (i = 0; i < PLAY_MODE_COUNT; i++) {
			f32 y = 118.0f + (f32)i * 24.0f;
			int sel = (i == g_stsel_sel);
			if (sel) {
				GX_SetChanMatColor(GX_COLOR0A0,
						   (GXColor){40, 70, 140, 180});
				drawSolidQuad(372.0f, y - 2.0f, 230.0f, 22.0f);
				GX_SetChanMatColor(GX_COLOR0A0,
						   (GXColor){255, 255, 255, 255});
				drawHudText(380.0f, y, ">", 1.8f);
				drawHudText(400.0f, y, labels[i], 1.7f);
			} else {
				GX_SetChanMatColor(GX_COLOR0A0,
						   (GXColor){180, 190, 210, 255});
				drawHudText(400.0f, y, labels[i], 1.7f);
			}
		}
	}

	if (g_fob_err_flash > 0) {
		GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 80, 80, 255});
		drawHudText(376.0f, 172.0f, "FOB LOAD FAIL", 1.5f);
	}

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
	drawPadPrompt(RBO_PAD_ICON_A, 376.0f, 196.0f, "START", 1.8f);
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){240, 240, 255, 255});
	drawPadPrompt(RBO_PAD_ICON_B, 500.0f, 196.0f, "BACK", 1.8f);
	drawPanelTextEnd();
}

static const char *hud_class_name(u8 class_id)
{
	(void)class_id;
	return "NOVICE";
}

static void hud_fill_slot(RboHudSlot *dst, int save_i)
{
	if (!dst || save_i < 0 || save_i >= SAVE_SLOTS)
		return;
	dst->occupied = g_save.occupied[save_i] ? 1 : 0;
	dst->name = g_save.name[save_i];
	dst->class_name = hud_class_name(g_save.class_id[save_i]);
	dst->level = g_save.level[save_i];
	dst->hp = g_save.hp[save_i];
	dst->hp_max = g_save.hp_max[save_i];
	dst->sp = g_save.sp[save_i];
	dst->sp_max = g_save.sp_max[save_i];
}

static int scenery_culled(const Actor *a, f32 cam_x)
{
	f32 dx;

	if (!a)
		return 1;
	dx = a->x - (cam_x + (f32)SCREEN_W * 0.5f);
	if (dx < 0.0f)
		dx = -dx;
	return dx > (f32)BG_W;
}

static f32 actor_sort_y(int id)
{
	if (id < 0)
		return novice.y;
	if (id < NUM_ENEMIES)
		return enemies[id].base.y;
	return scenery[id - NUM_ENEMIES].base.y;
}

static void drawPlayHud(f32 cam_x)
{
	RboHudView v;
	RboHudBlip blips[RBO_HUD_BLIP_MAX];
	char note[48];
	char note2[48];
	int i, n = 0, extra;
	u32 used, pct;

	memset(&v, 0, sizeof(v));
	if (g_save.occupied[g_active_slot])
		hud_fill_slot(&v.slot[1], g_active_slot);
	extra = 0;
	for (i = 0; i < SAVE_SLOTS; i++) {
		int dst;

		if (!g_save.occupied[i] || i == g_active_slot)
			continue;
		dst = (extra == 0) ? 0 : 2;
		hud_fill_slot(&v.slot[dst], i);
		extra++;
		if (extra >= 2)
			break;
	}

	v.play_timer = g_play_timer;
	v.area = g_section + 1;
	v.stage_clear = g_stage_clear;
	v.novice_sx = novice.x - cam_x;
	v.novice_sy = novice.y;
	v.cam_x = cam_x;
	v.lock_lo = g_lock_lo;
	v.lock_hi = g_lock_hi;

	if (n < RBO_HUD_BLIP_MAX) {
		blips[n].world_x = novice.x;
		blips[n].kind = RBO_HUD_BLIP_PLAYER;
		n++;
	}
	for (i = 0; i < NUM_ENEMIES && n < RBO_HUD_BLIP_MAX; i++) {
		if (!enemies[i].alive)
			continue;
		blips[n].world_x = enemies[i].base.x;
		blips[n].kind = RBO_HUD_BLIP_ENEMY;
		n++;
	}
	for (i = 0; i < NPC_SCENERY_MAX && n < RBO_HUD_BLIP_MAX; i++) {
		if (!scenery[i].alive)
			continue;
		blips[n].world_x = scenery[i].base.x;
		blips[n].kind = RBO_HUD_BLIP_NPC;
		n++;
	}
	v.blips = blips;
	v.n_blips = n;

	sample_mem();
	used = (g_arena_free_now < MEM1_BUDGET) ? (MEM1_BUDGET - g_arena_free_now) : 0;
	pct = (used * 100u) / MEM1_BUDGET;
	v.mem_pct = pct;
	v.mem_free_k = g_arena_free_now / 1024u;

	note[0] = 0;
	note2[0] = 0;
	if (g_play_mode == PLAY_FULL && g_fob_loaded) {
		s32 phase = rbo_fob_read_slot(&g_fob, FOB_PHASE_SLOT, -1);

		snprintf(note, sizeof(note), "FULL PH %d I %d H %u",
			 (int)phase, g_fob_init_rc, g_fob.host_hits);
		snprintf(note2, sizeof(note2), "S %u D %u P %u B %u",
			 rbo_spawn_emit_count(), rbo_spawn_drop_count(),
			 rbo_spawn_place_count(), g_fob.d0_hits);
		v.note = note;
		v.note2 = note2;
	}

	rbo_hud_draw(&v, &texSystem, drawPanelTextStart, drawHudText,
		     drawPanelTextEnd);
}

/* PC pause second screen (FUN_004748f0 / FUN_00474d40) — no overlay/confirm. */
static void drawPauseMenu(void)
{
	static const char *labels[PAUSE_COUNT] = {
		"RETRY RETURN LOBBY",
		"RETURN TITLE",
		"EXIT GAME"
	};
	int i;
	f32 y0 = 188.0f;
	f32 row = 28.0f;

	/* Soft dim over frozen stage. */
	drawPanelTextStart();
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 16, 140});
	drawSolidQuad(0.0f, 0.0f, (f32)SCREEN_W, (f32)SCREEN_H);
	drawPanelTextEnd();

	/* SYSTEM pause plate (relocated toward menu center). */
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	drawUiBlit(&texSystem, &k_sys_pause_panel, (f32)UI_ATLAS_W, (f32)UI_ATLAS_H);

	drawPanelTextStart();
	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 200});
	drawSolidQuad(160.0f, 148.0f, 320.0f, 160.0f);

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){120, 180, 255, 255});
	drawHudText(232.0f, 158.0f, "PAUSE MENU", 2.2f);

	for (i = 0; i < PAUSE_COUNT; i++) {
		f32 y = y0 + (f32)i * row;
		int sel = (i == g_pause_sel);
		if (sel) {
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){40, 70, 140, 180});
			drawSolidQuad(176.0f, y - 4.0f, 288.0f, 24.0f);
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 255, 255, 255});
			drawHudText(184.0f, y, ">", 2.0f);
			drawHudText(208.0f, y, labels[i], 2.0f);
		} else {
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){200, 210, 230, 255});
			drawHudText(208.0f, y, labels[i], 2.0f);
		}
	}

	GX_SetChanMatColor(GX_COLOR0A0, (GXColor){160, 170, 190, 255});
	/* STAGE01 has no pad-icon sheet — glyph prompts only. */
	drawHudText(184.0f, y0 + row * 3.0f + 8.0f, "A SELECT  B BACK", 1.5f);
	drawPanelTextEnd();
}

static void play_clear_combat(void)
{
	g_atk_timer = 0;
	g_atk_hit = 0;
	g_hit_flash_t = 0;
	g_paused = 0;
	g_pause_sel = PAUSE_LOBBY;
	g_section = 0;
	g_stage_clear = 0;
	g_stage = NULL;
	clear_enemy_pool();
	clear_scenery_pool();
	rbo_spawn_set_emit(NULL, NULL);
	if (g_fob_loaded) {
		rbo_fob_unload(&g_fob);
		g_fob_loaded = 0;
	}
	g_fob_init_rc = 0;
	g_fob_frame_rc = 0;
	g_play_mode = PLAY_LEAN;
}

/* FULL path only: malloc FOB after STAGE01.TPL. Soft-fail → stay lean. */
static int play_load_full_fob(void)
{
	int rc;

	if (g_fob_loaded) {
		rbo_fob_unload(&g_fob);
		g_fob_loaded = 0;
	}
	rc = rbo_fob_load(&g_fob, g_sd_vol, FOB_STAGE01_PATH);
	if (rc != 0)
		return rc;
	g_fob_loaded = 1;
	rbo_spawn_set_emit(fob_on_spawn, NULL);
	(void)rbo_fob_run_export(&g_fob, "SetLoadCharaList", FOB_INIT_BUDGET);
	g_fob_init_rc = rbo_fob_run_export(&g_fob, "StageInit", FOB_INIT_BUDGET);
	return 0;
}

static void play_tick_full_fob(void)
{
	if (!g_fob_loaded || g_paused)
		return;
	g_fob.have_lock = 0;
	g_fob_frame_rc = rbo_fob_run_export(&g_fob, "StageMainCallback",
					    FOB_FRAME_BUDGET);
	if (g_fob.have_lock)
		apply_gate_lock(g_fob.lock_lo, g_fob.lock_hi);
}

static void fatal_sd_loop(int err, u32 *fb)
{
	char l0[40], l1[40], l2[40], l3[40], l4[40];

	snprintf(l0, sizeof(l0), "SD ERR %d", err);
	snprintf(l1, sizeof(l1), "%s", sd_assets_last_diag());
	snprintf(l2, sizeof(l2), "%s", rbo_dvd_last_diag());
	snprintf(l3, sizeof(l3), "MAGIC %08X", rbo_dvd_last_magic());
	snprintf(l4, sizeof(l4), "FST %X %d",
		 rbo_dvd_last_fst_off(), (int)rbo_dvd_last_fst_len());
	/* VAT + ortho must be live here — this can run before main's 2D setup. */
	rbo_gx_apply_vtxfmt0();
	rbo_gx_set_ui_ortho();
	while (SYS_MainLoop()) {
		escape_pad_sample();
		if (escape_requested() || (escape_pad_held(0) & PAD_BUTTON_START))
			escape_exit();

		GX_InvVtxCache();
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		GX_SetCopyClear((GXColor){80, 16, 16, 255}, 0x00ffffff);

		{
			Mtx identity;
			guMtxIdentity(identity);
			GX_LoadPosMtxImm(identity, GX_PNMTX0);
			GX_SetNumChans(1);
			GX_SetNumTexGens(0);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
			GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
			GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
				       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){12, 12, 18, 220});
			drawSolidQuad(6.0f, 4.0f, 628.0f, 118.0f);
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 80, 80, 255});
			drawHudText(10.0f, 8.0f, l0, 2.0f);
			GX_SetChanMatColor(GX_COLOR0A0, (GXColor){255, 200, 160, 255});
			drawHudText(10.0f, 28.0f, l1, 1.5f);
			drawHudText(10.0f, 46.0f, l2, 1.5f);
			drawHudText(10.0f, 64.0f, l3, 1.5f);
			drawHudText(10.0f, 82.0f, l4, 1.5f);
			GX_SetNumTexGens(1);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
			GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
		}

		GX_DrawDone();
		GX_CopyDisp(frameBuffer[*fb], GX_TRUE);
		VIDEO_SetNextFramebuffer(frameBuffer[*fb]);
		VIDEO_SetBlack(FALSE);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		*fb ^= 1;
	}
}

int main(int argc, char **argv)
{
	u32 fb = 0;
	u32 first_frame = 1;
	f32 yscale;
	u32 xfbHeight;
	void *gp_fifo = NULL;
	GXColor background = {32, 48, 80, 0xff};
	f32 cam_x = 0.0f;
	f32 ground_y = FLOOR_Y;
	int i;
	int load_err;

	(void)argc;
	(void)argv;

	VIDEO_Init();
	PAD_Init();
	escape_install();

	rmode = VIDEO_GetPreferredMode(NULL);
	frameBuffer[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	frameBuffer[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(frameBuffer[fb]);
	/* Stay black until the first GX_CopyDisp — an uncleared XFB is green in YUV. */
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	gp_fifo = memalign(32, DEFAULT_FIFO_SIZE);
	if (!gp_fifo) {
		while (SYS_MainLoop())
			VIDEO_WaitVSync();
	}
	memset(gp_fifo, 0, DEFAULT_FIFO_SIZE);
	GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);
	rbo_gx_init(rmode, gp_fifo, DEFAULT_FIFO_SIZE);
	rbo_gx_set_aspect(RBO_ASPECT_4_3);

	GX_SetCopyClear(background, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
			((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(frameBuffer[fb], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);
	VIDEO_SetNextFramebuffer(frameBuffer[fb]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	fb ^= 1;

	rbo_platform_init();
	sd_assets_set_load_poll(rbo_audio_poll);

	load_err = load_title_textures();
	if (load_err)
		fatal_sd_loop(load_err, &fb);
	rbo_audio_init();
	if (g_sd_vol)
		rbo_audio_set_vol(g_sd_vol);
	save_load_from_sd();

	rbo_gx_apply_vtxfmt0();
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_InvalidateTexAll();

	sample_mem();
	{
		Mtx44 perspective;
		guOrtho(perspective, 0, 479, 0, 639, 0, 300);
		GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);
	}

	scene_goto(SCENE_CAUTION);
	g_scene_timer = CAUTION_FRAMES;
	g_stage_ready = 0;
	g_menu_sel = MENU_START;
	g_menu_flash = 0;
	g_lobby_mode = LOBBY_GREET;
	g_slot_sel = 0;
	g_depart_sel = DEPART_ADVENTURE;
	g_ranking_flash = 0;
	g_play_timer = 0;

	while (SYS_MainLoop()) {
		u16 down, held;
		CharDef *cd;

		/* DI-like once-per-frame poll (all pads). */
		rbo_input_poll();
		rbo_audio_poll();
		held = rbo_input_pad(0)->held;
		down = rbo_input_pad(0)->pressed;

		/* RESET always Swiss. START→Swiss outside PLAY; PLAY opens pause. */
		if (escape_reset_held())
			quit_to_swiss();
		if (g_scene != SCENE_PLAY && rbo_input_pressed(PAD_BUTTON_START))
			quit_to_swiss();

		g_menu_flash++;
		if (g_ranking_flash > 0)
			g_ranking_flash--;

		if (g_scene == SCENE_CAUTION) {
			g_scene_timer--;
			if ((down & PAD_BUTTON_A) || (down & PAD_BUTTON_START) ||
			    g_scene_timer <= 0) {
				scene_goto(SCENE_LOGO);
				g_logo_variant = 0;
				g_scene_timer = LOGO_FRAMES;
			}
		} else if (g_scene == SCENE_LOGO) {
			g_scene_timer--;
			if ((down & PAD_BUTTON_A) || (down & PAD_BUTTON_START) ||
			    g_scene_timer <= 0) {
				g_logo_variant++;
				if (g_logo_variant >= BOOT_LOGO_COUNT) {
					scene_goto(SCENE_INTRO);
					g_scene_timer = INTRO_FRAMES;
				} else {
					g_scene_timer = LOGO_FRAMES;
				}
			}
		} else if (g_scene == SCENE_INTRO) {
			g_scene_timer--;
			if ((down & PAD_BUTTON_A) || (down & PAD_BUTTON_START) ||
			    (down & PAD_BUTTON_B) || g_scene_timer <= 0) {
				scene_goto(SCENE_TITLE);
				g_menu_sel = MENU_START;
			}
		} else if (g_scene == SCENE_TITLE) {
			if (down & PAD_BUTTON_UP) {
				g_menu_sel--;
				if (g_menu_sel < 0)
					g_menu_sel = MENU_COUNT - 1;
			}
			if (down & PAD_BUTTON_DOWN) {
				g_menu_sel++;
				if (g_menu_sel >= MENU_COUNT)
					g_menu_sel = 0;
			}
			if (down & PAD_BUTTON_A) {
				if (g_menu_sel == MENU_START) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					scene_goto(SCENE_LOAD);
					g_scene_timer = LOAD_FRAMES;
				} else if (g_menu_sel == MENU_RANKING) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					g_ranking_flash = 90;
				} else if (g_menu_sel == MENU_EXIT) {
					quit_to_swiss();
				}
			}
		} else if (g_scene == SCENE_LOAD) {
			g_scene_timer--;
			if (g_scene_timer <= 0) {
				unload_ui_textures();
				load_err = load_lobby_textures();
				if (load_err)
					fatal_sd_loop(load_err, &fb);
				g_lobby_mode = LOBBY_GREET;
				scene_goto(SCENE_LOBBY);
				GX_InvalidateTexAll();
			}
		} else if (g_scene == SCENE_LOBBY) {
			if (g_lobby_mode == LOBBY_GREET) {
				/* Advance when any pad presses A (multi-pad join). */
				if (rbo_input_any_pressed(PAD_BUTTON_A)) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					g_lobby_mode = LOBBY_SLOTS;
				}
			} else if (g_lobby_mode == LOBBY_SLOTS) {
				if (down & PAD_BUTTON_UP) {
					g_slot_sel--;
					if (g_slot_sel < 0)
						g_slot_sel = SAVE_SLOTS - 1;
				}
				if (down & PAD_BUTTON_DOWN) {
					g_slot_sel++;
					if (g_slot_sel >= SAVE_SLOTS)
						g_slot_sel = 0;
				}
				if (down & PAD_BUTTON_A) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					if (g_save.occupied[g_slot_sel]) {
						g_active_slot = g_slot_sel;
						g_lobby_mode = LOBBY_READY;
					} else {
						g_lobby_mode = LOBBY_CREATE;
					}
				}
				if (down & PAD_BUTTON_B) {
					rbo_audio_play_se(RBO_SE_CANCEL);
					unload_ui_textures();
					load_err = load_title_textures();
					if (load_err)
						fatal_sd_loop(load_err, &fb);
					scene_goto(SCENE_TITLE);
					GX_InvalidateTexAll();
				}
			} else if (g_lobby_mode == LOBBY_CREATE) {
				if (down & PAD_BUTTON_A) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					if (!g_save.occupied[g_slot_sel]) {
						stamp_new_character(g_slot_sel);
						g_lobby_mode = LOBBY_READY;
					} else {
						/* Viewing status sheet — back to depart */
						g_lobby_mode = LOBBY_DEPART;
					}
				}
				if (down & PAD_BUTTON_B) {
					rbo_audio_play_se(RBO_SE_CANCEL);
					if (g_save.occupied[g_slot_sel])
						g_lobby_mode = LOBBY_DEPART;
					else
						g_lobby_mode = LOBBY_SLOTS;
				}
			} else if (g_lobby_mode == LOBBY_READY) {
				if (down & PAD_BUTTON_A) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					g_lobby_mode = LOBBY_DEPART;
				}
				if (down & PAD_BUTTON_B) {
					rbo_audio_play_se(RBO_SE_CANCEL);
					g_lobby_mode = LOBBY_SLOTS;
				}
			} else if (g_lobby_mode == LOBBY_DEPART) {
				if (down & PAD_BUTTON_UP) {
					g_depart_sel--;
					if (g_depart_sel < 0)
						g_depart_sel = DEPART_COUNT - 1;
				}
				if (down & PAD_BUTTON_DOWN) {
					g_depart_sel++;
					if (g_depart_sel >= DEPART_COUNT)
						g_depart_sel = 0;
				}
				if (down & PAD_BUTTON_A) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					if (g_depart_sel == DEPART_ADVENTURE) {
						unload_ui_textures();
						load_err = load_stsel_textures();
						if (load_err)
							fatal_sd_loop(load_err, &fb);
						scene_goto(SCENE_STSEL);
						GX_InvalidateTexAll();
					} else if (g_depart_sel == DEPART_STATUS) {
						/* Status uses create plate as char sheet stand-in */
						g_lobby_mode = LOBBY_CREATE;
						g_menu_flash = 0;
					} else if (g_depart_sel == DEPART_EXIT) {
						unload_ui_textures();
						load_err = load_title_textures();
						if (load_err)
							fatal_sd_loop(load_err, &fb);
						scene_goto(SCENE_TITLE);
						GX_InvalidateTexAll();
					}
				}
				if (down & PAD_BUTTON_B) {
					rbo_audio_play_se(RBO_SE_CANCEL);
					g_lobby_mode = LOBBY_READY;
				}
			}
		} else if (g_scene == SCENE_STSEL) {
			if (g_fob_err_flash > 0)
				g_fob_err_flash--;
			if (down & PAD_BUTTON_UP) {
				g_stsel_sel--;
				if (g_stsel_sel < 0)
					g_stsel_sel = PLAY_MODE_COUNT - 1;
			}
			if (down & PAD_BUTTON_DOWN) {
				g_stsel_sel++;
				if (g_stsel_sel >= PLAY_MODE_COUNT)
					g_stsel_sel = 0;
			}
			if (down & PAD_BUTTON_A) {
				rbo_audio_play_se(RBO_SE_CONFIRM);
				unload_ui_textures();
				/* BGM before STAGE01 — avoids soft-fail when MEM1 is tight. */
				rbo_audio_play_bgm("RBO/SOUND/bgm/stage01.ogg", 1);
				load_err = load_stage_textures();
				if (load_err)
					fatal_sd_loop(load_err, &fb);
				g_play_mode = g_stsel_sel;
				if (g_play_mode == PLAY_FULL) {
					init_char_defs();
					g_ground_y = ground_y;
					load_err = play_load_full_fob();
					if (load_err) {
						/* Stay out of PLAY — unload stage, show error. */
						unload_stage_textures();
						load_err = load_stsel_textures();
						if (load_err)
							fatal_sd_loop(load_err, &fb);
						g_fob_err_flash = 180;
						g_play_mode = PLAY_LEAN;
						scene_goto(SCENE_STSEL);
						GX_InvalidateTexAll();
					} else {
						init_actors(ground_y);
						g_stage_ready = 1;
						g_play_timer = 0;
						g_paused = 0;
						g_pause_sel = PAUSE_LOBBY;
						scene_goto(SCENE_PLAY);
						GX_InvalidateTexAll();
					}
				} else {
					init_char_defs();
					init_actors(ground_y);
					g_stage_ready = 1;
					g_play_timer = 0;
					g_paused = 0;
					g_pause_sel = PAUSE_LOBBY;
					scene_goto(SCENE_PLAY);
					GX_InvalidateTexAll();
				}
			}
			if (down & PAD_BUTTON_B) {
				rbo_audio_play_se(RBO_SE_CANCEL);
				unload_ui_textures();
				load_err = load_lobby_textures();
				if (load_err)
					fatal_sd_loop(load_err, &fb);
				g_lobby_mode = LOBBY_DEPART;
				scene_goto(SCENE_LOBBY);
				GX_InvalidateTexAll();
			}
		} else if (g_scene == SCENE_PLAY) {
			/*
			 * Pause: PC FUN_0043a850 — skip overlay (cases 0–2), jump to
			 * 3-item menu (FUN_004748f0). Confirm dialogs omitted.
			 * Returns: 2 lobby, 3 title (FUN_004087a0), 4 exit.
			 */
			if (g_paused) {
				if ((down & PAD_BUTTON_START) || (down & PAD_BUTTON_B)) {
					rbo_audio_play_se(RBO_SE_CANCEL);
					g_paused = 0;
				} else if (down & PAD_BUTTON_UP) {
					g_pause_sel--;
					if (g_pause_sel < 0)
						g_pause_sel = PAUSE_COUNT - 1;
				} else if (down & PAD_BUTTON_DOWN) {
					g_pause_sel++;
					if (g_pause_sel >= PAUSE_COUNT)
						g_pause_sel = 0;
				} else if (down & PAD_BUTTON_A) {
					rbo_audio_play_se(RBO_SE_CONFIRM);
					if (g_pause_sel == PAUSE_EXIT) {
						quit_to_swiss();
					} else if (g_pause_sel == PAUSE_LOBBY) {
						unload_stage_textures();
						load_err = load_lobby_textures();
						if (load_err)
							fatal_sd_loop(load_err, &fb);
						play_clear_combat();
						g_lobby_mode = LOBBY_DEPART;
						scene_goto(SCENE_LOBBY);
						GX_InvalidateTexAll();
					} else if (g_pause_sel == PAUSE_TITLE) {
						/* PC case 3: party reset stub + title mode. */
						unload_stage_textures();
						load_err = load_title_textures();
						if (load_err)
							fatal_sd_loop(load_err, &fb);
						play_clear_combat();
						g_menu_sel = MENU_START;
						scene_goto(SCENE_TITLE);
						GX_InvalidateTexAll();
					}
				}
			} else if (down & PAD_BUTTON_START) {
				rbo_audio_play_se(RBO_SE_CONFIRM);
				g_paused = 1;
				g_pause_sel = PAUSE_LOBBY;
			} else if (down & PAD_BUTTON_B) {
				/* Quick hub exit (unload STAGE01 → STSEL). */
				rbo_audio_play_se(RBO_SE_CANCEL);
				unload_stage_textures();
				load_err = load_stsel_textures();
				if (load_err)
					fatal_sd_loop(load_err, &fb);
				play_clear_combat();
				scene_goto(SCENE_STSEL);
				GX_InvalidateTexAll();
			} else if ((down & PAD_BUTTON_A) && g_atk_timer <= 0) {
				g_atk_timer = ATK_TOTAL;
				g_atk_hit = 0;
				novice.manual_pose = 1;
				novice.moving = 0;
				novice.pose = novice_atk_a;
			} else if (down & PAD_BUTTON_Y) {
				if (g_char_scale < 1.25f)
					apply_char_scale(1.5f);
				else if (g_char_scale < 1.75f)
					apply_char_scale(2.0f);
				else
					apply_char_scale(CHAR_SCALE_DEFAULT);
			}

			if (g_scene != SCENE_PLAY) {
				/* Left for hub this frame — skip stage sim. */
			} else if (g_paused) {
				/* Freeze timer + actors while paused. */
			} else {
			g_play_timer++;
			if (g_play_mode == PLAY_FULL)
				play_tick_full_fob();
			if (g_atk_timer > 0) {
				int elapsed = ATK_TOTAL - g_atk_timer;

				novice.manual_pose = 1;
				novice.moving = 0;
				novice.pose = (elapsed < ATK_ACTIVE_START)
						     ? novice_atk_a
						     : novice_atk_b;
				try_attack_hits();
				g_atk_timer--;
				if (g_atk_timer <= 0) {
					novice.manual_pose = 0;
					novice.pose = novice_idle;
				}
				novice.y = g_ground_y;
				clamp_to_stage_x(&novice);
			} else {
				novice.moving = 0;
				if (held & PAD_BUTTON_LEFT) {
					novice.x -= 2.5f;
					novice.facing = -1.0f;
					novice.moving = 1;
					novice.manual_pose = 0;
				}
				if (held & PAD_BUTTON_RIGHT) {
					novice.x += 2.5f;
					novice.facing = 1.0f;
					novice.moving = 1;
					novice.manual_pose = 0;
				}
				novice.y = g_ground_y;
				update_walk(&novice, novice_walk_poses,
					    novice_walk_count, novice_idle);
				clamp_to_stage_x(&novice);
			}

			/* HOST_C.1-style section lock: clamp to lo..hi until clear. */
			if (novice.x > g_lock_hi)
				novice.x = g_lock_hi;
			if (novice.x < g_lock_lo)
				novice.x = g_lock_lo;

			for (i = 0; i < NUM_ENEMIES; i++) {
				Actor *a = &enemies[i].base;
				f32 dx, adx;
				cd = &g_chardefs[enemies[i].type];

				if (!enemies[i].alive)
					continue;
				if (enemies[i].hit_timer > 0) {
					enemies[i].hit_timer--;
					a->moving = 0;
					if (enemies[i].hit_timer <= 0)
						a->manual_pose = 0;
				} else {
					/* Thin StageEventCall AI: chase when novice close. */
					dx = novice.x - a->x;
					adx = (dx < 0.0f) ? -dx : dx;
					a->moving = 1;
					a->manual_pose = 0;
					if (adx < AGGRO_RANGE && adx > 8.0f) {
						a->facing = (dx > 0.0f) ? 1.0f : -1.0f;
						a->x += a->facing * AGGRO_SPEED;
					} else {
						a->x += a->facing *
							(0.55f + (f32)(i % 4) * 0.2f);
					}
					if (a->x < g_lock_lo + 20.0f) {
						a->x = g_lock_lo + 20.0f;
						a->facing = 1.0f;
					}
					if (a->x > g_lock_hi - 20.0f) {
						a->x = g_lock_hi - 20.0f;
						a->facing = -1.0f;
					}
				}
				clamp_to_stage_x(a);
				update_walk(a, cd->walk_poses, cd->walk_count,
					    cd->idle);
			}

			advance_section_if_clear();

			if (g_hit_flash_t > 0)
				g_hit_flash_t--;

			cam_x = novice.x - (f32)SCREEN_W * 0.5f;
			/* Follow the player; stop at the current hi gate only. */
			{
				f32 cam_hi = g_lock_hi - (f32)SCREEN_W;
				if (cam_x > cam_hi)
					cam_x = cam_hi;
			}
			if (cam_x < 0.0f)
				cam_x = 0.0f;
			if (cam_x > (f32)(STAGE_W - SCREEN_W))
				cam_x = (f32)(STAGE_W - SCREEN_W);
			}
		}

		GX_InvVtxCache();
		GX_InvalidateTexAll();
		GX_ClearVtxDesc();
		GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
		GX_SetAlphaUpdate(GX_TRUE);
		GX_SetColorUpdate(GX_TRUE);

		if (g_scene == SCENE_CAUTION) {
			GX_SetCopyClear((GXColor){0, 0, 0, 0xff}, 0x00ffffff);
			drawFullscreenTex(&texCaution);
			drawPressAHud();
		} else if (g_scene == SCENE_LOGO) {
			GX_SetCopyClear((GXColor){0, 0, 0, 0xff}, 0x00ffffff);
			drawBootLogoVariant(g_logo_variant);
			drawPressAHud();
		} else if (g_scene == SCENE_INTRO) {
			GX_SetCopyClear((GXColor){0, 0, 0, 0xff}, 0x00ffffff);
			drawFullscreenTex(&texIntro);
			drawPressAHud();
		} else if (g_scene == SCENE_TITLE) {
			GX_SetCopyClear((GXColor){0, 0, 0, 0xff}, 0x00ffffff);
			drawTitleAtlas();
			drawTitleMenu();
		} else if (g_scene == SCENE_LOAD) {
			GX_SetCopyClear((GXColor){0, 0, 0, 0xff}, 0x00ffffff);
			drawLoadHud();
		} else if (g_scene == SCENE_LOBBY) {
			GX_SetCopyClear((GXColor){0x10, 0x14, 0x28, 0xff}, 0x00ffffff);
			if (g_lobby_mode == LOBBY_CREATE)
				drawFullscreenTex(&texCreate);
			else
				drawLobbyAtlas();
			drawLobbyHud();
		} else if (g_scene == SCENE_STSEL) {
			GX_SetCopyClear((GXColor){0x08, 0x10, 0x20, 0xff}, 0x00ffffff);
			drawStselAtlas();
			drawStselHud();
		} else if (g_scene == SCENE_PLAY && g_stage_ready) {
			GX_SetCopyClear(background, 0x00ffffff);
			drawStageBg(cam_x);
			{
				int order[NUM_ENEMIES + 1 + NPC_SCENERY_MAX];
				int n = 0;
				int a, b;

				for (i = 0; i < NUM_ENEMIES; i++)
					order[n++] = i;
				order[n++] = -1;
				for (i = 0; i < NPC_SCENERY_MAX; i++)
					order[n++] = NUM_ENEMIES + i;

				for (a = 0; a < n - 1; a++) {
					for (b = a + 1; b < n; b++) {
						f32 ya = actor_sort_y(order[a]);
						f32 yb = actor_sort_y(order[b]);
						if (yb < ya) {
							int tmp = order[a];
							order[a] = order[b];
							order[b] = tmp;
						}
					}
				}

				for (i = 0; i < n; i++) {
					if (order[i] < 0) {
						drawNovicePose(novice.x - cam_x, novice.y,
							       novice.scale, novice.pose, novice.facing);
					} else if (order[i] < NUM_ENEMIES) {
						Actor *ea;
						if (!enemies[order[i]].alive)
							continue;
						/* Blink while stunned. */
						if (enemies[order[i]].hit_timer > 0 &&
						    ((enemies[order[i]].hit_timer / 2) & 1))
							continue;
						ea = &enemies[order[i]].base;
						cd = &g_chardefs[enemies[order[i]].type];
						drawCharacterPose(cd, ea->pose, ea->x - cam_x, ea->y,
								  ea->scale, ea->facing);
					} else {
						int ni = order[i] - NUM_ENEMIES;
						Actor *na;

						if (ni < 0 || ni >= NPC_SCENERY_MAX)
							continue;
						if (!scenery[ni].alive)
							continue;
						na = &scenery[ni].base;
						if (scenery_culled(na, cam_x))
							continue;
						drawCharacterPose(&g_npcdef, na->pose,
								  na->x - cam_x, na->y,
								  na->scale, na->facing);
					}
				}
			}
			drawA5Deco(cam_x);
			if (g_hit_flash_t > 0) {
				drawPanelTextStart();
				GX_SetChanMatColor(GX_COLOR0A0,
						   (GXColor){255, 240, 80,
							     (u8)(40 + g_hit_flash_t * 20)});
				drawSolidQuad(g_hit_flash_x - cam_x - 24.0f,
					      g_hit_flash_y - 24.0f, 48.0f, 48.0f);
				drawPanelTextEnd();
			}
			drawPlayHud(cam_x);
			if (g_paused)
				drawPauseMenu();
		}

		GX_DrawDone();
		GX_CopyDisp(frameBuffer[fb], GX_TRUE);
		VIDEO_SetNextFramebuffer(frameBuffer[fb]);
		if (first_frame) {
			VIDEO_SetBlack(FALSE);
			first_frame = 0;
		}
		VIDEO_Flush();
		VIDEO_WaitVSync();
		fb ^= 1;
	}

	escape_exit();
	return 0;
}
