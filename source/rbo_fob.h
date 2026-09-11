/* French-Bread FOB loader + thin VM (Stage FULL path only).
 *
 * Opcode sizes match tools/disasm-fob.py / FUN_0042bf60.
 * HOST_C.1 applies X locks. HOST_D.0/1/2/4 run the spawn-program VM
 * (FUN_00425540) over DATA payloads. Other natives still drain the stack.
 * Allocate only via rbo_fob_load(); free with rbo_fob_unload().
 */
#ifndef RBO_FOB_H
#define RBO_FOB_H

#include <gctypes.h>

#define RBO_FOB_MAX_EXPORTS 64
#define RBO_FOB_STACK       256
#define RBO_FOB_CALLS       64
#define RBO_FOB_VARS        96
#define RBO_FOB_REGS        64

typedef struct {
	char name[32];
	u32 code_off; /* offset into code[] */
} RboFobExport;

typedef struct RboFob {
	u8 *raw;
	u32 raw_size;
	u8 *code;
	u32 code_len;
	RboFobExport exports[RBO_FOB_MAX_EXPORTS];
	int n_exports;

	/* VM */
	s32 stack[RBO_FOB_STACK];
	int sp;
	u32 call_stack[RBO_FOB_CALLS];
	int csp;
	u32 pc;
	int halted;
	int error;
	u32 host_hits;
	u32 d0_hits;
	u32 d1_hits;
	u32 d4_hits;
	u32 insn_ran;

	/* Named int vars (PUSH_STR + STORE targets like @12098 phase). */
	char var_name[RBO_FOB_VARS][32];
	s32 var_val[RBO_FOB_VARS];
	int n_vars;

	/* FUN_0042afe0 PUSH_REG: &DAT_00ab25f4 / &DAT_00add6ac plus scratch. */
	s32 reg_ab25f4[RBO_FOB_REGS];
	s32 reg_add6ac[RBO_FOB_REGS];
	s32 scratch[5];

	/* Last HOST_C.1 lock band (world X). */
	int have_lock;
	f32 lock_lo;
	f32 lock_hi;
} RboFob;

/* Load entire FOB into MEM1 (32-byte aligned). 0=ok. */
int rbo_fob_load(RboFob *f, const char *vol, const char *path);
void rbo_fob_unload(RboFob *f);

int rbo_fob_find_export(const RboFob *f, const char *name);

/* Run export until RET/HALT/budget. Returns 0=ok finished, 1=budget, <0=error. */
int rbo_fob_run_export(RboFob *f, const char *name, u32 max_insns);

/* Continue from current pc (for per-frame MainCallback). */
int rbo_fob_run(RboFob *f, u32 max_insns);

s32 rbo_fob_get_var(RboFob *f, const char *name, s32 fallback);
void rbo_fob_set_var(RboFob *f, const char *name, s32 val);

/* Read mutable bytecode slot (phase vars live in the FOB image). */
s32 rbo_fob_read_slot(RboFob *f, u32 rel, s32 fallback);

#endif /* RBO_FOB_H */
