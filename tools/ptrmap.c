/* ptrmap OUT SLOTDIR SLOTDIR... - which words of default.exe's and dat112.bin's
 * images are not pointers into a module, learned from saves of the same game
 * point taken with modules at different bases, written as d3d9sw_ptrmap.bin
 * for the load's D3D9SW_PTRMAP=1.
 *
 * For every pair of saves and every image word both hold:
 *   - the same value, and that value is not inside a module that kept its base
 *     between the two saves: a vote for data;
 *   - values that differ by exactly the move of the module the first one points
 *     into: a vote for pointer;
 *   - anything else: noise (the word changes from run to run).
 * A word is marked data only with a data vote, no pointer vote and no noise.
 *
 * Build: zig cc -O2 -target x86_64-windows-gnu -o ptrmap.exe ptrmap.c */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 1024
#define MAXM 256
#define MAXS 16

typedef struct { unsigned base, size; unsigned long long off; } Reg;
typedef struct { unsigned lo, hi; char name[64]; } Mod;
typedef struct {
	char dir[MAX_PATH];
	Reg r[MAXR]; int nr;
	Mod m[MAXM]; int nm;
	const unsigned char *bin;
} Slot;

static Slot S[MAXS];
static int ns;
static const char *const want[] = { "default.exe", "dat112.bin" };
#define NW (sizeof(want) / sizeof(want[0]))

static int load(Slot *s, const char *dir)
{
	char path[MAX_PATH], line[1024];
	FILE *f;
	HANDLE h, mh;

	strcpy(s->dir, dir);
	sprintf(path, "%s\\d3d9sw_slot0.regions", dir);
	if (!(f = fopen(path, "r"))) { fprintf(stderr, "%s: no regions\n", dir); return 0; }
	while (fgets(line, sizeof(line), f)) {
		unsigned a, b, c;
		unsigned long long off;
		char name[64];

		if (sscanf(line, "# module %x %x %63s", &a, &b, name) == 3 && s->nm < MAXM) {
			s->m[s->nm].lo = a; s->m[s->nm].hi = b;
			strcpy(s->m[s->nm].name, name);
			s->nm++;
		} else if (line[0] != '#' && sscanf(line, "%x %x %llx %x", &a, &b, &off, &c) == 4 &&
			   s->nr < MAXR) {
			s->r[s->nr].base = a; s->r[s->nr].size = b; s->r[s->nr].off = off;
			s->nr++;
		}
	}
	fclose(f);
	sprintf(path, "%s\\d3d9sw_slot0.bin", dir);
	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "%s: no bin\n", dir); return 0; }
	mh = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
	s->bin = mh ? (const unsigned char *)MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0) : NULL;
	if (!s->bin) { fprintf(stderr, "%s: cannot map bin\n", dir); return 0; }
	return 1;
}

static const Mod *mod_named(const Slot *s, const char *name)
{
	int i;
	for (i = 0; i < s->nm; i++)
		if (!_stricmp(s->m[i].name, name))
			return &s->m[i];
	return NULL;
}

static const Mod *mod_at(const Slot *s, unsigned v)
{
	int i;
	for (i = 0; i < s->nm; i++)
		if (v >= s->m[i].lo && v < s->m[i].hi)
			return &s->m[i];
	return NULL;
}

static int word(const Slot *s, unsigned a, unsigned *out)
{
	int i;
	for (i = 0; i < s->nr; i++)
		if (a >= s->r[i].base && a - s->r[i].base + 4 <= s->r[i].size) {
			*out = *(const unsigned *)(s->bin + s->r[i].off + (a - s->r[i].base));
			return 1;
		}
	return 0;
}

int main(int argc, char **argv)
{
	FILE *o;
	unsigned k, total[NW];
	unsigned *bits[NW];
	int i, j;

	if (argc < 4) { fprintf(stderr, "usage: ptrmap OUT SLOTDIR SLOTDIR...\n"); return 1; }
	for (i = 2; i < argc && ns < MAXS; i++)
		if (load(&S[ns], argv[i]))
			ns++;
	if (ns < 2) { fprintf(stderr, "need two saves\n"); return 1; }
	for (k = 0; k < NW; k++) {
		const Mod *m0 = mod_named(&S[0], want[k]);
		unsigned size, x, nw, nd = 0, np = 0, nn = 0, nu = 0, nz = 0;

		bits[k] = NULL;
		total[k] = 0;
		if (!m0) { printf("%s: not in %s\n", want[k], S[0].dir); continue; }
		size = m0->hi - m0->lo;
		nw = size / 4;
		total[k] = nw;
		bits[k] = (unsigned *)calloc((nw + 31) / 32, 4);
		for (i = 0; i < ns; i++)
			printf("%s in %s at %08X\n", want[k], S[i].dir,
			       mod_named(&S[i], want[k]) ? mod_named(&S[i], want[k])->lo : 0);
		for (x = 0; x < nw; x++) {
			int dv = 0, pv = 0, nv = 0, seen = 0, zero = 1;

			for (i = 0; i < ns; i++) {
				const Mod *mi = mod_named(&S[i], want[k]);
				unsigned vi;

				if (!mi || mi->hi - mi->lo != size || !word(&S[i], mi->lo + x * 4, &vi))
					continue;
				seen++;
				if (vi)
					zero = 0;
				for (j = i + 1; j < ns; j++) {
					const Mod *mj = mod_named(&S[j], want[k]), *ti, *tj;
					unsigned vj;

					if (!mj || mj->hi - mj->lo != size ||
					    !word(&S[j], mj->lo + x * 4, &vj))
						continue;
					ti = mod_at(&S[i], vi);
					tj = ti ? mod_named(&S[j], ti->name) : NULL;
					if (vi == vj) {
						if (!(ti && tj && ti->lo == tj->lo))
							dv++;
					} else if (ti && tj && ti->lo != tj->lo &&
						   vj - vi == tj->lo - ti->lo) {
						pv++;
					} else {
						nv++;
					}
				}
			}
			if (!seen)
				nu++;
			else if (zero)
				nz++;
			if (dv && !pv && !nv) {
				bits[k][x >> 5] |= 1u << (x & 31);
				nd++;
			} else if (pv && !nv && !dv) {
				np++;
			} else if (nv || (pv && dv)) {
				nn++;
			}
		}
		printf("%s: %u words - %u data (%u of them zero everywhere), %u pointer, %u noisy, "
		       "%u not saved, %u with no evidence\n",
		       want[k], nw, nd, nz, np, nn, nu, nw - nd - np - nn - nu);
	}
	if (!(o = fopen(argv[1], "wb"))) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
	fwrite("PTRMAP1", 1, 8, o);
	{
		unsigned n = 0;
		for (k = 0; k < NW; k++)
			n += bits[k] != NULL;
		fwrite(&n, 4, 1, o);
	}
	for (k = 0; k < NW; k++) {
		char name[32];
		unsigned size;

		if (!bits[k])
			continue;
		memset(name, 0, sizeof(name));
		strncpy(name, want[k], sizeof(name) - 1);
		size = mod_named(&S[0], want[k])->hi - mod_named(&S[0], want[k])->lo;
		fwrite(name, 1, 32, o);
		fwrite(&size, 4, 1, o);
		fwrite(&total[k], 4, 1, o);
		fwrite(bits[k], 4, (total[k] + 31) / 32, o);
	}
	fclose(o);
	printf("wrote %s\n", argv[1]);
	return 0;
}
