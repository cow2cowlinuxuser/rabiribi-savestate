#ifndef RBO_DMA_H
#define RBO_DMA_H

/*
 * DMA / cache scaffolding for GC texture & buffer uploads.
 *
 * PC RBO uploaded pixels into DirectDraw surfaces; on GC, CPU-written
 * buffers must be flushed before GX/TPL/DMA sees them.
 *
 * Live pattern already used conceptually with SD loads:
 *   buf = memalign(32, size);
 *   fread(...);
 *   DCFlushRange(buf, size);   // required before TPL_Open / GX_InitTexObj
 *
 * No ARAM/FIFO DMA engine here yet — just the cache contract.
 */

#include <gccore.h>
#include <ogc/cache.h>

/* Flush CPU cache for a buffer GX or DVD/SD DMA may consume. */
static inline void rbo_dma_flush(void *ptr, u32 size)
{
	if (ptr && size)
		DCFlushRange(ptr, size);
}

/* Invalidate before CPU re-reads GPU/DMA-written memory (future). */
static inline void rbo_dma_invalidate(void *ptr, u32 size)
{
	if (ptr && size)
		DCInvalidateRange(ptr, size);
}

#endif /* RBO_DMA_H */
