#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "rbo_mem.h"

static u32 s_tracked;

void rbo_mem_init(void)
{
	s_tracked = 0;
}

void *rbo_mem_alloc32(u32 size)
{
	void *p;

	if (size == 0)
		return NULL;
	p = memalign(32, (size_t)size);
	if (p)
		s_tracked += size;
	return p;
}

void rbo_mem_free(void *ptr)
{
	/* Size tracking is best-effort; real free does not know size here. */
	if (ptr)
		free(ptr);
}

void rbo_mem_flush(void *ptr, u32 size)
{
	rbo_dma_flush(ptr, size);
}

u32 rbo_mem_bytes_tracked(void)
{
	return s_tracked;
}
