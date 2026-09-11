#ifndef RBO_SPAWN_H
#define RBO_SPAWN_H

/*
 * Nested spawn-program VM (PC FUN_00425540) plus the 4-slot table
 * HOST_D.0/1/2/4 drive (DAT_00aad440, stride 0x12 dwords).
 *
 * FOB opcode 0 only skips the blob. PUSH_STR points at the payload;
 * HOST_D.0 binds that pointer to a slot; HOST_D.4 ticks the interpreter.
 */

#include <gctypes.h>

struct RboFob;

#define RBO_SPAWN_SLOTS 4

typedef struct {
	s32 char_id;
	s32 sub;
	s32 x;
	s32 y;
	s32 pose;
	s32 facing;
	s32 flags;
} RboSpawnRec;

typedef void (*RboSpawnEmitFn)(const RboSpawnRec *rec, void *user);

void rbo_spawn_reset(void);
void rbo_spawn_set_emit(RboSpawnEmitFn fn, void *user);

/* HOST_D.0 — slot PC + backup = DATA payload (code-relative). */
void rbo_spawn_bind(int slot, u32 code_rel);
/* HOST_D.1 — non-zero enables the slot for HOST_D.4. */
void rbo_spawn_enable(int slot, s32 enable);
/* HOST_D.2 — OR of enable flags from slot..end (slot -1 = all four). */
s32 rbo_spawn_enabled_mask(int from_slot);
/* HOST_D.4 — tick every enabled slot (FUN_00425bb0 → FUN_00425b30). */
void rbo_spawn_tick_all(struct RboFob *f);

u32 rbo_spawn_emit_count(void);
u32 rbo_spawn_drop_count(void);
u32 rbo_spawn_place_count(void);
s32 rbo_spawn_last_drop_id(void);
s32 rbo_spawn_last_drop_pose(void);

/* Lookup miss (no resident sheet). Place = actually inserted into a pool. */
void rbo_spawn_count_drop(s32 char_id, s32 pose);
void rbo_spawn_count_place(void);

#endif /* RBO_SPAWN_H */
