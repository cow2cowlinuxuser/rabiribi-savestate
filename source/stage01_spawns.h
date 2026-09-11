/* Stage 1 section plan — FOB DATA placements + HOST_C.1 hi gates.
 *
 * Locks are right-hand gates on one contiguous strip (lo stays 0).
 * Clear a pack → raise lock_hi; player/camera are not teleported.
 *
 * HOST_C.1 mined (tools/mine-host-c1.py):
 *   2048..4096, 0..5120, 5120..7168, 6144..8192, 8192..9216, …
 * area_a (CALL 75296) has no HOST_C.1 — it still emits town NPCs @01265C
 * AND the first six Porings @012F50 (x≈4340). Do not invent a 2048 town
 * gate; that trapped the player on the plate-0 wall with no enemies.
 * First real hi is area_c's 0..5120 (covers those Porings). HUD AREA 1
 * until that pack is cleared (PC DAT_00adff24).
 *
 * Static DATA combat = rows whose slot_id == char_id.
 * NPC_ALL (slot 182) param is a pose index, NOT a mob id. Pose 91/86/90/88
 * collide with Creamy/Rocker/Spore/ChonChon ids — those are town NPCs.
 * EventCall still owns Fabre / Lunatic / Equrips / ElderWillow / Ambernite.
 * HunterFly never appears.
 *
 * Types: 0 Poring, 1 Equrips, 2 Rocker, 3 Creamy, 4 Spore,
 *        5 Fabre, 6 Lunatic, 7 Willow, 8 ChonChon
 */
#ifndef STAGE01_SPAWNS_H
#define STAGE01_SPAWNS_H

#include "stage_section.h"

#define ENEMY_POOL 8 /* max concurrent; recycle per section (MEM1) */

static const StageSection k_stage01_sections[] = {
	/* 0 area_a — town NPCs + first Porings; hi from area_c 0..5120 */
	{ STAGE_SEC_FIELD, STAGE_SEC_F_CAM_LOCK, 75296, 0.0f, 5120.0f },
	/* 1 plate1 remnant — HOST_C.1 2048..4096 (raise-only; already 5120) */
	{ STAGE_SEC_FIELD, STAGE_SEC_F_CAM_LOCK, 78400, 0.0f, 4096.0f },
	/* 2 area_c lock already applied in sec 0 */
	{ STAGE_SEC_FIELD, STAGE_SEC_F_CAM_LOCK, 81972, 0.0f, 5120.0f },
	/* 3 clear — unlock full walkscape */
	{ STAGE_SEC_CLEAR,
	  (u8)(STAGE_SEC_F_NO_SPAWN | STAGE_SEC_F_CLEAR_BGM), 0, 0.0f,
	  10240.0f },
};

static const StageSpawn k_stage01_spawns[] = {
	/* Section 0 — DATA @012F50, slot_id=25. Placed by area_a, not area_c. */
	{ 0, 0, 4340.0f,   0.0f,  1.0f }, /* PORING peko-event pose 220 */
	{ 0, 0, 4370.0f,   0.0f,  1.0f },
	{ 0, 0, 4520.0f,   0.0f,  1.0f },
	{ 0, 0, 4600.0f,   0.0f, -1.0f },
	{ 0, 0, 4670.0f,   0.0f,  1.0f },
	{ 0, 0, 4720.0f,   0.0f, -1.0f },
};

static const StagePlan k_stage01_plan = {
	k_stage01_sections,
	(int)(sizeof(k_stage01_sections) / sizeof(k_stage01_sections[0])),
	k_stage01_spawns,
	(int)(sizeof(k_stage01_spawns) / sizeof(k_stage01_spawns[0])),
};

#define STAGE01_SECTION_COUNT (k_stage01_plan.n_sections)
#define STAGE01_SPAWN_COUNT   (k_stage01_plan.n_spawns)

#endif /* STAGE01_SPAWNS_H */
