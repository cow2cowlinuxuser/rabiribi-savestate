/* Stage 1 plate-0 NPC_ALL scenery (quarter of DATA @01265C rows).
 * param = NPC_ALL pose index; compact atlas pose = table order.
 * NPC_SCENERY_MAX cuts the pool without changing the table.
 */
#ifndef STAGE01_NPCS_H
#define STAGE01_NPCS_H

#include <gctypes.h>

#ifndef NPC_SCENERY_MAX
#define NPC_SCENERY_MAX 10
#endif

typedef struct {
	f32 x, y_off, facing;
	u8 pose; /* index into npc_all_poses */
} StageNpcRow;

/* y_off = FOB DATA rec[9]: depth lane on the floor (neg = further back).
 * Not jump height. Player Y is pinned; these offsets stay. */
static const StageNpcRow k_stage01_npcs[] = {
	{ 180.0f, -45.0f,  1.0f, 0 }, /* NOVI_A        DAT 30 */
	{ 250.0f, -80.0f,  1.0f, 1 }, /* ARC_M_1       DAT 45 */
	{ 310.0f, -35.0f,  1.0f, 2 }, /* ARCHER_F_2    DAT 75 */
	{ 400.0f, -90.0f,  1.0f, 3 }, /* MAGI_M_1      DAT 50 */
	{ 450.0f, -90.0f,  1.0f, 4 }, /* PATTERN059    DAT 60 */
	{ 500.0f,  50.0f,  1.0f, 5 }, /* MARCH_M_1     DAT 65 */
	{ 580.0f, -70.0f, -1.0f, 6 }, /* PATTERN015    DAT 15 */
	{ 750.0f,  10.0f, -1.0f, 7 }, /* PATTERN020    DAT 20 */
	{ 850.0f,  30.0f,  1.0f, 8 }, /* PATTERN025    DAT 26 */
	{ 950.0f, -30.0f, -1.0f, 9 }, /* PATTERN001    DAT 31 */
};

#define STAGE01_NPC_ROWS (int)(sizeof(k_stage01_npcs) / sizeof(k_stage01_npcs[0]))

#endif /* STAGE01_NPCS_H */
