#ifndef RBO_CHARA_H
#define RBO_CHARA_H

/*
 * Character .dat + .fob drop-in (PC FUN_00444c80 / chara_file.fob).
 * Proto uses offline anim tables (*_anims.h) + STAGE01.TPL sheets.
 * Lookup maps FOB char_id onto those sheets; no NPC_ALL.DAT load.
 */

#include <gctypes.h>
#include <ogc/tpl.h>

#define RBO_CHAR_PORING     25
#define RBO_CHAR_LUNATIC    84
#define RBO_CHAR_WILLOW     85
#define RBO_CHAR_ROCKER     86
#define RBO_CHAR_CHONCHON   88
#define RBO_CHAR_FABRE      89
#define RBO_CHAR_SPORE      90
#define RBO_CHAR_CREAMY     91
#define RBO_CHAR_EQURIPS    93
#define RBO_CHAR_NPC_ALL    182

enum {
	RBO_BIND_NONE = 0,
	RBO_BIND_ENEMY,
	RBO_BIND_NPC
};

typedef struct {
	int kind;        /* RBO_BIND_* */
	int enemy_type;  /* 0..8, enemies[] */
	int npc_pose;    /* compact NPC_ALL atlas index */
} RboCharaBind;

typedef struct {
	int char_id;
	int pose_id;
	int frame;
	f32 x, y;
	f32 scale;
} RboCharaInst;

void rbo_chara_init(void);

/* char_id + DATA pose → resident TPL sheet. 0 = no sheet (skip). */
int rbo_chara_lookup(int char_id, int dat_pose, RboCharaBind *out);

int rbo_chara_bind_sheet(int char_id, GXTexObj *sheet);

void rbo_chara_set_pose(RboCharaInst *inst, int pose_id);
void rbo_chara_tick(RboCharaInst *inst);
void rbo_chara_draw(const RboCharaInst *inst);

#endif /* RBO_CHARA_H */
