#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "rbo_fob.h"
#include "rbo_audio.h"
#include "rbo_spawn.h"
#include "sd_assets.h"

#define RBO_FOB_AUDIO_POLL_INSNS 256u

/* Tagged stack values. Bit 30 = bytecode reloc; bit 29 = VM register file. */
#define FOB_TAG_PTR 0x40000000u
#define FOB_TAG_REG 0x20000000u
#define FOB_PTR_MASK 0x3fffffffu
#define FOB_TAG_KIND 0x60000000u
#define FOB_REG_BANK_SHIFT 16
#define FOB_REG_BANK_MASK 0xFu
#define FOB_REG_OFF_MASK 0xFFFFu
#define FOB_REG_BANK_AB25F4 0u
#define FOB_REG_BANK_ADD6AC 1u

static u16 rd_u16(const u8 *p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32 rd_u32(const u8 *p)
{
	return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static s32 rd_i32(const u8 *p)
{
	return (s32)rd_u32(p);
}

static void wr_i32(u8 *p, s32 v)
{
	u32 u = (u32)v;
	p[0] = (u8)(u & 0xff);
	p[1] = (u8)((u >> 8) & 0xff);
	p[2] = (u8)((u >> 16) & 0xff);
	p[3] = (u8)((u >> 24) & 0xff);
}

static int push(RboFob *f, s32 v)
{
	if (f->sp >= RBO_FOB_STACK) {
		f->error = 1;
		return -1;
	}
	f->stack[f->sp++] = v;
	return 0;
}

static s32 pop(RboFob *f)
{
	if (f->sp <= 0) {
		f->error = 1;
		return 0;
	}
	return f->stack[--f->sp];
}

static s32 peek(RboFob *f)
{
	if (f->sp <= 0) {
		f->error = 1;
		return 0;
	}
	return f->stack[f->sp - 1];
}

static int is_code_ptr(s32 v)
{
	return ((u32)v & FOB_TAG_KIND) == FOB_TAG_PTR;
}

static int is_reg_ptr(s32 v)
{
	return ((u32)v & FOB_TAG_KIND) == FOB_TAG_REG;
}

static int is_ptr(s32 v)
{
	return is_code_ptr(v) || is_reg_ptr(v);
}

static u32 ptr_rel(s32 v)
{
	return (u32)v & FOB_PTR_MASK;
}

static s32 make_reg_ptr(u32 bank, u32 byte_off)
{
	return (s32)(FOB_TAG_REG | ((bank & FOB_REG_BANK_MASK) << FOB_REG_BANK_SHIFT) |
		     (byte_off & FOB_REG_OFF_MASK));
}

static s32 *reg_slot(RboFob *f, s32 p)
{
	u32 bank, off, idx;

	if (!is_reg_ptr(p))
		return NULL;
	bank = ((u32)p >> FOB_REG_BANK_SHIFT) & FOB_REG_BANK_MASK;
	off = (u32)p & FOB_REG_OFF_MASK;
	if (off & 3)
		return NULL;
	idx = off / 4;
	if (idx >= RBO_FOB_REGS)
		return NULL;
	if (bank == FOB_REG_BANK_AB25F4)
		return &f->reg_ab25f4[idx];
	if (bank == FOB_REG_BANK_ADD6AC)
		return &f->reg_add6ac[idx];
	return NULL;
}

/* FUN_0042afe0 flag bits: 1 = deref right/value, 2 = deref left/dest. */
static s32 load_indirect(RboFob *f, s32 p)
{
	if (is_code_ptr(p)) {
		u32 rel = ptr_rel(p);

		if (rel + 4 > f->code_len) {
			f->error = 1;
			return 0;
		}
		return rd_i32(f->code + rel);
	}
	if (is_reg_ptr(p)) {
		s32 *slot = reg_slot(f, p);

		return slot ? *slot : 0;
	}
	return p;
}

static s32 apply_store_mode(s32 cur, s32 val, u16 mode)
{
	switch (mode) {
	case 0:
		return val;
	case 100:
		return cur + val;
	case 101:
		return cur - val;
	case 102:
		return cur * val;
	case 103:
		return val ? cur / val : 0;
	case 104:
		return val ? cur % val : 0;
	case 105:
		return cur & val;
	case 106:
		return cur | val;
	case 107:
		return cur ^ val;
	default:
		return val;
	}
}

/* Resolve stack value; if tagged reloc + want_deref, read through the pointer. */
static s32 resolve_val(RboFob *f, s32 v, int want_deref)
{
	if (want_deref && is_ptr(v))
		return load_indirect(f, v);
	return v;
}

static int var_index_name(RboFob *f, const char *name, int create)
{
	int i;

	if (!name || !name[0])
		return -1;
	for (i = 0; i < f->n_vars; i++) {
		if (strcmp(f->var_name[i], name) == 0)
			return i;
	}
	if (!create || f->n_vars >= RBO_FOB_VARS)
		return -1;
	snprintf(f->var_name[f->n_vars], sizeof(f->var_name[0]), "%s", name);
	f->var_val[f->n_vars] = 0;
	return f->n_vars++;
}

s32 rbo_fob_get_var(RboFob *f, const char *name, s32 fallback)
{
	int i = var_index_name(f, name, 0);
	return (i >= 0) ? f->var_val[i] : fallback;
}

void rbo_fob_set_var(RboFob *f, const char *name, s32 val)
{
	int i = var_index_name(f, name, 1);
	if (i >= 0)
		f->var_val[i] = val;
}

/* Phase-style slots live in mutable bytecode (PC writes into the FOB image). */
s32 rbo_fob_read_slot(RboFob *f, u32 rel, s32 fallback)
{
	if (!f || !f->code || rel + 4 > f->code_len)
		return fallback;
	return rd_i32(f->code + rel);
}

static const char *cstr_at(RboFob *f, u32 rel)
{
	static char tmp[240];
	u32 i = 0;

	if (rel >= f->code_len)
		return "";
	while (i + 1 < sizeof(tmp) && rel + i < f->code_len) {
		char c = (char)f->code[rel + i];
		if (!c)
			break;
		tmp[i++] = c;
	}
	tmp[i] = 0;
	return tmp;
}

/* PC STORE (FUN_0042afe0 case 10): dest stays on the stack; the following POP
 * discards it. Mode 0 assigns; 100..107 are the in-place ALU stores. */
static void store_indirect(RboFob *f, s32 dest, s32 val, u16 mode)
{
	if (is_code_ptr(dest)) {
		u32 rel = ptr_rel(dest);
		s32 cur;

		if (rel + 4 > f->code_len)
			return;
		cur = rd_i32(f->code + rel);
		wr_i32(f->code + rel, apply_store_mode(cur, val, mode));
		return;
	}
	if (is_reg_ptr(dest)) {
		s32 *slot = reg_slot(f, dest);

		if (slot)
			*slot = apply_store_mode(*slot, val, mode);
		return;
	}
	if (mode == 0) {
		const char *s = cstr_at(f, (u32)dest);

		if (s[0])
			rbo_fob_set_var(f, s, val);
	}
}

int rbo_fob_load(RboFob *f, const char *vol, const char *path)
{
	u8 *raw;
	u32 size = 0;
	u32 n, pos, i, n_pools, code_len;

	memset(f, 0, sizeof(*f));
	raw = sd_assets_load(vol, path, &size);
	if (!raw || size < 16)
		return -1;

	n = rd_u32(raw);
	if (n == 0 || n > RBO_FOB_MAX_EXPORTS) {
		free(raw);
		return -2;
	}
	pos = 4;
	for (i = 0; i < n; i++) {
		if (pos + 0x24 > size) {
			free(raw);
			return -3;
		}
		memcpy(f->exports[i].name, raw + pos, 0x20);
		f->exports[i].name[31] = 0;
		f->exports[i].code_off = rd_u32(raw + pos + 0x20);
		pos += 0x24;
	}
	f->n_exports = (int)n;
	if (pos + 8 > size) {
		free(raw);
		return -4;
	}
	n_pools = rd_u32(raw + pos);
	pos += 4;
	for (i = 0; i < n_pools; i++) {
		u32 cnt;
		if (pos + 4 > size) {
			free(raw);
			return -5;
		}
		cnt = rd_u32(raw + pos);
		pos += 4 + cnt * 4;
		if (pos > size) {
			free(raw);
			return -5;
		}
	}
	if (pos + 4 > size) {
		free(raw);
		return -6;
	}
	code_len = rd_u32(raw + pos);
	pos += 4;
	if (pos + code_len > size) {
		free(raw);
		return -7;
	}

	f->raw = raw;
	f->raw_size = size;
	f->code = raw + pos;
	f->code_len = code_len;
	rbo_spawn_reset();
	return 0;
}

void rbo_fob_unload(RboFob *f)
{
	if (!f)
		return;
	if (f->raw)
		free(f->raw);
	memset(f, 0, sizeof(*f));
	rbo_spawn_reset();
}

int rbo_fob_find_export(const RboFob *f, const char *name)
{
	int i;
	for (i = 0; i < f->n_exports; i++) {
		if (strcmp(f->exports[i].name, name) == 0)
			return i;
	}
	return -1;
}

/* PC stack is (value, type) with type on top. Type 2 = pointer. */
static int host_pop_typed(RboFob *f, s32 *type_out, s32 *val_out)
{
	if (f->sp < 2)
		return -1;
	*type_out = f->stack[--f->sp];
	*val_out = f->stack[--f->sp];
	return 0;
}

static s32 host_deref_if_ptr(RboFob *f, s32 type, s32 val)
{
	if (type != 2)
		return val;
	if (is_ptr(val))
		return load_indirect(f, val);
	return val;
}

static int host_as_code_rel(RboFob *f, s32 val, u32 *rel_out)
{
	u32 rel;

	if (is_reg_ptr(val))
		return -1;
	if (is_code_ptr(val))
		rel = ptr_rel(val);
	else if (val >= 0)
		rel = (u32)val;
	else
		return -1;
	if (rel >= f->code_len)
		return -1;
	*rel_out = rel;
	return 0;
}

static void host_write_dest(RboFob *f, s32 dest, s32 val)
{
	u32 rel;

	if (is_ptr(dest)) {
		store_indirect(f, dest, val, 0);
		return;
	}
	if (host_as_code_rel(f, dest, &rel) != 0)
		return;
	if (rel + 4 <= f->code_len)
		wr_i32(f->code + rel, val);
}

static void host_d0(RboFob *f)
{
	s32 t_ptr, ptr, t_idx, idx;
	u32 rel;

	/* FUN_0042e690: (ptr, type=2), (index, type). ptr is NOT deref'd. */
	if (host_pop_typed(f, &t_ptr, &ptr) != 0 || t_ptr != 2)
		return;
	if (host_pop_typed(f, &t_idx, &idx) != 0)
		return;
	idx = host_deref_if_ptr(f, t_idx, idx);
	if (host_as_code_rel(f, ptr, &rel) != 0)
		return;
	f->d0_hits++;
	rbo_spawn_bind((int)idx, rel);
}

static void host_d1(RboFob *f)
{
	s32 t_en, en, t_idx, idx;

	if (host_pop_typed(f, &t_en, &en) != 0)
		return;
	if (host_pop_typed(f, &t_idx, &idx) != 0)
		return;
	en = host_deref_if_ptr(f, t_en, en);
	idx = host_deref_if_ptr(f, t_idx, idx);
	f->d1_hits++;
	rbo_spawn_enable((int)idx, en);
}

static void host_d2(RboFob *f)
{
	s32 t_dst, dst, t_idx, idx;

	if (host_pop_typed(f, &t_dst, &dst) != 0 || t_dst != 2)
		return;
	if (host_pop_typed(f, &t_idx, &idx) != 0)
		return;
	idx = host_deref_if_ptr(f, t_idx, idx);
	host_write_dest(f, dst, rbo_spawn_enabled_mask((int)idx));
}

/* D.3/6/7/8: (dest type=2, arg) — write 0 so scripts do not read stale stack. */
static void host_d_query_stub(RboFob *f)
{
	s32 t_dst, dst, t_arg, arg;

	if (host_pop_typed(f, &t_dst, &dst) != 0)
		return;
	if (host_pop_typed(f, &t_arg, &arg) != 0)
		return;
	(void)t_dst;
	(void)arg;
	host_write_dest(f, dst, 0);
}

static void host_dispatch(RboFob *f, u16 op, u16 sub)
{
	s32 a, b, c, d, e, g;

	f->host_hits++;
	/* HOST_C.1 — camera/X lock band (StageMain area scripts). */
	if (op == 0xC && sub == 1) {
		/* Pushes: 17, 0, lo, 0, hi, 0 (top = last). */
		g = pop(f);
		e = pop(f);
		d = pop(f);
		c = pop(f);
		b = pop(f);
		a = pop(f);
		(void)a;
		(void)b;
		(void)d;
		(void)g;
		f->lock_lo = (f32)c;
		f->lock_hi = (f32)e;
		if (f->lock_hi < f->lock_lo) {
			f32 t = f->lock_lo;
			f->lock_lo = f->lock_hi;
			f->lock_hi = t;
		}
		f->have_lock = 1;
		return;
	}
	if (op == 0xD) {
		switch (sub) {
		case 0:
			host_d0(f);
			return;
		case 1:
			host_d1(f);
			return;
		case 2:
			host_d2(f);
			return;
		case 3:
		case 6:
		case 7:
		case 8:
			host_d_query_stub(f);
			return;
		case 4:
			f->d4_hits++;
			rbo_spawn_tick_all(f);
			return;
		case 5:
			return;
		default:
			break;
		}
	}
	/* Drain all args — unknown hosts must not grow the stack. */
	f->sp = 0;
}

static int step(RboFob *f)
{
	u16 op, sub;
	u32 pc = f->pc;
	const u8 *code = f->code;

	if (f->halted || f->error)
		return -1;
	if (pc + 2 > f->code_len) {
		f->error = 1;
		return -1;
	}
	op = rd_u16(code + pc);
	if (op == 0x18 || op == 0x19) {
		f->halted = 1;
		f->pc = pc + 2;
		return 1;
	}
	if (op > 0x19) {
		f->error = 1;
		return -1;
	}
	if (pc + 4 > f->code_len) {
		f->error = 1;
		return -1;
	}
	sub = rd_u16(code + pc + 2);

	/* op 0 DATA */
	if (op == 0) {
		u32 n, size;
		if (pc + 8 > f->code_len) {
			f->error = 1;
			return -1;
		}
		n = rd_u32(code + pc + 4);
		size = 8 + 4 * n;
		if (pc + size > f->code_len) {
			f->error = 1;
			return -1;
		}
		f->pc = pc + size;
		return 0;
	}

	/* op 1 stack/alu */
	if (op == 1) {
		if (sub == 0) {
			push(f, rd_i32(code + pc + 4));
			f->pc = pc + 8;
			return 0;
		}
		if (sub == 1) {
			u32 rel = rd_u32(code + pc + 4);
			/* Pointer into mutable bytecode (phase slots / C strings). */
			push(f, (s32)(FOB_TAG_PTR | (rel & FOB_PTR_MASK)));
			f->pc = pc + 8;
			return 0;
		}
		if (sub == 2) {
			push(f, make_reg_ptr(FOB_REG_BANK_ADD6AC, 0));
			f->pc = pc + 4;
			return 0;
		}
		if (sub == 3) {
			push(f, make_reg_ptr(FOB_REG_BANK_AB25F4, 0));
			f->pc = pc + 4;
			return 0;
		}
		if (sub >= 4 && sub <= 8) {
			push(f, f->scratch[sub - 4]);
			f->pc = pc + 4;
			return 0;
		}
		if (sub == 9) {
			(void)pop(f);
			f->pc = pc + 4;
			return 0;
		}
		if (sub == 0xA) {
			/* STORE: TOS = value, NOS = dest pointer. Dest stays
			 * (FUN_0042afe0 case 10); scripts POP it afterwards. */
			u16 flags = rd_u16(code + pc + 4);
			u16 mode = rd_u16(code + pc + 6);
			s32 val = pop(f);
			s32 dest;

			if (f->sp <= 0) {
				f->error = 1;
				f->pc = pc + 8;
				return -1;
			}
			dest = peek(f);
			if (flags & 1)
				val = load_indirect(f, val);
			store_indirect(f, dest, val, mode);
			f->pc = pc + 8;
			return 0;
		}
		if (sub >= 0xB && sub <= 0x18) {
			s32 r, l;
			u16 flags = rd_u16(code + pc + 4);

			r = pop(f);
			if (flags & 1)
				r = load_indirect(f, r);
			l = pop(f);
			if (flags & 2)
				l = load_indirect(f, l);
			switch (sub) {
			case 0xB: push(f, l + r); break;
			case 0xC: push(f, l - r); break;
			case 0xD: push(f, l * r); break;
			case 0xE: push(f, r ? l / r : 0); break;
			case 0xF: push(f, r ? l % r : 0); break;
			case 0x10: push(f, l << r); break;
			case 0x11: push(f, (s32)((u32)l >> r)); break;
			case 0x12: push(f, l & r); break;
			case 0x13: push(f, l | r); break;
			case 0x14: push(f, ~l); (void)r; break;
			case 0x15: push(f, l ^ r); break;
			case 0x16: push(f, !l); (void)r; break;
			case 0x17: push(f, l && r); break;
			case 0x18: push(f, l || r); break;
			default: push(f, 0); break;
			}
			f->pc = pc + 6;
			return 0;
		}
		if (sub == 0x19) {
			u16 flags = rd_u16(code + pc + 4);
			u16 mode = rd_u16(code + pc + 6);
			s32 r = pop(f);
			s32 l = pop(f);
			s32 c = 0;

			if (flags & 1)
				r = load_indirect(f, r);
			if (flags & 2)
				l = load_indirect(f, l);
			switch (mode) {
			case 0: c = (l == r); break;
			case 1: c = (l != r); break;
			case 2: c = (l <= r); break;
			case 3: c = (l >= r); break;
			case 4: c = (l < r); break;
			case 5: c = (l > r); break;
			default: break;
			}
			push(f, c);
			f->pc = pc + 8;
			return 0;
		}
		if (sub == 0x1A) {
			s32 a, b;
			a = pop(f);
			b = pop(f);
			push(f, a);
			push(f, b);
			f->pc = pc + 4;
			return 0;
		}
		if (sub == 0x1B) {
			u16 flags = rd_u16(code + pc + 4);
			s32 v = pop(f);

			push(f, resolve_val(f, v, (flags & 2) != 0));
			f->pc = pc + 6;
			return 0;
		}
		f->error = 1;
		return -1;
	}

	/* op 2 control — FUN_0042b610 */
	if (op == 2) {
		if (sub == 0) {
			/* CJMP: flag(deref), cmp, target. Branch if cond==1. */
			u16 flag, cmp;
			u32 tgt;
			s32 v, take;
			if (pc + 12 > f->code_len) {
				f->error = 1;
				return -1;
			}
			flag = rd_u16(code + pc + 4);
			cmp = rd_u16(code + pc + 6);
			tgt = rd_u32(code + pc + 8);
			v = resolve_val(f, peek(f), flag == 2);
			switch (cmp) {
			case 0: take = (v == 0); break;
			case 1: take = (v != 0); break;
			case 2: take = (v < 1); break;
			case 3: take = (v >= 0); break;
			case 4: take = (v < 0); break;
			case 5: take = (v > 0); break;
			default: take = 0; break;
			}
			(void)pop(f);
			f->pc = take ? tgt : (pc + 12);
			return 0;
		}
		if (sub == 1) {
			/* SWITCH: flag, table@. Table: default, count, then (key,tgt)*count.
			 * FUN_0042b610 case 1 — key may be deref when flag==2. */
			u16 flag;
			u32 table, def_tgt, count, i, pos;
			s32 key;
			if (pc + 10 > f->code_len) {
				f->error = 1;
				return -1;
			}
			flag = rd_u16(code + pc + 4);
			table = rd_u32(code + pc + 6);
			key = resolve_val(f, peek(f), flag == 2);
			(void)pop(f);
			if (table + 8 > f->code_len) {
				f->error = 1;
				return -1;
			}
			def_tgt = rd_u32(code + table);
			count = rd_u32(code + table + 4);
			pos = table + 8;
			for (i = 0; i < count; i++) {
				s32 k;
				u32 tgt;
				if (pos + 8 > f->code_len)
					break;
				k = rd_i32(code + pos);
				tgt = rd_u32(code + pos + 4);
				if (k == key) {
					f->pc = tgt;
					return 0;
				}
				pos += 8;
			}
			f->pc = def_tgt;
			return 0;
		}
		if (sub == 2) {
			f->pc = rd_u32(code + pc + 4);
			return 0;
		}
		if (sub == 3) {
			u32 tgt = rd_u32(code + pc + 4);
			if (f->csp >= RBO_FOB_CALLS) {
				f->error = 1;
				return -1;
			}
			f->call_stack[f->csp++] = pc + 8;
			f->pc = tgt;
			return 0;
		}
		if (sub == 4 || sub == 5 || sub == 6 || sub == 7) {
			if (f->csp > 0)
				f->pc = f->call_stack[--f->csp];
			else
				f->halted = 1;
			return f->halted ? 1 : 0;
		}
		f->error = 1;
		return -1;
	}

	/* op 3 helpers */
	if (op == 3) {
		if (sub == 0) {
			f->pc = pc + 8;
			return 0;
		}
		if (sub == 2 || sub == 5) {
			u32 argc = rd_u32(code + pc + 4);
			while (argc-- > 0 && f->sp > 0)
				(void)pop(f);
			f->pc = pc + 8;
			return 0;
		}
		f->pc = pc + 4;
		return 0;
	}

	/* ops 4..0x17 HOST */
	if (op >= 4 && op <= 0x17) {
		host_dispatch(f, op, sub);
		f->pc = pc + 4;
		return 0;
	}

	f->error = 1;
	return -1;
}

int rbo_fob_run(RboFob *f, u32 max_insns)
{
	u32 i;
	int rc = 0;

	if (!f || !f->code)
		return -1;
	f->insn_ran = 0;
	for (i = 0; i < max_insns; i++) {
		rc = step(f);
		f->insn_ran++;
		if ((i % RBO_FOB_AUDIO_POLL_INSNS) == 0)
			rbo_audio_poll();
		if (rc != 0)
			break;
		if (f->error)
			return -2;
		if (f->halted)
			return 0;
	}
	if (!f->halted && rc == 0)
		return 1; /* budget */
	return f->error ? -2 : 0;
}

int rbo_fob_run_export(RboFob *f, const char *name, u32 max_insns)
{
	int idx = rbo_fob_find_export(f, name);
	if (idx < 0)
		return -3;
	f->pc = f->exports[idx].code_off;
	f->sp = 0;
	f->csp = 0;
	f->halted = 0;
	f->error = 0;
	return rbo_fob_run(f, max_insns);
}
