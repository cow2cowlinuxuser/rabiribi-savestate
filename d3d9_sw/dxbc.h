#ifndef D3D11_SW_DXBC_H
#define D3D11_SW_DXBC_H

#include <stdint.h>

typedef struct DxbcSig {
	char name[32];
	unsigned sem_index;
	unsigned sysval;
	unsigned reg;
	unsigned mask;
} DxbcSig;

typedef struct DxbcShader {
	const uint32_t *shex;
	unsigned shex_dwords;
	DxbcSig isgn[32];
	unsigned n_isgn;
	DxbcSig osgn[32];
	unsigned n_osgn;
	int type; /* 0 = pixel, 1 = vertex */
	/* Which register files this shader actually touches. The execution state
	 * is around 10KB and was being cleared in full for every vertex; these
	 * let the clear cover only what the shader can read. Set by dxbc_parse. */
	int has_icb;
	int has_itemp;
	unsigned ntemps;
} DxbcShader;

int dxbc_parse(const void *bytes, unsigned nbytes, DxbcShader *out);
int dxbc_find_sem(const DxbcSig *sigs, unsigned n, const char *name, unsigned idx);
int dxbc_find_sem_any(const DxbcSig *sigs, unsigned n, const char *name);
int dxbc_find_sysval_pos(const DxbcSig *sigs, unsigned n);
/* inputs[reg][4], outputs[reg][4]. cb[slot] is bytes of the bound constant buffer. */
int dxbc_exec_vs(const DxbcShader *sh, const float inputs[32][4], const void *const cb[14],
		 const unsigned cb_bytes[14], float outputs[32][4]);

/* Bitmask, indexed by opcode, of instructions the interpreter skipped. */
extern unsigned dxbc_unhandled_ops[8];

#endif
