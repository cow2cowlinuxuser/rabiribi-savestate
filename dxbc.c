#include "dxbc.h"

#include <math.h>
#include <string.h>

#define OP_ADD 0
#define OP_AND 1
#define OP_DISCARD 13
#define OP_DIV 14
#define OP_DP2 15
#define OP_DP3 16
#define OP_DP4 17
#define OP_ELSE 18
#define OP_ENDIF 21
#define OP_ENDLOOP 22
#define OP_EQ 24
#define OP_EXP 25
#define OP_FRC 26
#define OP_FTOI 27
#define OP_FTOU 28
#define OP_GE 29
#define OP_IADD 30
#define OP_IF 31
#define OP_IEQ 32
#define OP_IGE 33
#define OP_ILT 34
#define OP_IMAD 35
#define OP_IMAX 36
#define OP_IMIN 37
#define OP_IMUL 38
#define OP_INE 39
#define OP_INEG 40
#define OP_ISHL 41
#define OP_ISHR 42
#define OP_ITOF 43
#define OP_LOG 47
#define OP_LOOP 48
#define OP_LT 49
#define OP_MAD 50
#define OP_MIN 51
#define OP_MAX 52
#define OP_CUSTOMDATA 53
#define OP_MOV 54
#define OP_MOVC 55
#define OP_MUL 56
#define OP_NE 57
#define OP_NOP 58
#define OP_NOT 59
#define OP_OR 60
#define OP_RET 62
#define OP_ROUND_NE 64
#define OP_ROUND_NI 65
#define OP_ROUND_PI 66
#define OP_ROUND_Z 67
#define OP_RSQ 68
#define OP_SQRT 75
#define OP_SINCOS 77
#define OP_ULT 79
#define OP_UGE 80
#define OP_USHR 85
#define OP_UTOF 86
#define OP_XOR 87
#define OP_UDIV 78
#define OP_UMUL 81
#define OP_UMAD 82
#define OP_UMAX 83
#define OP_UMIN 84
#define OP_RCP 129
#define OP_F32TOF16 130
#define OP_F16TOF32 131
#define OP_COUNTBITS 134
#define OP_FIRSTBIT_HI 135
#define OP_FIRSTBIT_LO 136
#define OP_BFREV 141
#define OP_DCL_INDEX_RANGE 91
#define OP_DCL_RESOURCE 88
#define OP_DCL_CONSTANT_BUFFER 89
#define OP_DCL_SAMPLER 90
#define OP_DCL_INPUT 95
#define OP_DCL_INPUT_SGV 96
#define OP_DCL_INPUT_SIV 97
#define OP_DCL_INPUT_PS 98
#define OP_DCL_INPUT_PS_SGV 99
#define OP_DCL_INPUT_PS_SIV 100
#define OP_DCL_OUTPUT 101
#define OP_DCL_OUTPUT_SGV 102
#define OP_DCL_OUTPUT_SIV 103
#define OP_DCL_TEMPS 104
#define OP_DCL_INDEXABLE_TEMP 105
#define OP_DCL_GLOBAL_FLAGS 106

#define T_TEMP 0
#define T_INPUT 1
#define T_OUTPUT 2
#define T_INDEXABLE_TEMP 3
#define T_IMM32 4
#define T_CB 8
#define T_ICB 9
#define T_NULL 13

static int fourcc(const char *p, const char *s)
{
	return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

static const void *chunk_find(const unsigned char *b, unsigned n, const char *tag,
			      unsigned *out_size)
{
	const unsigned char *end;
	unsigned count, i;
	if (n < 32 || !fourcc((const char *)b, "DXBC"))
		return NULL;
	count = *(const uint32_t *)(b + 28);
	if (32 + count * 4 > n)
		return NULL;
	end = b + n;
	for (i = 0; i < count; i++) {
		unsigned off = *(const uint32_t *)(b + 32 + i * 4);
		unsigned sz;
		if (off + 8 > n)
			continue;
		if (!fourcc((const char *)(b + off), tag))
			continue;
		sz = *(const uint32_t *)(b + off + 4);
		if (b + off + 8 + sz > end)
			return NULL;
		*out_size = sz;
		return b + off + 8;
	}
	return NULL;
}

static void parse_sig(const unsigned char *p, unsigned sz, DxbcSig *out, unsigned *n)
{
	unsigned count, i;
	*n = 0;
	if (sz < 8)
		return;
	count = *(const uint32_t *)p;
	if (count > 32)
		count = 32;
	for (i = 0; i < count; i++) {
		const unsigned char *e = p + 8 + i * 24;
		unsigned name_off;
		const char *name;
		unsigned k;
		if ((unsigned)(e + 24 - p) > sz)
			break;
		name_off = *(const uint32_t *)e;
		out[i].sem_index = *(const uint32_t *)(e + 4);
		out[i].sysval = *(const uint32_t *)(e + 8);
		out[i].reg = *(const uint32_t *)(e + 16);
		out[i].mask = e[20];
		name = (const char *)p + name_off;
		if (name_off >= sz)
			name = "";
		for (k = 0; k < 31 && name_off + k < sz && name[k]; k++)
			out[i].name[k] = name[k];
		out[i].name[k] = 0;
		(*n)++;
	}
}

int dxbc_parse(const void *bytes, unsigned nbytes, DxbcShader *out)
{
	const unsigned char *b = (const unsigned char *)bytes;
	const void *p;
	unsigned sz = 0;
	uint32_t ver;

	memset(out, 0, sizeof(*out));
	if (!bytes || nbytes < 32)
		return 0;
	p = chunk_find(b, nbytes, "ISGN", &sz);
	if (p)
		parse_sig(p, sz, out->isgn, &out->n_isgn);
	p = chunk_find(b, nbytes, "OSGN", &sz);
	if (p)
		parse_sig(p, sz, out->osgn, &out->n_osgn);
	p = chunk_find(b, nbytes, "SHEX", &sz);
	if (!p)
		p = chunk_find(b, nbytes, "SHDR", &sz);
	if (!p || sz < 8)
		return 0;
	out->shex = (const uint32_t *)p;
	out->shex_dwords = sz / 4;
	ver = out->shex[0];
	out->type = (int)((ver >> 16) & 0xffff);
	/* One pass over the declarations so the per-vertex path does not have to
	 * assume the worst about which register files are live. */
	out->has_icb = 0;
	out->has_itemp = 0;
	out->ntemps = 0;
	{
		unsigned nd = out->shex[1], pc = 2;
		if (nd > out->shex_dwords)
			nd = out->shex_dwords;
		while (pc < nd) {
			uint32_t tok = out->shex[pc];
			int op = (int)(tok & 0x7ff);
			unsigned len;
			if (op == OP_CUSTOMDATA) {
				unsigned clen = (pc + 1 < nd) ? out->shex[pc + 1] : 2;
				if (clen < 2)
					clen = 2;
				if (((tok >> 11) & 0x1fffff) == 3)
					out->has_icb = 1;
				pc += clen;
				continue;
			}
			len = (tok >> 24) & 0x7f;
			if (len == 0)
				len = 1;
			if (op == OP_DCL_INDEXABLE_TEMP)
				out->has_itemp = 1;
			else if (op == OP_DCL_TEMPS && pc + 1 < nd)
				out->ntemps = out->shex[pc + 1];
			pc += len;
		}
		if (out->ntemps > 64)
			out->ntemps = 64;
	}
	return 1;
}

int dxbc_find_sem(const DxbcSig *sigs, unsigned n, const char *name, unsigned idx)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		if (sigs[i].sem_index == idx && _stricmp(sigs[i].name, name) == 0)
			return (int)i;
	}
	return -1;
}

/* Lowest-indexed entry with this semantic, whatever its semantic index. A
 * shader that only emits TEXCOORD1 still needs its UVs used, and demanding
 * index 0 leaves the rasteriser sampling a single texel. */
int dxbc_find_sem_any(const DxbcSig *sigs, unsigned n, const char *name)
{
	unsigned i;
	int best = -1;
	for (i = 0; i < n; i++) {
		if (_stricmp(sigs[i].name, name) != 0)
			continue;
		if (best < 0 || sigs[i].sem_index < sigs[best].sem_index)
			best = (int)i;
	}
	return best;
}

/* D3D_NAME_POSITION. Authoritative for clip-space output regardless of how the
 * compiler spelled the semantic. */
int dxbc_find_sysval_pos(const DxbcSig *sigs, unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++)
		if (sigs[i].sysval == 1)
			return (int)i;
	return -1;
}

typedef struct Opnd {
	int type;
	int nidx;
	int idx[3];
	int rel[3];
	int relreg[3];
	int relcomp[3];
	int swz[4];
	int mask;
	int neg, absn;
	float imm[4];
} Opnd;

typedef struct Exec {
	float r[64][4];
	float v[32][4];
	float o[32][4];
	float icb[256][4];
	float x[4][64][4]; /* indexable temps x0..x3 */
	unsigned nicb;
	const void *cb[14];
	unsigned cb_bytes[14];
	int skip;
} Exec;

static unsigned parse_index(const uint32_t *t, unsigned *p, unsigned end, int dim, uint32_t tok0,
			     Opnd *o)
{
	int rep = (int)((tok0 >> (22 + 3 * dim)) & 7);
	unsigned at = *p;
	o->rel[dim] = 0;
	if (at >= end)
		return 1;
	if (rep == 0) {
		o->idx[dim] = (int)t[at++];
	} else if (rep == 2 || rep == 3) {
		uint32_t relt;
		if (rep == 3) {
			if (at >= end)
				return 1;
			o->idx[dim] = (int)t[at++];
		} else
			o->idx[dim] = 0;
		if (at >= end)
			return 1;
		relt = t[at++];
		if (relt & 0x80000000u) {
			if (at >= end)
				return 1;
			at++;
		}
		if (at >= end)
			return 1;
		o->rel[dim] = 1;
		o->relreg[dim] = (int)t[at++];
		o->relcomp[dim] = (int)((relt >> 4) & 3);
	} else {
		o->idx[dim] = (int)t[at++];
	}
	*p = at;
	return 0;
}

static unsigned parse_opnd(const uint32_t *t, unsigned p, unsigned end, Opnd *o)
{
	uint32_t tok0;
	int ncomp, sel, dim, c;
	memset(o, 0, sizeof(*o));
	o->swz[0] = 0;
	o->swz[1] = 1;
	o->swz[2] = 2;
	o->swz[3] = 3;
	o->mask = 0xf;
	if (p >= end)
		return p;
	tok0 = t[p++];
	o->type = (int)((tok0 >> 12) & 0xff);
	ncomp = (int)(tok0 & 3);
	if (ncomp == 2) {
		sel = (int)((tok0 >> 2) & 3);
		if (sel == 0)
			o->mask = (int)((tok0 >> 4) & 0xf);
		else if (sel == 1) {
			unsigned sw = (tok0 >> 4) & 0xff;
			o->swz[0] = (int)(sw & 3);
			o->swz[1] = (int)((sw >> 2) & 3);
			o->swz[2] = (int)((sw >> 4) & 3);
			o->swz[3] = (int)((sw >> 6) & 3);
			o->mask = 0xf;
		} else {
			int name = (int)((tok0 >> 4) & 3);
			o->swz[0] = o->swz[1] = o->swz[2] = o->swz[3] = name;
			o->mask = 1 << name;
		}
	}
	if (tok0 & 0x80000000u) {
		uint32_t ext;
		int mod;
		if (p >= end)
			return end;
		ext = t[p++];
		mod = (int)((ext >> 6) & 0xff);
		if (mod == 1)
			o->neg = 1;
		else if (mod == 2)
			o->absn = 1;
		else if (mod == 3) {
			o->neg = 1;
			o->absn = 1;
		}
	}
	dim = (int)((tok0 >> 20) & 3);
	o->nidx = dim;
	for (c = 0; c < dim; c++) {
		if (parse_index(t, &p, end, c, tok0, o))
			return end;
	}
	if (o->type == T_IMM32) {
		int n = (ncomp == 1) ? 1 : 4;
		for (c = 0; c < n && p < end; c++) {
			uint32_t u = t[p++];
			memcpy(&o->imm[c], &u, 4);
		}
		if (n == 1)
			o->imm[1] = o->imm[2] = o->imm[3] = o->imm[0];
	}
	return p;
}

static int idx_val(const Exec *e, const Opnd *o, int dim)
{
	int v = o->idx[dim];
	if (o->rel[dim]) {
		int r = o->relreg[dim];
		int c = o->relcomp[dim];
		if (r >= 0 && r < 64)
			v += (int)e->r[r][c];
	}
	return v;
}

static void read_vec(Exec *e, const Opnd *o, float dst[4])
{
	float src[4];
	int i, a, b;
	src[0] = src[1] = src[2] = src[3] = 0.0f;
	if (o->type == T_IMM32) {
		memcpy(src, o->imm, 16);
	} else if (o->type == T_TEMP) {
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 64)
			memcpy(src, e->r[a], 16);
	} else if (o->type == T_INPUT) {
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 32)
			memcpy(src, e->v[a], 16);
	} else if (o->type == T_OUTPUT) {
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 32)
			memcpy(src, e->o[a], 16);
	} else if (o->type == T_CB) {
		a = idx_val(e, o, 0);
		b = (o->nidx >= 2) ? idx_val(e, o, 1) : 0;
		if (o->nidx == 1) {
			b = a;
			a = 0;
		}
		if (a >= 0 && a < 14 && e->cb[a] && (unsigned)(b * 16 + 16) <= e->cb_bytes[a])
			memcpy(src, (const char *)e->cb[a] + (size_t)b * 16, 16);
	} else if (o->type == T_ICB) {
		a = idx_val(e, o, 0);
		if (a >= 0 && (unsigned)a < e->nicb)
			memcpy(src, e->icb[a], 16);
	} else if (o->type == T_INDEXABLE_TEMP) {
		a = idx_val(e, o, 0);
		b = (o->nidx >= 2) ? idx_val(e, o, 1) : 0;
		if (a >= 0 && a < 4 && b >= 0 && b < 64)
			memcpy(src, e->x[a][b], 16);
	}
	for (i = 0; i < 4; i++) {
		float x = src[o->swz[i]];
		if (o->absn)
			x = fabsf(x);
		if (o->neg)
			x = -x;
		dst[i] = x;
	}
}

static void write_vec(Exec *e, const Opnd *o, const float src[4], int sat)
{
	float *dst = NULL;
	int a, i;
	if (o->type == T_TEMP) {
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 64)
			dst = e->r[a];
	} else if (o->type == T_OUTPUT) {
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 32)
			dst = e->o[a];
	} else if (o->type == T_INDEXABLE_TEMP) {
		int b = (o->nidx >= 2) ? idx_val(e, o, 1) : 0;
		a = idx_val(e, o, 0);
		if (a >= 0 && a < 4 && b >= 0 && b < 64)
			dst = e->x[a][b];
	}
	if (!dst)
		return;
	for (i = 0; i < 4; i++) {
		if (o->mask & (1 << i)) {
			float x = src[i];
			if (sat) {
				if (x < 0.0f)
					x = 0.0f;
				if (x > 1.0f)
					x = 1.0f;
			}
			dst[i] = x;
		}
	}
}

/* Set for any opcode the interpreter met but could not execute, so the wrapper
 * can report what a frame actually needed instead of failing silently. */
unsigned dxbc_unhandled_ops[8];

static int nz(const float v[4])
{
	return v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || v[3] != 0.0f;
}

int dxbc_exec_vs(const DxbcShader *sh, const float inputs[32][4], const void *const cb[14],
		 const unsigned cb_bytes[14], float outputs[32][4])
{
	Exec e;
	unsigned pc, nd, steps;
	int sat;

	if (!sh || !sh->shex || sh->shex_dwords < 2)
		return 0;
	/* Runs once per vertex, so it clears only what the shader can actually
	 * read back. v, cb and cb_bytes are fully overwritten just below; the
	 * immediate constant buffer and the indexable temps are 4KB each and
	 * stay untouched unless this shader declared them. Temps are cleared to
	 * the declared count rather than all 64, since a shader that reads an
	 * undeclared temp is malformed. */
	e.nicb = 0;
	e.skip = 0;
	memset(e.o, 0, sizeof(e.o));
	memset(e.r, 0, (sh->ntemps ? sh->ntemps : 64) * sizeof(e.r[0]));
	if (sh->has_icb)
		memset(e.icb, 0, sizeof(e.icb));
	if (sh->has_itemp)
		memset(e.x, 0, sizeof(e.x));
	memcpy(e.v, inputs, sizeof(e.v));
	memcpy(e.cb, cb, sizeof(e.cb));
	memcpy(e.cb_bytes, cb_bytes, sizeof(e.cb_bytes));
	nd = sh->shex[1];
	if (nd > sh->shex_dwords)
		nd = sh->shex_dwords;
	pc = 2;
	steps = 0;
	while (pc < nd) {
		uint32_t tok = sh->shex[pc];
		int op = (int)(tok & 0x7ff);
		unsigned len, i;
		Opnd dst, a, b, c;
		float va[4], vb[4], vc[4], vo[4];
		if (++steps > 100000u)
			break;

		if (op == OP_CUSTOMDATA) {
			unsigned clen = (pc + 1 < nd) ? sh->shex[pc + 1] : 2;
			if (clen < 2)
				clen = 2;
			if (pc + clen > nd)
				clen = nd - pc;
			if (((tok >> 11) & 0x1fffff) == 3 && clen > 2) {
				unsigned nvec = (clen - 2) / 4;
				unsigned maxv = (nd - pc - 2) / 4;
				if (nvec > 256)
					nvec = 256;
				if (nvec > maxv)
					nvec = maxv;
				e.nicb = nvec;
				if (nvec)
					memcpy(e.icb, sh->shex + pc + 2, nvec * 16);
			}
			pc += clen;
			continue;
		}
		len = (tok >> 24) & 0x7f;
		if (len == 0)
			len = 1;
		if (pc + len > nd)
			break;
		if (tok & 0x80000000u)
			; /* extended opcode token included in length */
		sat = (tok & 0x2000) ? 1 : 0;

		/* Opcodes 107..128 are sampling, geometry and hull/domain work a
		 * vertex shader never needs. 129..142 are ALU (rcp, bit ops) and
		 * must not be skipped: dropping an rcp leaves its destination at
		 * zero, which silently zeroes SV_Position. */
		if (op == OP_DCL_TEMPS || op == OP_DCL_GLOBAL_FLAGS || op == OP_DCL_RESOURCE ||
		    op == OP_DCL_CONSTANT_BUFFER || op == OP_DCL_SAMPLER || op == OP_DCL_INPUT ||
		    op == OP_DCL_INPUT_SGV || op == OP_DCL_INPUT_SIV || op == OP_DCL_INPUT_PS ||
		    op == OP_DCL_INPUT_PS_SGV || op == OP_DCL_INPUT_PS_SIV || op == OP_DCL_OUTPUT ||
		    op == OP_DCL_OUTPUT_SGV || op == OP_DCL_OUTPUT_SIV ||
		    op == OP_DCL_INDEXABLE_TEMP || op == OP_DCL_INDEX_RANGE ||
		    (op >= 107 && op <= 128) || op >= 143) {
			pc += len;
			continue;
		}
		if (op == OP_NOP) {
			pc += len;
			continue;
		}
		if (op == OP_RET) {
			if (!e.skip)
				break;
			pc += len;
			continue;
		}
		if (op == OP_IF) {
			unsigned q = pc + 1;
			int test_z = ((tok >> 18) & 1) == 0;
			parse_opnd(sh->shex, q, pc + len, &a);
			if (e.skip)
				e.skip++;
			else {
				read_vec(&e, &a, va);
				if (test_z ? nz(va) : !nz(va))
					e.skip = 1;
			}
			pc += len;
			continue;
		}
		if (op == OP_ELSE) {
			if (e.skip == 1)
				e.skip = 0;
			else if (e.skip == 0)
				e.skip = 1;
			pc += len;
			continue;
		}
		if (op == OP_ENDIF) {
			if (e.skip > 0)
				e.skip--;
			pc += len;
			continue;
		}
		if (e.skip) {
			pc += len;
			continue;
		}

		i = pc + 1;
		if (tok & 0x80000000u)
			i++;
		switch (op) {
		case OP_MOV:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			parse_opnd(sh->shex, i, pc + len, &a);
			read_vec(&e, &a, va);
			write_vec(&e, &dst, va, sat);
			break;
		case OP_ADD:
		case OP_MUL:
		case OP_DIV:
		case OP_MIN:
		case OP_MAX:
		case OP_AND:
		case OP_OR:
		case OP_XOR:
		case OP_IADD:
		case OP_EQ:
		case OP_NE:
		case OP_LT:
		case OP_GE:
		case OP_IEQ:
		case OP_INE:
		case OP_ILT:
		case OP_IGE:
		case OP_ULT:
		case OP_UGE:
		case OP_IMAX:
		case OP_IMIN:
		case OP_UMAX:
		case OP_UMIN:
		case OP_ISHL:
		case OP_ISHR:
		case OP_USHR:
		case OP_DP2:
		case OP_DP3:
		case OP_DP4:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			i = parse_opnd(sh->shex, i, pc + len, &a);
			parse_opnd(sh->shex, i, pc + len, &b);
			read_vec(&e, &a, va);
			read_vec(&e, &b, vb);
			for (i = 0; i < 4; i++) {
				int ia, ib;
				memcpy(&ia, &va[i], 4);
				memcpy(&ib, &vb[i], 4);
				switch (op) {
				case OP_IMAX: {
					int r = ia > ib ? ia : ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_IMIN: {
					int r = ia < ib ? ia : ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_UMAX: {
					unsigned r = (unsigned)ia > (unsigned)ib ? (unsigned)ia
										: (unsigned)ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_UMIN: {
					unsigned r = (unsigned)ia < (unsigned)ib ? (unsigned)ia
										: (unsigned)ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_ISHL: {
					int r = ia << ((unsigned)ib & 31u);
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_ISHR: {
					int r = ia >> ((unsigned)ib & 31u);
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_USHR: {
					unsigned r = (unsigned)ia >> ((unsigned)ib & 31u);
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_ADD:
					vo[i] = va[i] + vb[i];
					break;
				case OP_MUL:
					vo[i] = va[i] * vb[i];
					break;
				case OP_DIV:
					vo[i] = vb[i] != 0.0f ? va[i] / vb[i] : 0.0f;
					break;
				case OP_MIN:
					vo[i] = va[i] < vb[i] ? va[i] : vb[i];
					break;
				case OP_MAX:
					vo[i] = va[i] > vb[i] ? va[i] : vb[i];
					break;
				case OP_AND: {
					int r = ia & ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_OR: {
					int r = ia | ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_XOR: {
					int r = ia ^ ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_IADD: {
					int r = ia + ib;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_EQ:
					vo[i] = va[i] == vb[i] ? 0x1p127f /* keep as ~true */ : 0;
					{
						uint32_t u = va[i] == vb[i] ? 0xffffffffu : 0;
						memcpy(&vo[i], &u, 4);
					}
					break;
				case OP_NE: {
					uint32_t u = va[i] != vb[i] ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_LT: {
					uint32_t u = va[i] < vb[i] ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_GE: {
					uint32_t u = va[i] >= vb[i] ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_IEQ: {
					uint32_t u = ia == ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_INE: {
					uint32_t u = ia != ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_ILT: {
					uint32_t u = ia < ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_IGE: {
					uint32_t u = ia >= ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_ULT: {
					uint32_t u = (unsigned)ia < (unsigned)ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				case OP_UGE: {
					uint32_t u = (unsigned)ia >= (unsigned)ib ? 0xffffffffu : 0;
					memcpy(&vo[i], &u, 4);
					break;
				}
				default:
					vo[i] = 0;
					break;
				}
			}
			if (op == OP_DP2 || op == OP_DP3 || op == OP_DP4) {
				float d = va[0] * vb[0] + va[1] * vb[1];
				if (op >= OP_DP3)
					d += va[2] * vb[2];
				if (op == OP_DP4)
					d += va[3] * vb[3];
				vo[0] = vo[1] = vo[2] = vo[3] = d;
			}
			write_vec(&e, &dst, vo, sat);
			break;
		case OP_IMUL:
		case OP_UMUL:
		case OP_UDIV: {
			/* Two destinations: hi/lo for the multiplies, quotient/
			 * remainder for udiv. Either may be null. */
			Opnd dst2;
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			i = parse_opnd(sh->shex, i, pc + len, &dst2);
			i = parse_opnd(sh->shex, i, pc + len, &a);
			parse_opnd(sh->shex, i, pc + len, &b);
			read_vec(&e, &a, va);
			read_vec(&e, &b, vb);
			for (i = 0; i < 4; i++) {
				int ia, ib;
				memcpy(&ia, &va[i], 4);
				memcpy(&ib, &vb[i], 4);
				if (op == OP_UDIV) {
					unsigned ua = (unsigned)ia, ub = (unsigned)ib;
					unsigned q = ub ? ua / ub : 0xffffffffu;
					unsigned rem = ub ? ua % ub : ua;
					memcpy(&vo[i], &q, 4);
					memcpy(&vb[i], &rem, 4);
				} else if (op == OP_IMUL) {
					long long p = (long long)ia * (long long)ib;
					int hi = (int)((unsigned long long)p >> 32);
					int lo = (int)(unsigned)p;
					memcpy(&vo[i], &hi, 4);
					memcpy(&vb[i], &lo, 4);
				} else {
					unsigned long long p = (unsigned long long)(unsigned)ia *
							       (unsigned long long)(unsigned)ib;
					unsigned hi = (unsigned)(p >> 32);
					unsigned lo = (unsigned)p;
					memcpy(&vo[i], &hi, 4);
					memcpy(&vb[i], &lo, 4);
				}
			}
			if (dst.type != T_NULL)
				write_vec(&e, &dst, vo, sat);
			if (dst2.type != T_NULL)
				write_vec(&e, &dst2, vb, sat);
			break;
		}
		case OP_MAD:
		case OP_IMAD:
		case OP_UMAD:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			i = parse_opnd(sh->shex, i, pc + len, &a);
			i = parse_opnd(sh->shex, i, pc + len, &b);
			parse_opnd(sh->shex, i, pc + len, &c);
			read_vec(&e, &a, va);
			read_vec(&e, &b, vb);
			read_vec(&e, &c, vc);
			for (i = 0; i < 4; i++) {
				if (op == OP_MAD)
					vo[i] = va[i] * vb[i] + vc[i];
				else {
					int ia, ib, ic, r;
					memcpy(&ia, &va[i], 4);
					memcpy(&ib, &vb[i], 4);
					memcpy(&ic, &vc[i], 4);
					r = ia * ib + ic;
					memcpy(&vo[i], &r, 4);
				}
			}
			write_vec(&e, &dst, vo, sat);
			break;
		case OP_MOVC:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			i = parse_opnd(sh->shex, i, pc + len, &a);
			i = parse_opnd(sh->shex, i, pc + len, &b);
			parse_opnd(sh->shex, i, pc + len, &c);
			read_vec(&e, &a, va);
			read_vec(&e, &b, vb);
			read_vec(&e, &c, vc);
			for (i = 0; i < 4; i++) {
				uint32_t u;
				memcpy(&u, &va[i], 4);
				vo[i] = u ? vb[i] : vc[i];
			}
			write_vec(&e, &dst, vo, sat);
			break;
		case OP_FRC:
		case OP_RSQ:
		case OP_SQRT:
		case OP_EXP:
		case OP_LOG:
		case OP_FTOI:
		case OP_FTOU:
		case OP_ITOF:
		case OP_UTOF:
		case OP_INEG:
		case OP_NOT:
		case OP_ROUND_Z:
		case OP_ROUND_NI:
		case OP_ROUND_PI:
		case OP_ROUND_NE:
		case OP_RCP:
		case OP_F32TOF16:
		case OP_F16TOF32:
		case OP_COUNTBITS:
		case OP_FIRSTBIT_HI:
		case OP_FIRSTBIT_LO:
		case OP_BFREV:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			parse_opnd(sh->shex, i, pc + len, &a);
			read_vec(&e, &a, va);
			for (i = 0; i < 4; i++) {
				int ia;
				memcpy(&ia, &va[i], 4);
				switch (op) {
				case OP_RCP:
					vo[i] = va[i] != 0.0f ? 1.0f / va[i] : 0.0f;
					break;
				case OP_COUNTBITS: {
					unsigned u = (unsigned)ia, n2 = 0;
					while (u) {
						n2 += u & 1u;
						u >>= 1;
					}
					memcpy(&vo[i], &n2, 4);
					break;
				}
				case OP_FIRSTBIT_HI: {
					unsigned u = (unsigned)ia, r = 0xffffffffu, k;
					for (k = 0; k < 32; k++)
						if (u & (1u << (31 - k))) {
							r = k;
							break;
						}
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_FIRSTBIT_LO: {
					unsigned u = (unsigned)ia, r = 0xffffffffu, k;
					for (k = 0; k < 32; k++)
						if (u & (1u << k)) {
							r = k;
							break;
						}
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_BFREV: {
					unsigned u = (unsigned)ia, r = 0, k;
					for (k = 0; k < 32; k++)
						if (u & (1u << k))
							r |= 1u << (31 - k);
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_F32TOF16:
				case OP_F16TOF32:
					vo[i] = va[i];
					break;
				case OP_FRC:
					vo[i] = va[i] - floorf(va[i]);
					break;
				case OP_RSQ:
					vo[i] = va[i] > 0.0f ? 1.0f / sqrtf(va[i]) : 0.0f;
					break;
				case OP_SQRT:
					vo[i] = va[i] > 0.0f ? sqrtf(va[i]) : 0.0f;
					break;
				case OP_EXP:
					vo[i] = exp2f(va[i]);
					break;
				case OP_LOG:
					vo[i] = va[i] > 0.0f ? log2f(va[i]) : 0.0f;
					break;
				case OP_FTOI: {
					int r = (int)va[i];
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_FTOU: {
					unsigned r = (unsigned)va[i];
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_ITOF:
					vo[i] = (float)ia;
					break;
				case OP_UTOF:
					vo[i] = (float)(unsigned)ia;
					break;
				case OP_INEG: {
					int r = -ia;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_NOT: {
					int r = ~ia;
					memcpy(&vo[i], &r, 4);
					break;
				}
				case OP_ROUND_Z:
					vo[i] = va[i] < 0.0f ? ceilf(va[i]) : floorf(va[i]);
					break;
				case OP_ROUND_NI:
					vo[i] = floorf(va[i]);
					break;
				case OP_ROUND_PI:
					vo[i] = ceilf(va[i]);
					break;
				case OP_ROUND_NE:
					vo[i] = roundf(va[i]);
					break;
				default:
					vo[i] = va[i];
					break;
				}
			}
			write_vec(&e, &dst, vo, sat);
			break;
		case OP_SINCOS:
			i = parse_opnd(sh->shex, i, pc + len, &dst);
			i = parse_opnd(sh->shex, i, pc + len, &b);
			parse_opnd(sh->shex, i, pc + len, &a);
			read_vec(&e, &a, va);
			for (i = 0; i < 4; i++) {
				vo[i] = sinf(va[i]);
				vb[i] = cosf(va[i]);
			}
			if (dst.type != T_NULL)
				write_vec(&e, &dst, vo, sat);
			if (b.type != T_NULL)
				write_vec(&e, &b, vb, sat);
			break;
		default:
			if (op >= 0 && op < 256)
				dxbc_unhandled_ops[op >> 5] |= 1u << (op & 31);
			break;
		}
		pc += len;
	}
	memcpy(outputs, e.o, sizeof(e.o));
	return 1;
}
