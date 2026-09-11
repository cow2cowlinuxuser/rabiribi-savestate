/* Resolve the crash chain's call targets straight out of rabiribi.exe.
 *
 * The addresses the fault handler prints are return addresses - the instruction
 * after a call, not the entry of the thing that was called. Forcing "the same
 * pathway" by jumping to one of them would land in the middle of a function
 * with a half-built frame, which would crash for reasons that have nothing to
 * do with the restore and would look exactly like success.
 *
 * A 32-bit MSVC direct call is E8 rel32, so when the byte five before a return
 * address is E8 the callee is retaddr + rel32 exactly, no disassembly and no
 * prologue guessing. This checks that assumption against the file and prints
 * the entries, so the hook targets are derived rather than assumed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *buf;
static long len;
static unsigned image_base;
static unsigned nsec;
static struct {
	unsigned va, vsz, raw, rsz;
} sec[32];

static long rva_to_off(unsigned rva)
{
	unsigned i;

	for (i = 0; i < nsec; i++)
		if (rva >= sec[i].va && rva < sec[i].va + sec[i].vsz) {
			unsigned d = rva - sec[i].va;

			if (d >= sec[i].rsz)
				return -1;
			return (long)(sec[i].raw + d);
		}
	return -1;
}

static unsigned rd32(long off)
{
	return (unsigned)buf[off] | ((unsigned)buf[off + 1] << 8) |
	       ((unsigned)buf[off + 2] << 16) | ((unsigned)buf[off + 3] << 24);
}

static void probe(const char *what, unsigned ret_rva)
{
	long o = rva_to_off(ret_rva);
	long oc = rva_to_off(ret_rva - 5);
	unsigned target;
	int rel;

	if (o < 0 || oc < 0) {
		printf("  %-12s rva %06X  NOT MAPPED in the file\n", what, ret_rva);
		return;
	}
	if (buf[oc] != 0xE8) {
		printf("  %-12s rva %06X  byte at -5 is %02X, not E8 - not a direct "
		       "call, so the entry cannot be derived this way\n",
		       what, ret_rva, buf[oc]);
		/* Show what is actually there, so the next guess is informed. */
		printf("               bytes before: %02X %02X %02X %02X %02X | at: "
		       "%02X %02X %02X\n",
		       buf[oc], buf[oc + 1], buf[oc + 2], buf[oc + 3], buf[oc + 4],
		       buf[o], buf[o + 1], buf[o + 2]);
		return;
	}
	rel = (int)rd32(oc + 1);
	target = ret_rva + (unsigned)rel;
	printf("  %-12s rva %06X  E8 rel32 -> callee entry rva %06X  (va %08X)\n",
	       what, ret_rva, target, image_base + target);
	{
		long t = rva_to_off(target);

		if (t < 0) {
			printf("               callee entry not in the file\n");
			return;
		}
		printf("               entry bytes: %02X %02X %02X %02X %02X %02X "
		       "%02X %02X\n",
		       buf[t], buf[t + 1], buf[t + 2], buf[t + 3], buf[t + 4],
		       buf[t + 5], buf[t + 6], buf[t + 7]);
	}
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argc > 1 ? argv[1] : "rabiribi.exe", "rb");
	unsigned pe, opt, i;

	if (!f) {
		printf("cannot open the exe\n");
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len);
	if (fread(buf, 1, len, f) != (size_t)len) {
		printf("short read\n");
		return 1;
	}
	fclose(f);

	pe = rd32(0x3C);
	image_base = rd32(pe + 0x34);
	nsec = buf[pe + 6] | (buf[pe + 7] << 8);
	opt = buf[pe + 20] | (buf[pe + 21] << 8);
	if (nsec > 32)
		nsec = 32;
	for (i = 0; i < nsec; i++) {
		long s = pe + 24 + opt + i * 40;

		sec[i].vsz = rd32(s + 8);
		sec[i].va = rd32(s + 12);
		sec[i].rsz = rd32(s + 16);
		sec[i].raw = rd32(s + 20);
	}
	printf("rabiribi.exe  image base %08X, %u section(s), %ld bytes on disk\n\n",
	       image_base, nsec, len);

	printf("call sites from the two crash logs (return addresses):\n");
	probe("outer", 0x33DCE);
	probe("middle", 0x36370);
	probe("inner-a", 0x6F533); /* first crash */
	probe("inner-b", 0x6F4FB); /* second crash */
	probe("memcpy-site", 0x6E9F8);

	printf("\nthe faulting idiv itself (not a call site, shown for context):\n");
	{
		long o = rva_to_off(0x355759);

		if (o > 0)
			printf("  rva 355759  bytes: %02X %02X %02X %02X %02X %02X\n",
			       buf[o], buf[o + 1], buf[o + 2], buf[o + 3], buf[o + 4],
			       buf[o + 5]);
	}
	return 0;
}
