#include "rbo_chara.h"

#include <stddef.h>

/* Compact NPC_ALL atlas: DAT pose id → table order in npc_all_anims.h. */
static const struct {
	int dat_pose;
	int atlas;
} k_npc_pose_map[] = {
	{ 30, 0 }, /* NOVI_A */
	{ 45, 1 }, /* ARC_M_1 */
	{ 75, 2 }, /* ARCHER_F_2 */
	{ 50, 3 }, /* MAGI_M_1 */
	{ 60, 4 }, /* PATTERN059 */
	{ 65, 5 }, /* MARCH_M_1 */
	{ 15, 6 }, /* PATTERN015 */
	{ 20, 7 }, /* PATTERN020 */
	{ 26, 8 }, /* PATTERN025 */
	{ 31, 9 }, /* PATTERN001 */
};

void rbo_chara_init(void)
{
}

int rbo_chara_lookup(int char_id, int dat_pose, RboCharaBind *out)
{
	unsigned i;

	if (!out)
		return 0;
	out->kind = RBO_BIND_NONE;
	out->enemy_type = 0;
	out->npc_pose = 0;

	if (char_id == RBO_CHAR_NPC_ALL) {
		for (i = 0; i < sizeof(k_npc_pose_map) / sizeof(k_npc_pose_map[0]); i++) {
			if (k_npc_pose_map[i].dat_pose == dat_pose) {
				out->kind = RBO_BIND_NPC;
				out->npc_pose = k_npc_pose_map[i].atlas;
				return 1;
			}
		}
		return 0;
	}

	out->kind = RBO_BIND_ENEMY;
	switch (char_id) {
	case RBO_CHAR_PORING:   out->enemy_type = 0; return 1;
	case RBO_CHAR_EQURIPS:  out->enemy_type = 1; return 1;
	case RBO_CHAR_ROCKER:   out->enemy_type = 2; return 1;
	case RBO_CHAR_CREAMY:   out->enemy_type = 3; return 1;
	case RBO_CHAR_SPORE:    out->enemy_type = 4; return 1;
	case RBO_CHAR_FABRE:    out->enemy_type = 5; return 1;
	case RBO_CHAR_LUNATIC:  out->enemy_type = 6; return 1;
	case RBO_CHAR_WILLOW:   out->enemy_type = 7; return 1;
	case RBO_CHAR_CHONCHON: out->enemy_type = 8; return 1;
	default:
		out->kind = RBO_BIND_NONE;
		return 0;
	}
}

int rbo_chara_bind_sheet(int char_id, GXTexObj *sheet)
{
	(void)char_id;
	(void)sheet;
	return 0;
}

void rbo_chara_set_pose(RboCharaInst *inst, int pose_id)
{
	if (!inst)
		return;
	inst->pose_id = pose_id;
	inst->frame = 0;
}

void rbo_chara_tick(RboCharaInst *inst)
{
	if (!inst)
		return;
	inst->frame++;
}

void rbo_chara_draw(const RboCharaInst *inst)
{
	(void)inst;
}
