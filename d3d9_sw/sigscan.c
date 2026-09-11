/* Does rabiribi.exe on disk match rabiribi.exe in memory?
 *
 * The crash-chain probe read bytes from the file that disagreed with what cdb
 * disassembled live, which would mean the image is packed and no static work is
 * possible. That is a large conclusion to draw from one address, and the
 * cheaper explanation is that the RVA arithmetic is wrong.
 *
 * decoder_patch_once scans live memory for a fixed nine-byte signature and
 * finds it, so the same bytes must exist on disk if the file matches memory.
 * Searching the whole file for them settles which of the two it is, and the
 * offset it lands at converts back to an RVA that can be checked against the
 * section table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	static const unsigned char sig[] = { 0x83, 0xBE, 0x54, 0x04, 0x00,
					     0x00, 0x00, 0x75 };
	FILE *f = fopen(argc > 1 ? argv[1] : "rabiribi.exe", "rb");
	unsigned char *buf;
	long len, i;
	unsigned pe, nsec, opt, s, hits = 0;

	if (!f)
		return 1;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(len);
	if (fread(buf, 1, len, f) != (size_t)len)
		return 1;
	fclose(f);

	pe = buf[0x3C] | (buf[0x3D] << 8) | (buf[0x3E] << 16) | (buf[0x3F] << 24);
	nsec = buf[pe + 6] | (buf[pe + 7] << 8);
	opt = buf[pe + 20] | (buf[pe + 21] << 8);

	printf("section table (raw -> rva):\n");
	for (s = 0; s < nsec; s++) {
		long o = pe + 24 + opt + s * 40;
		unsigned vsz = *(unsigned *)(buf + o + 8);
		unsigned va = *(unsigned *)(buf + o + 12);
		unsigned rsz = *(unsigned *)(buf + o + 16);
		unsigned raw = *(unsigned *)(buf + o + 20);
		char nm[9];

		memcpy(nm, buf + o, 8);
		nm[8] = 0;
		printf("  %-8s rva %08X vsz %08X  raw %08X rsz %08X\n", nm, va, vsz,
		       raw, rsz);
	}

	printf("\nsearching %ld bytes for the decoder signature:\n", len);
	for (i = 0; i + (long)sizeof(sig) <= len; i++) {
		if (memcmp(buf + i, sig, sizeof(sig)) != 0)
			continue;
		hits++;
		printf("  found at file offset %08lX", i);
		for (s = 0; s < nsec; s++) {
			long o = pe + 24 + opt + s * 40;
			unsigned va = *(unsigned *)(buf + o + 12);
			unsigned rsz = *(unsigned *)(buf + o + 16);
			unsigned raw = *(unsigned *)(buf + o + 20);

			if ((unsigned)i >= raw && (unsigned)i < raw + rsz) {
				printf("  -> rva %08X", va + ((unsigned)i - raw));
				break;
			}
		}
		printf("\n");
		if (hits > 8)
			break;
	}
	if (!hits)
		printf("  NOT PRESENT - the file does not contain what memory does\n");
	return 0;
}
