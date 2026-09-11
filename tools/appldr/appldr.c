/*
 * Minimal GameCube apploader: load the disc DOL and jump to it.
 * Linked at 0x81200000. The packer prepends the 0x20 disc header.
 *
 * IPL/Dolphin protocol (YAGCD 13.3 / Dolphin Boot_BS2Emu):
 *   entry(init*, main*, close*)
 *   init(report)
 *   main(dst*, len*, off*)  -> 1 = DVD read those values (byte off on GC), 0 = done
 *   close() -> DOL entrypoint in r3
 */

typedef unsigned int u32;
typedef unsigned char u8;
typedef int s32;

#define BB2_OFF 0x420u

typedef struct {
	u32 dol_off;
	u32 fst_off;
	u32 fst_len;
	u32 fst_max;
	u32 fst_addr;
	u32 user_off;
	u32 user_len;
	u32 pad;
} Bb2;

typedef struct {
	u32 text_off[7];
	u32 data_off[11];
	u32 text_addr[7];
	u32 data_addr[11];
	u32 text_size[7];
	u32 data_size[11];
	u32 bss_addr;
	u32 bss_size;
	u32 entry;
} DolHdr;

enum {
	ST_BB2 = 0,
	ST_DOLHDR,
	ST_TEXT,
	ST_DATA,
	ST_DONE
};

static Bb2 s_bb2 __attribute__((aligned(32)));
static union {
	DolHdr hdr;
	u8 raw[256];
} s_dolu __attribute__((aligned(32)));
static int s_state;
static int s_sect;
static u32 s_entry;

static u32 align32(u32 n)
{
	return (n + 31u) & ~31u;
}

static void zero(void *p, u32 n)
{
	u8 *b = (u8 *)p;
	while (n--)
		*b++ = 0;
}

static void sync_range(u32 addr, u32 size)
{
	u32 p;
	u32 e;

	if (!size)
		return;
	p = addr & ~31u;
	e = (addr + size + 31u) & ~31u;
	for (; p < e; p += 32u) {
		__asm__ volatile("dcbst 0,%0; icbi 0,%0" ::"r"(p) : "memory");
	}
	__asm__ volatile("sync; isync" ::: "memory");
}

/* IPL DMA-fills *addr after we return 1. Invalidate so that write is visible. */
static void inval_range(void *p, u32 size)
{
	u32 a;
	u32 e;

	if (!p || !size)
		return;
	a = (u32)p & ~31u;
	e = ((u32)p + size + 31u) & ~31u;
	for (; a < e; a += 32u)
		__asm__ volatile("dcbi 0,%0" ::"r"(a) : "memory");
	__asm__ volatile("sync" ::: "memory");
}

void appldr_init(void (*report)(const char *fmt, ...))
{
	(void)report;
	s_state = ST_BB2;
	s_sect = 0;
	s_entry = 0;
	zero(&s_bb2, sizeof(s_bb2));
	zero(&s_dolu, sizeof(s_dolu));
}

int appldr_main(void **addr, s32 *length, s32 *offset)
{
	int i;

	switch (s_state) {
	case ST_BB2:
		*addr = &s_bb2;
		*length = (s32)sizeof(s_bb2);
		*offset = (s32)BB2_OFF;
		inval_range(*addr, (u32)*length);
		s_state = ST_DOLHDR;
		return 1;
	case ST_DOLHDR:
		*addr = &s_dolu;
		*length = (s32)sizeof(s_dolu.raw);
		*offset = (s32)s_bb2.dol_off;
		inval_range(*addr, (u32)*length);
		s_state = ST_TEXT;
		s_sect = 0;
		return 1;
	case ST_TEXT:
		for (i = s_sect; i < 7; i++) {
			if (s_dolu.hdr.text_size[i] == 0)
				continue;
			*addr = (void *)s_dolu.hdr.text_addr[i];
			*length = (s32)align32(s_dolu.hdr.text_size[i]);
			*offset = (s32)(s_bb2.dol_off + s_dolu.hdr.text_off[i]);
			inval_range(*addr, (u32)*length);
			s_sect = i + 1;
			return 1;
		}
		s_state = ST_DATA;
		s_sect = 0;
		goto data_loop;
	case ST_DATA:
	data_loop:
		for (i = s_sect; i < 11; i++) {
			if (s_dolu.hdr.data_size[i] == 0)
				continue;
			*addr = (void *)s_dolu.hdr.data_addr[i];
			*length = (s32)align32(s_dolu.hdr.data_size[i]);
			*offset = (s32)(s_bb2.dol_off + s_dolu.hdr.data_off[i]);
			inval_range(*addr, (u32)*length);
			s_sect = i + 1;
			return 1;
		}
		s_entry = s_dolu.hdr.entry;
		if (s_dolu.hdr.bss_addr && s_dolu.hdr.bss_size)
			zero((void *)s_dolu.hdr.bss_addr, s_dolu.hdr.bss_size);
		s_state = ST_DONE;
		return 0;
	default:
		return 0;
	}
}

void *appldr_close(void)
{
	int i;

	for (i = 0; i < 7; i++) {
		if (s_dolu.hdr.text_size[i])
			sync_range(s_dolu.hdr.text_addr[i], s_dolu.hdr.text_size[i]);
	}
	for (i = 0; i < 11; i++) {
		if (s_dolu.hdr.data_size[i])
			sync_range(s_dolu.hdr.data_addr[i], s_dolu.hdr.data_size[i]);
	}
	return (void *)s_entry;
}

__attribute__((section(".text._entry")))
void _entry(void (**init)(void (*report)(const char *, ...)),
	    int (**mainfn)(void **addr, s32 *len, s32 *off),
	    void *(**close)(void))
{
	*init = appldr_init;
	*mainfn = appldr_main;
	*close = appldr_close;
}

void __eabi(void)
{
}
