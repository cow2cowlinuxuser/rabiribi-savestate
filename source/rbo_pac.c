#include "rbo_pac.h"

struct RboPac {
	int unused;
};

void rbo_pac_init(void)
{
}

RboPac *rbo_pac_open(const char *vol, const char *path)
{
	(void)vol;
	(void)path;
	return NULL;
}

void rbo_pac_close(RboPac *pac)
{
	(void)pac;
}

u32 rbo_pac_entry_count(const RboPac *pac)
{
	(void)pac;
	return 0;
}

u32 rbo_pac_read_entry(RboPac *pac, u32 index, void *dst, u32 dst_cap)
{
	(void)pac;
	(void)index;
	(void)dst;
	(void)dst_cap;
	return 0;
}

u32 rbo_pac_xor_dword(u32 enc)
{
	return enc ^ RBO_PAC_XOR_KEY;
}
