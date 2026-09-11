#ifndef RBO_PAC_H
#define RBO_PAC_H

/*
 * PAC archive API (PC FUN_00425c90 / FUN_00426b60).
 *
 * Header dword is XOR-decrypted with key 0xE3DF59AC after CreateFile/ReadFile.
 * Packs: SE.pac, BG01/02.pac, DATA01/02.pac, ETC.pac, CG.pac, Ex1/2/3Disc.PAC.
 *
 * Stub only — no real TOC/decompress yet. Prefer SD TPLs via rbo_fs for now.
 */

#include <gctypes.h>

#define RBO_PAC_XOR_KEY 0xE3DF59ACu

typedef struct RboPac RboPac;

void rbo_pac_init(void);

/* Open pack from SD path ("RBO/Data/CG.pac"). Returns NULL until implemented. */
RboPac *rbo_pac_open(const char *vol, const char *path);
void rbo_pac_close(RboPac *pac);

u32 rbo_pac_entry_count(const RboPac *pac);

/* Read entry by index into caller buffer; returns bytes copied, 0 on stub/fail. */
u32 rbo_pac_read_entry(RboPac *pac, u32 index, void *dst, u32 dst_cap);

/* Decrypt helper matching PC: value ^ RBO_PAC_XOR_KEY */
u32 rbo_pac_xor_dword(u32 enc);

#endif /* RBO_PAC_H */
