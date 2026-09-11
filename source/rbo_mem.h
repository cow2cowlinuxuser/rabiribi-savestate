#ifndef RBO_MEM_H
#define RBO_MEM_H

/*
 * Scene memory helpers — GC MEM1 budget awareness + aligned alloc.
 * Pair with rbo_assets for "load UI TPL → use → free → load next".
 *
 * PC had large heaps + DD surfaces; GC has ~24MB MEM1 usable for game
 * (see MEM1_BUDGET in main.c). Keep one heavy TPL resident at a time.
 */

#include <gctypes.h>
#include "rbo_dma.h"

/* Soft MEM1 budget hint (bytes). Not enforced yet. */
#define RBO_MEM1_BUDGET (24u * 1024u * 1024u)

void rbo_mem_init(void);

/* 32-byte aligned alloc; returns NULL on failure. Caller rbo_mem_free(). */
void *rbo_mem_alloc32(u32 size);

void rbo_mem_free(void *ptr);

/* Alloc + DCFlushRange after caller fills (convenience for tex upload path). */
void rbo_mem_flush(void *ptr, u32 size);

/* Optional accounting stubs for future scene swap budgets. */
u32 rbo_mem_bytes_tracked(void);

#endif /* RBO_MEM_H */
