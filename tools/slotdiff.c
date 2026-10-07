/* slotdiff RUNDIR... - compare slot 0 across launches word by word.
 *
 * Each RUNDIR holds d3d9sw_slot0.json and .bin. Regions are matched by base
 * address; the comparison covers the bytes all runs have. A differing word is
 * classed by what it holds in every run: a pointer to the same module or region
 * offset (moved), a pointer that lands somewhere different, a pointer outside
 * the save, a small value (id, handle, count), or other data. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 512
#define MAXM 256
#define MAXRUN 16

typedef struct { unsigned base, size; unsigned long long off; char kind[16], in[64]; } Reg;
typedef struct { unsigned base, size; char name[64]; } Mod;
typedef struct {
	Reg r[MAXR]; int nr;
	Mod m[MAXM]; int nm;
	const unsigned char *bin; unsigned long long binsz;
} Run;

static Run runs[MAXRUN];
static int nrun;

static int field(const char *line, const char *key, char *out, int cap)
{
	char pat[32];
	const char *p, *e;
	sprintf(pat, "\"%s\":\"", key);
	if (!(p = strstr(line, pat))) return 0;
	p += strlen(pat);
	if (!(e = strchr(p, '"')) || e - p >= cap) return 0;
	memcpy(out, p, e - p); out[e - p] = 0;
	return 1;
}

static void load(Run *R, const char *dir)
{
	char path[MAX_PATH], line[4096], v[128];
	FILE *f;
	HANDLE h, mh;
	LARGE_INTEGER sz;

	sprintf(path, "%s\\d3d9sw_slot0.json", dir);
	if (!(f = fopen(path, "r"))) { fprintf(stderr, "no %s\n", path); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "\"k\":\"region\"") && R->nr < MAXR) {
			Reg *g = &R->r[R->nr++];
			field(line, "base", v, sizeof(v)); g->base = strtoul(v, 0, 16);
			field(line, "size", v, sizeof(v)); g->size = strtoul(v, 0, 16);
			field(line, "off", v, sizeof(v)); g->off = _strtoui64(v, 0, 16);
			field(line, "kind", g->kind, sizeof(g->kind));
			field(line, "in", g->in, sizeof(g->in));
		} else if (strstr(line, "\"k\":\"module\"") && R->nm < MAXM) {
			Mod *m = &R->m[R->nm++];
			field(line, "base", v, sizeof(v)); m->base = strtoul(v, 0, 16);
			field(line, "size", v, sizeof(v)); m->size = strtoul(v, 0, 16);
			field(line, "name", m->name, sizeof(m->name));
		}
	}
	fclose(f);
	sprintf(path, "%s\\d3d9sw_slot0.bin", dir);
	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "no %s\n", path); exit(1); }
	GetFileSizeEx(h, &sz);
	mh = CreateFileMappingA(h, 0, PAGE_READONLY, 0, 0, 0);
	R->bin = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
	R->binsz = sz.QuadPart;
	if (!R->bin) { fprintf(stderr, "map %s failed\n", path); exit(1); }
}

/* Fixed-address regions keep their base across launches, so base+offset names
 * the same place; anything else is named by kind and offset only. */
static int is_fixed(unsigned base)
{
	return base == 0x0D000000 || base == 0x0D800000 || base == 0x10000000 ||
	       base == 0x14000000 || (base >= 0x5F000000 && base < 0x61000000);
}

static void label(const Run *R, unsigned v, char *out)
{
	int i;
	for (i = 0; i < R->nm; i++)
		if (v >= R->m[i].base && v < R->m[i].base + R->m[i].size) {
			sprintf(out, "%s+%X", R->m[i].name, v - R->m[i].base); return;
		}
	for (i = 0; i < R->nr; i++)
		if (v >= R->r[i].base && v < R->r[i].base + R->r[i].size) {
			if (is_fixed(R->r[i].base)) sprintf(out, "@%08X", v);
			else sprintf(out, "%s+%X", R->r[i].kind, v - R->r[i].base);
			return;
		}
	strcpy(out, "unsaved");
}

static const Reg *find(const Run *R, unsigned base)
{
	int i;
	for (i = 0; i < R->nr; i++) if (R->r[i].base == base) return &R->r[i];
	return NULL;
}

enum { C_MOVED, C_ELSEWHERE, C_UNSAVED, C_SMALL, C_DATA, NC };
static const char *cname[NC] = { "ptr-moved", "ptr-elsewhere", "ptr-unsaved", "small", "data" };

/* -shift: for each fixed region, how much of run k's content exists anywhere in
 * the first run. Every 32-byte window of the first run, at every 4-byte step, is
 * hashed; run k's non-zero 32-byte blocks are looked up. */
#define HBITS 26
static unsigned long long *ht;

static unsigned long long h32(const unsigned *p)
{
	unsigned long long h = 1469598103934665603ull;
	int i;
	for (i = 0; i < 8; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h | 1;
}

static void ht_add(unsigned long long h)
{
	unsigned long long m = (1ull << HBITS) - 1, i = (h >> 7) & m;
	while (ht[i] && ht[i] != h) i = (i + 1) & m;
	ht[i] = h;
}

static int ht_has(unsigned long long h)
{
	unsigned long long m = (1ull << HBITS) - 1, i = (h >> 7) & m;
	while (ht[i]) { if (ht[i] == h) return 1; i = (i + 1) & m; }
	return 0;
}

static int nonzero(const unsigned *p)
{
	int i;
	for (i = 0; i < 8; i++) if (p[i]) return 1;
	return 0;
}

/* 32 bytes of run R at address a, if one saved region holds all of them. */
static const unsigned *at(const Run *R, unsigned a)
{
	int i;
	for (i = 0; i < R->nr; i++)
		if (a >= R->r[i].base && a + 32 <= R->r[i].base + R->r[i].size)
			return (const unsigned *)(R->bin + R->r[i].off + (a - R->r[i].base));
	return NULL;
}

/* Every saved region inside each span counts, so an arena committed in pieces
 * is compared whole. Run 1's content is indexed up to 48 MB per span. */
static void shift_mode(void)
{
	static const struct { unsigned lo, hi; const char *name; } span[] = {
		{ 0x0D000000, 0x0E000000, "runtime heaps" },
		{ 0x10000000, 0x14000000, "small arena" },
		{ 0x14000000, 0x5F000000, "big arena" },
		{ 0x5F400000, 0x5FC00000, "voice arena" },
	};
	int s, k, i;
	ht = calloc(1ull << HBITS, 8);
	for (s = 0; s < 4; s++) {
		unsigned long long idx = 0, cap = 48ull << 20;
		memset(ht, 0, (1ull << HBITS) * 8);
		for (i = 0; i < runs[0].nr && idx < cap; i++) {
			const Reg *g = &runs[0].r[i];
			const unsigned *p = (const unsigned *)(runs[0].bin + g->off);
			unsigned w;
			if (g->base < span[s].lo || g->base >= span[s].hi) continue;
			for (w = 0; w + 8 <= g->size / 4 && idx < cap; w++, idx += 4)
				if (nonzero(p + w)) ht_add(h32(p + w));
		}
		if (!idx) continue;
		printf("%08X-%08X %s (%llu MB of run 1 indexed)\n", span[s].lo, span[s].hi, span[s].name, idx >> 20);
		for (k = 1; k < nrun; k++) {
			unsigned long long zero = 0, same = 0, moved = 0, none = 0, t;
			for (i = 0; i < runs[k].nr; i++) {
				const Reg *g = &runs[k].r[i];
				const unsigned *p = (const unsigned *)(runs[k].bin + g->off), *p0;
				unsigned w;
				if (g->base < span[s].lo || g->base >= span[s].hi) continue;
				for (w = 0; w + 8 <= g->size / 4; w += 8) {
					if (!nonzero(p + w)) zero++;
					else if ((p0 = at(&runs[0], g->base + w * 4)) && !memcmp(p + w, p0, 32)) same++;
					else if (ht_has(h32(p + w))) moved++;
					else none++;
				}
			}
			t = zero + same + moved + none;
			if (!t) continue;
			printf("  run %2d: %6.1f%% zero  %6.1f%% same place  %6.1f%% found elsewhere  %6.1f%% not in run 1   (%llu MB)\n", k + 1,
			       100.0 * zero / t, 100.0 * same / t, 100.0 * moved / t, 100.0 * none / t, (t * 32) >> 20);
		}
	}
}

/* -blocks REF SAME OTHER...: every block in REF's gh_ledger.txt, paired by
 * identity (role, site, size, ordinal) with the same block in the other saves.
 * SAME is a later save from REF's own launch, so a block that differs there is
 * rewritten while the game runs; one that differs only in OTHER launches is
 * fixed per launch. Differing words are split into pointer-like (both values
 * land inside their own save) and data. Totals are per (role, site). */
#define LCAP 32768
typedef struct { unsigned long long key; unsigned addr, size, site; int role; } LEnt;
typedef struct { LEnt e[LCAP]; int n; char roles[64][48]; int nroles; } Led;
static Led leds[MAXRUN];

static unsigned long long fnv(const char *s, unsigned long long h)
{
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
	return h;
}

static void led_load(Led *L, const char *dir)
{
	char path[MAX_PATH], line[256], name[48], role[16];
	unsigned r, addr, size, site, ord;
	FILE *f;
	sprintf(path, "%s\\gh_ledger.txt", dir);
	if (!(f = fopen(path, "r"))) { fprintf(stderr, "no %s\n", path); exit(1); }
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "# role R%u %47s", &r, name) == 2 && r < 64) {
			strcpy(L->roles[r], name);
			if ((int)r >= L->nroles) L->nroles = r + 1;
		} else if (line[0] != '#' && L->n < LCAP &&
			   sscanf(line, "%x %x %x %u %15s", &addr, &size, &site, &ord, role) == 5) {
			LEnt *e = &L->e[L->n++];
			char k[96];
			r = (unsigned)atoi(role + 1);
			sprintf(k, "%s|%X|%X|%u", r < 64 ? L->roles[r] : "?", site, size, ord);
			e->key = fnv(k, 1469598103934665603ull);
			e->addr = addr; e->size = size; e->site = site; e->role = (int)r;
		}
	}
	fclose(f);
}

static const LEnt *led_find(const Led *L, unsigned long long key)
{
	int i;
	for (i = 0; i < L->n; i++) if (L->e[i].key == key) return &L->e[i];
	return NULL;
}

static const unsigned char *at_n(const Run *R, unsigned a, unsigned n)
{
	int i;
	for (i = 0; i < R->nr; i++)
		if (a >= R->r[i].base && a + n <= R->r[i].base + R->r[i].size)
			return R->bin + R->r[i].off + (a - R->r[i].base);
	return NULL;
}

static int in_save(const Run *R, unsigned v)
{
	int i;
	if (v < 0x10000) return 0;
	for (i = 0; i < R->nr; i++) if (v >= R->r[i].base && v < R->r[i].base + R->r[i].size) return 1;
	for (i = 0; i < R->nm; i++) if (v >= R->m[i].base && v < R->m[i].base + R->m[i].size) return 1;
	return 0;
}

typedef struct {
	char role[48]; unsigned site; int big;
	unsigned long long blocks, bytes, words, same_diff, other_diff, other_ptr, other_words, missing;
} Agg;
static Agg agg[8192];
static int nagg;

static Agg *agg_of(const char *role, unsigned site, int big)
{
	int i;
	for (i = 0; i < nagg; i++)
		if (agg[i].site == site && agg[i].big == big && !strcmp(agg[i].role, role)) return &agg[i];
	if (nagg >= 8192) return &agg[8191];
	strcpy(agg[nagg].role, role); agg[nagg].site = site; agg[nagg].big = big;
	return &agg[nagg++];
}

static int agg_cmp(const void *a, const void *b)
{
	const Agg *x = a, *y = b;
	unsigned long long dx = x->other_diff - x->other_ptr, dy = y->other_diff - y->other_ptr;
	if (x->big != y->big) return y->big - x->big;
	return dx < dy ? 1 : dx > dy ? -1 : 0;
}

static void blocks_mode(void)
{
	int i, k;
	unsigned long long tot[2][6] = { { 0 } };
	for (i = 0; i < leds[0].n; i++) {
		const LEnt *e = &leds[0].e[i];
		const unsigned *p0 = (const unsigned *)at_n(&runs[0], e->addr, e->size);
		const char *rn = e->role < 64 ? leds[0].roles[e->role] : "?";
		Agg *a;
		unsigned w, nw = e->size / 4;
		if (!p0 || !nw) continue;
		a = agg_of(rn, e->site, e->addr >= 0x14000000);
		a->blocks++; a->bytes += e->size; a->words += nw;
		for (k = 1; k < nrun; k++) {
			const LEnt *o = led_find(&leds[k], e->key);
			const unsigned *p = o ? (const unsigned *)at_n(&runs[k], o->addr, e->size) : NULL;
			if (!p) { a->missing++; continue; }
			for (w = 0; w < nw; w++) {
				if (p[w] == p0[w]) continue;
				if (k == 1) a->same_diff++;
				else {
					a->other_diff++;
					if (in_save(&runs[0], p0[w]) && in_save(&runs[k], p[w])) a->other_ptr++;
				}
			}
			if (k > 1) a->other_words += nw;
		}
	}
	qsort(agg, nagg, sizeof(Agg), agg_cmp);
	printf("arena role site blocks KB | changed-in-launch%% | other-launch data%% ptr%% | class\n");
	for (i = 0; i < nagg; i++) {
		Agg *a = &agg[i];
		double sd = a->words ? 100.0 * a->same_diff / a->words : 0;
		double od = a->other_words ? 100.0 * (a->other_diff - a->other_ptr) / a->other_words : 0;
		double op = a->other_words ? 100.0 * a->other_ptr / a->other_words : 0;
		const char *cls = sd > 0.5 ? "LIVE (rewritten while running)"
				: od > 0.5 ? "PER-LAUNCH data"
				: op > 0.5 ? "pointers only"
				: "stable";
		int c = sd > 0.5 ? 0 : od > 0.5 ? 1 : op > 0.5 ? 2 : 3;
		tot[a->big][c] += a->bytes;
		if (a->bytes >= 4096 || od > 0.5 || sd > 0.5)
			printf("%-5s %-24s %08X %5llu %8llu | %6.1f | %6.1f %6.1f | %s%s\n", a->big ? "big" : "small",
			       a->role, a->site, a->blocks, a->bytes >> 10, sd, od, op, cls,
			       a->missing ? " (some unpaired)" : "");
	}
	for (k = 0; k < 2; k++)
		printf("\n%s arena: LIVE %llu KB, PER-LAUNCH %llu KB, pointers-only %llu KB, stable %llu KB",
		       k ? "big" : "small", tot[k][0] >> 10, tot[k][1] >> 10, tot[k][2] >> 10, tot[k][3] >> 10);
	printf("\n");
}

/* -regions REF SAME OTHER...: module images paired by module+offset, stacks by
 * order and compared from the top. A differing word is "reloc" when both
 * values name the same module offset or fixed address, "ptr" when both look
 * like pointers into their save but to different things, else data. For
 * default.exe's writable pages the differing data words are listed. */
static const Reg *pair_reg(const Run *R, const Reg *g, int nth_stack)
{
	int i, n = 0;
	for (i = 0; i < R->nr; i++) {
		const Reg *h = &R->r[i];
		if (strcmp(h->kind, g->kind)) continue;
		if (!strcmp(g->kind, "image") && !strcmp(h->in, g->in)) return h;
		if (!strcmp(g->kind, "stack") && n++ == nth_stack) return h;
	}
	return NULL;
}

/* -regions with env SLOTDIFF_TALLY=<region in>: what the differing words of
 * that region point at, ref run vs the first other launch, by target class. */
static const char *g_tally;
static struct { char key[96]; unsigned n; unsigned ex0, exk; } tal[256];
static int ntal;

static void tclass(const char *l, unsigned v, char *out)
{
	const char *plus = strchr(l, '+');
	if (!strcmp(l, "unsaved")) strcpy(out, v < 0x10000 ? "small-value" : v >= 0x10000 && v < 0x7FFF0000 ? "unsaved-addr" : "big-value");
	else if (l[0] == '@') sprintf(out, "fixed %c%c", l[1], l[2]);
	else if (plus) { memcpy(out, l, plus - l); out[plus - l] = 0; }
	else strcpy(out, l);
}

static void tally_add(const char *l0, const char *lk, unsigned v0, unsigned vk)
{
	char a[48], b[48], key[96];
	int i;
	tclass(l0, v0, a); tclass(lk, vk, b);
	sprintf(key, "%-16s -> %s", a, b);
	for (i = 0; i < ntal; i++) if (!strcmp(tal[i].key, key)) break;
	if (i == ntal) { if (ntal == 256) return; strcpy(tal[ntal].key, key); tal[ntal].ex0 = v0; tal[ntal].exk = vk; ntal++; }
	tal[i].n++;
}

static void regions_mode(void)
{
	int i, k, nst = 0, listed = 0;
	unsigned long long tot[4] = { 0 };
	printf("region                         KB | in-launch%% | other: data%% ptr%% reloc%%\n");
	for (i = 0; i < runs[0].nr; i++) {
		const Reg *g = &runs[0].r[i];
		int st = !strcmp(g->kind, "stack"), idx = st ? nst++ : 0;
		unsigned long long nw = g->size / 4, sd = 0, od = 0, op = 0, orl = 0, ow = 0;
		const unsigned *p0 = (const unsigned *)(runs[0].bin + g->off);
		if (!st && strcmp(g->kind, "image")) continue;
		for (k = 1; k < nrun; k++) {
			const Reg *h = pair_reg(&runs[k], g, idx);
			const unsigned *p;
			unsigned n, w, a0, ak;
			if (!h) continue;
			n = (g->size < h->size ? g->size : h->size) / 4;
			a0 = st ? (unsigned)(nw - n) : 0;	/* align stacks at the top */
			ak = st ? h->size / 4 - n : 0;
			p = (const unsigned *)(runs[k].bin + h->off);
			if (k > 1) ow += n;
			for (w = 0; w < n; w++) {
				unsigned v0 = p0[a0 + w], vk = p[ak + w];
				char l0[96], lk[96];
				if (v0 == vk) continue;
				if (k == 1) { sd++; continue; }
				label(&runs[0], v0, l0); label(&runs[k], vk, lk);
				if (g_tally && !strcmp(g->in, g_tally) && k == 2) tally_add(l0, lk, v0, vk);
				if (strcmp(l0, "unsaved") && !strcmp(l0, lk)) orl++;
				else if (in_save(&runs[0], v0) && in_save(&runs[k], vk)) op++;
				else {
					od++;
					if (!st && strstr(g->in, "default.exe") && listed < 60 && k == 2) {
						listed++;
						printf("      data %s+%X: %08X vs %08X\n", g->in, w * 4, v0, vk);
					}
				}
			}
		}
		tot[0] += g->size; tot[1] += od * 4 / (nrun > 2 ? nrun - 2 : 1);
		if (sd || od || op || orl)
			printf("%-28s %5u | %6.2f | %6.2f %6.2f %6.2f\n", st ? "stack" : g->in, g->size >> 10,
			       100.0 * sd / nw, ow ? 100.0 * od / ow : 0, ow ? 100.0 * op / ow : 0, ow ? 100.0 * orl / ow : 0);
	}
	printf("\n%llu KB compared, about %llu bytes of plain data differ per other launch\n", tot[0] >> 10, tot[1]);
	if (ntal) {
		printf("\ndiffering words in %s, ref value class -> other launch value class:\n", g_tally);
		for (i = 0; i < ntal; i++)
			printf("  %7u  %s   (e.g. %08X vs %08X)\n", tal[i].n, tal[i].key, tal[i].ex0, tal[i].exk);
	}
}

int main(int argc, char **argv)
{
	int i, k, j;
	unsigned long long tot_words = 0, tot_diff = 0, tot_c[NC] = { 0 };
	FILE *det;

	if (argc >= 4 && !strcmp(argv[1], "-regions")) {
		g_tally = getenv("SLOTDIFF_TALLY");
		for (i = 2; i < argc && nrun < MAXRUN; i++) load(&runs[nrun++], argv[i]);
		regions_mode();
		return 0;
	}

	if (argc == 4 && !strcmp(argv[1], "-find")) {
		/* -find HEX RUNDIR: every aligned word equal to HEX, with the 16 words
		 * around it, so the object holding a pointer can be read. */
		unsigned v = strtoul(argv[2], 0, 16), w, n = 0;
		load(&runs[0], argv[3]);
		for (i = 0; i < runs[0].nr && n < 64; i++) {
			const Reg *g = &runs[0].r[i];
			const unsigned *p = (const unsigned *)(runs[0].bin + g->off);
			for (w = 0; w < g->size / 4 && n < 64; w++) {
				char lab[96];
				if (p[w] != v) continue;
				n++;
				label(&runs[0], g->base + w * 4, lab);
				printf("%08X (%s, region %s)\n", g->base + w * 4, lab, g->kind);
				for (k = (int)w - 8; k < (int)w + 8; k++)
					if (k >= 0 && (unsigned)k < g->size / 4)
						printf("   %+4d: %08X%s\n", (k - (int)w) * 4, p[k], k == (int)w ? "  <" : "");
			}
		}
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "-ptrs")) {
		/* -ptrs RUNDIR LO-HI...: words inside the given spans that point at
		 * saved memory outside all of them, tallied by the target's label. */
		static char tl[256][96];
		static unsigned tn[256], ex[256];
		unsigned lo[16], hi[16], ns = 0, nt = 0, w, t;
		load(&runs[0], argv[2]);
		for (i = 3; i < argc && ns < 16; i++, ns++)
			sscanf(argv[i], "%x-%x", &lo[ns], &hi[ns]);
		for (i = 0; i < runs[0].nr; i++) {
			const Reg *g = &runs[0].r[i];
			const unsigned *p = (const unsigned *)(runs[0].bin + g->off);
			for (w = 0; w < g->size / 4; w++) {
				unsigned a = g->base + w * 4, v = p[w], s, in = 0, hit = 0;
				char lab[96];
				for (s = 0; s < ns; s++) {
					if (a >= lo[s] && a < hi[s]) in = 1;
					if (v >= lo[s] && v < hi[s]) hit = 1;
				}
				if (!in || hit || v < 0x10000) continue;
				for (k = 0; k < runs[0].nr; k++)
					if (v >= runs[0].r[k].base && v - runs[0].r[k].base < runs[0].r[k].size) break;
				if (k == runs[0].nr) continue;
				label(&runs[0], v, lab);
				{ char *c = strchr(lab, '+'); if (c) *c = 0; }
				if (strstr(lab, "default.exe") == 0) {
					char tmp[96];
					snprintf(tmp, sizeof tmp, "%08X+%X %s %.30s", runs[0].r[k].base,
						 runs[0].r[k].size, runs[0].r[k].kind, lab);
					strcpy(lab, tmp);
				}
				for (t = 0; t < nt && strcmp(tl[t], lab); t++);
				if (t == nt && nt < 256) { strcpy(tl[nt], lab); ex[nt] = a; nt++; }
				if (t < 256) tn[t]++;
			}
		}
		for (t = 0; t < nt; t++)
			printf("%8u  -> %-40s (e.g. from %08X)\n", tn[t], tl[t], ex[t]);
		return 0;
	}
	if (argc >= 4 && !strcmp(argv[1], "-blocks")) {
		for (i = 2; i < argc && nrun < MAXRUN; i++) {
			led_load(&leds[nrun], argv[i]);
			load(&runs[nrun++], argv[i]);
		}
		blocks_mode();
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "-shift")) {
		for (i = 2; i < argc && nrun < MAXRUN; i++) load(&runs[nrun++], argv[i]);
		shift_mode();
		return 0;
	}
	if (argc < 3) { fprintf(stderr, "usage: slotdiff OUT.txt RUNDIR... | slotdiff -shift RUNDIR...\n"); return 1; }
	det = fopen(argv[1], "w");
	for (i = 2; i < argc && nrun < MAXRUN; i++) load(&runs[nrun++], argv[i]);

	printf("== layout: regions by base, in how many of %d runs, size range\n", nrun);
	{
		unsigned seen[MAXR * MAXRUN]; int ns = 0;
		for (k = 0; k < nrun; k++)
			for (i = 0; i < runs[k].nr; i++) {
				unsigned b = runs[k].r[i].base, lo = ~0u, hi = 0; int c = 0;
				for (j = 0; j < ns; j++) if (seen[j] == b) break;
				if (j < ns) continue;
				seen[ns++] = b;
				for (j = 0; j < nrun; j++) { const Reg *g = find(&runs[j], b); if (g) { c++; if (g->size < lo) lo = g->size; if (g->size > hi) hi = g->size; } }
				if (c != nrun || lo != hi)
					printf("  %08X %-8s %2d/%d  size %X..%X\n", b, runs[k].r[i].kind, c, nrun, lo, hi);
			}
	}

	printf("\n== content: regions in all runs, bytes common to all\n");
	printf("  %-8s %-8s %-22s %9s %9s  %s\n", "base", "kind", "in", "words", "differ", "moved / elsewhere / unsaved / small / data");
	for (i = 0; i < runs[0].nr; i++) {
		const Reg *g0 = &runs[0].r[i];
		const Reg *g[MAXRUN];
		unsigned n = g0->size, w, words;
		unsigned long long c[NC] = { 0 }, diff = 0;
		int shown = 0;
		for (k = 0; k < nrun; k++) {
			if (!(g[k] = find(&runs[k], g0->base))) break;
			if (g[k]->size < n) n = g[k]->size;
			if (g[k]->off + g[k]->size > runs[k].binsz) break;
		}
		if (k < nrun) continue;
		words = n / 4;
		for (w = 0; w < words; w++) {
			unsigned v[MAXRUN], same = 1, ptr = 1, small = 1;
			char l0[96], l[96];
			int cls, unsaved = 0, moved = 1;
			for (k = 0; k < nrun; k++) {
				v[k] = ((const unsigned *)(runs[k].bin + g[k]->off))[w];
				if (v[k] != v[0]) same = 0;
				if (v[k] < 0x10000 || v[k] >= 0x7FFF0000) ptr = 0;
				if (v[k] >= 0x10000) small = 0;
			}
			if (same) continue;
			diff++;
			if (ptr) {
				label(&runs[0], v[0], l0);
				if (!strcmp(l0, "unsaved")) unsaved = 1;
				for (k = 1; k < nrun; k++) {
					label(&runs[k], v[k], l);
					if (!strcmp(l, "unsaved")) unsaved = 1;
					if (strcmp(l, l0)) moved = 0;
				}
				cls = unsaved ? C_UNSAVED : moved ? C_MOVED : C_ELSEWHERE;
			} else
				cls = small ? C_SMALL : C_DATA;
			c[cls]++;
			if (shown < 400 && cls != C_MOVED) {
				char lab[96];
				shown++;
				fprintf(det, "%08X %-8s %-22s +%-6X %-13s", g0->base + w * 4, g0->kind, g0->in, w * 4, cname[cls]);
				for (k = 0; k < nrun && k < 4; k++) {
					if (ptr) { label(&runs[k], v[k], lab); fprintf(det, " %08X(%s)", v[k], lab); }
					else fprintf(det, " %08X", v[k]);
				}
				fprintf(det, "\n");
			}
		}
		tot_words += words; tot_diff += diff;
		for (k = 0; k < NC; k++) tot_c[k] += c[k];
		if (diff)
			printf("  %08X %-8s %-22s %9u %9llu  %llu / %llu / %llu / %llu / %llu\n", g0->base, g0->kind, g0->in, words, diff,
			       c[0], c[1], c[2], c[3], c[4]);
	}
	printf("\n== total: %llu words compared, %llu differ (%.3f%%)\n", tot_words, tot_diff, 100.0 * tot_diff / tot_words);
	for (k = 0; k < NC; k++) printf("  %-14s %llu\n", cname[k], tot_c[k]);
	fclose(det);
	return 0;
}
