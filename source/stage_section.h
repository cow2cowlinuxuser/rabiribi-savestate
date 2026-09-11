/* Universal stage section FSM (all stages share this shape).
 *
 * PC StageMainCallback: SWITCH @12098 → CALL area_* ; each area nested SWITCH
 * @12118; HOST_C.1 → FUN_00438110 sets scroll/X lock globals (lo/hi).
 *
 * Per-stage tables supply sections + spawns; mid-boss / boss are kinds on the
 * same rail (cinematics / HP gates later — same enter/clear/advance).
 */
#ifndef STAGE_SECTION_H
#define STAGE_SECTION_H

#include <gctypes.h>

enum {
	STAGE_SEC_FIELD = 0,   /* normal locked fight band */
	STAGE_SEC_MIDBOSS = 1, /* same FSM; special UI/BGM later */
	STAGE_SEC_BOSS = 2,
	STAGE_SEC_CLEAR = 3    /* stage clear — unlock walk, no combat spawn */
};

enum {
	STAGE_SEC_F_NONE = 0,
	STAGE_SEC_F_NO_SPAWN = 1,
	STAGE_SEC_F_CLEAR_BGM = 2, /* swap to clear cue on enter (StageInit strings) */
	STAGE_SEC_F_CAM_LOCK = 4  /* keep camera inside lo..hi (HOST_C.1 feel) */
};

typedef struct {
	u8 kind;
	u8 flags;
	u32 fob_call; /* PC StageMain CALL target (RE breadcrumb; 0 = none) */
	f32 lock_lo;  /* min X; stay 0 / behind player (do not teleport) */
	f32 lock_hi;  /* right-hand gate until section clear */
} StageSection;

typedef struct {
	u8 type;    /* enemy type index for this port */
	u8 section; /* index into StagePlan.sections */
	f32 x;
	f32 y_off;
	f32 facing;
} StageSpawn;

typedef struct {
	const StageSection *sections;
	int n_sections;
	const StageSpawn *spawns;
	int n_spawns;
} StagePlan;

#endif /* STAGE_SECTION_H */
