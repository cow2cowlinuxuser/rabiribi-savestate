#include "rbo_spawn.h"
#include "rbo_fob.h"

#include <string.h>

#define SPAWN_STEP_CAP 256
#define SPAWN_REC_WORDS 13

typedef struct {
	u32 pc;
	u32 base;
	u32 loop_pc;
	u32 rec; /* body of last cmd-13 (pc + 4) */
	s32 enable;
	s32 timer;
	s32 counter;
} RboSpawnSlot;

static RboSpawnSlot s_slots[RBO_SPAWN_SLOTS];
static RboSpawnEmitFn s_emit;
static void *s_emit_user;
static u32 s_emit_count;
static u32 s_drop_count;
static u32 s_place_count;
static s32 s_last_drop_id;
static s32 s_last_drop_pose;

static u32 rd_u32(const u8 *p)
{
	return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static s32 rd_i32(const u8 *p)
{
	return (s32)rd_u32(p);
}

static int in_code(const RboFob *f, u32 rel, u32 bytes)
{
	return f && f->code && (rel + bytes) <= f->code_len;
}

static s32 word_at(const RboFob *f, u32 rel)
{
	if (!in_code(f, rel, 4))
		return 0;
	return rd_i32(f->code + rel);
}

void rbo_spawn_reset(void)
{
	memset(s_slots, 0, sizeof(s_slots));
	s_emit_count = 0;
	s_drop_count = 0;
	s_place_count = 0;
	s_last_drop_id = 0;
	s_last_drop_pose = 0;
}

void rbo_spawn_set_emit(RboSpawnEmitFn fn, void *user)
{
	s_emit = fn;
	s_emit_user = user;
}

u32 rbo_spawn_emit_count(void)
{
	return s_emit_count;
}

u32 rbo_spawn_drop_count(void)
{
	return s_drop_count;
}

u32 rbo_spawn_place_count(void)
{
	return s_place_count;
}

s32 rbo_spawn_last_drop_id(void)
{
	return s_last_drop_id;
}

s32 rbo_spawn_last_drop_pose(void)
{
	return s_last_drop_pose;
}

void rbo_spawn_count_drop(s32 char_id, s32 pose)
{
	s_drop_count++;
	s_last_drop_id = char_id;
	s_last_drop_pose = pose;
}

void rbo_spawn_count_place(void)
{
	s_place_count++;
}

void rbo_spawn_bind(int slot, u32 code_rel)
{
	RboSpawnSlot *s;

	if (slot < 0 || slot >= RBO_SPAWN_SLOTS)
		return;
	s = &s_slots[slot];
	s->pc = code_rel;
	s->base = code_rel;
	s->loop_pc = code_rel;
	s->rec = 0;
}

void rbo_spawn_enable(int slot, s32 enable)
{
	if (slot < 0 || slot >= RBO_SPAWN_SLOTS)
		return;
	s_slots[slot].enable = enable;
}

s32 rbo_spawn_enabled_mask(int from_slot)
{
	int i, last;
	s32 mask = 0;

	if (from_slot < 0) {
		i = 0;
		last = RBO_SPAWN_SLOTS;
	} else {
		i = from_slot;
		last = from_slot + 1;
		if (last > RBO_SPAWN_SLOTS)
			last = RBO_SPAWN_SLOTS;
	}
	for (; i < last; i++)
		mask |= s_slots[i].enable;
	return mask;
}

/* FUN_00425460 — extras used by cmd 17 / cmd 0x10. */
static void slot_reset_extras(RboSpawnSlot *s)
{
	s->timer = 0;
	s->counter = 0;
	s->rec = 0;
}

static void emit_rec(const RboFob *f, RboSpawnSlot *s)
{
	RboSpawnRec rec;

	if (!s_emit || !s->rec || !in_code(f, s->rec, 12 * 4))
		return;

	/* Body after the cmd-13 opcode: FUN_00425950 piVar1[n]. */
	rec.sub = word_at(f, s->rec + 0);
	rec.char_id = word_at(f, s->rec + 4);
	rec.x = word_at(f, s->rec + 28);
	rec.y = word_at(f, s->rec + 32);
	rec.pose = word_at(f, s->rec + 36);
	rec.facing = word_at(f, s->rec + 40);
	rec.flags = word_at(f, s->rec + 44);
	s_emit_count++;
	s_emit(&rec, s_emit_user);
}

/*
 * One FUN_00425540 step. Returns:
 *   < 0     stop this tick (terminator / fail / wait)
 *   0xffff  cmd 13 spawn
 *   0xfffe  cmd 20 spawn variant
 */
static int spawn_step(RboFob *f, RboSpawnSlot *s)
{
	s32 op, sub, n, i;

	if (!in_code(f, s->pc, 4))
		return -1;
	op = word_at(f, s->pc);

	switch (op) {
	case 0x11:
		slot_reset_extras(s);
		s->pc += 4;
		return 1;
	case 0x10:
		if (!in_code(f, s->pc, 12))
			return -1;
		sub = word_at(f, s->pc + 4);
		s->pc += 8;
		switch (sub) {
		case 0:
		case 4:
			s->pc += 8;
			break;
		case 1:
		case 2:
		case 5:
		case 6:
		case 7:
			s->pc += 4;
			break;
		case 3:
			n = word_at(f, s->pc);
			s->pc += 4;
			if (n != 0) {
				n = word_at(f, s->pc);
				s->pc += 8;
				if (n > 0 && n < 64)
					s->pc += (u32)n * 8u;
			}
			break;
		default:
			return -1;
		}
		return 1;
	case 0xd:
		if (!in_code(f, s->pc, SPAWN_REC_WORDS * 4))
			return -1;
		s->rec = s->pc + 4;
		s->pc += SPAWN_REC_WORDS * 4;
		return 0xffff;
	case 0x14:
		if (!in_code(f, s->pc, SPAWN_REC_WORDS * 4))
			return -1;
		s->rec = s->pc + 4;
		s->pc += SPAWN_REC_WORDS * 4;
		return 0xfffe;
	case -1:
		s->enable = 0;
		s->pc += 4;
		return -1;
	case 3:
		s->timer = word_at(f, s->pc + 4);
		s->pc += 8;
		return 1;
	case 4:
		if (s->timer <= word_at(f, s->pc + 4)) {
			s->pc += 8;
			return 1;
		}
		return -1;
	case 6:
		s->pc += 4;
		s->loop_pc = s->pc;
		return 1;
	case 7:
		s->pc = s->loop_pc;
		return 1;
	case 9:
		n = word_at(f, s->pc + 4);
		i = word_at(f, s->pc + 8);
		if (n >= 0 && n < RBO_SPAWN_SLOTS)
			s_slots[n].enable = i;
		s->pc += 12;
		return 1;
	case 0xa:
		s->counter = word_at(f, s->pc + 4);
		s->pc += 8;
		return 1;
	case 0xb:
		s->counter--;
		if (s->counter < 1) {
			s->pc += 4;
			return 1;
		}
		s->pc = s->loop_pc;
		return -1;
	case 0xc:
		s->counter--;
		if (s->counter < 1) {
			s->pc += 4;
			return 1;
		}
		s->pc = s->loop_pc;
		return 1;
	case 1:
	case 8:
	case 0x12:
	case 0x13:
		s->pc += 8;
		return 1;
	case 2:
		s->pc += 8;
		return 1;
	case 0xf:
		s->pc += 4;
		return 1;
	default:
		/* PC default is an infinite loop. Stop instead. */
		s->enable = 0;
		return -1;
	}
}

/* FUN_00425b30 — burst until flags.low == 1 or the VM returns < 0. */
static void spawn_burst(RboFob *f, RboSpawnSlot *s)
{
	int step, rc;
	s32 flags;

	for (step = 0; step < SPAWN_STEP_CAP; step++) {
		rc = spawn_step(f, s);
		if (rc < 0)
			return;
		if (rc == 1)
			continue;
		emit_rec(f, s);
		flags = in_code(f, s->rec + 44, 4) ? word_at(f, s->rec + 44) : 1;
		if ((flags & 0xff) == 1) {
			s->enable = 0;
			return;
		}
	}
}

void rbo_spawn_tick_all(RboFob *f)
{
	int i;
	RboSpawnSlot *s;

	if (!f || !f->code)
		return;
	for (i = 0; i < RBO_SPAWN_SLOTS; i++) {
		s = &s_slots[i];
		if (!s->enable)
			continue;
		if (s->timer > 0)
			s->timer--;
		spawn_burst(f, s);
	}
}
