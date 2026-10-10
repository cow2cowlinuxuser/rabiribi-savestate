/* ptrlearn SLOTDIR... - learn which words of the game's heap blocks are
 * pointers, from the blocks themselves, and judge the load's module shift by it.
 *
 * Every slot carries the arena's block ledger (address, size, allocation site)
 * at 10001000. Blocks from one site share a layout, so each (site, offset) is
 * tallied across every block and every slot: how often it held a pointer into a
 * module, a pointer into saved memory, zero, or anything else. A field that only
 * ever holds module pointers or zero, non-zero at least twice, is a pointer
 * field; one that has held anything else is data.
 *
 * Then every word the cross-launch load would shift as an address in
 * default.exe - any value inside that slot's default.exe range - is placed:
 * inside a ledger block at a learned pointer field, a learned data field, a
 * field seen too rarely to judge, or outside the ledger (by region kind).
 *
 * Game sites are default.exe offsets, so they match across launches and
 * machines; renderer sites are d3d9 addresses, which match within one build. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXR 1024
#define MAXM 256
#define MAXS 32
#define LEDGER 0x10001000u
#define GL_CAP 65536u
#define GL_ORD 16384u
#define GL_ROLES 64
#define LEARN_MAX (64u * 1024u)	/* bytes of each block that are learned from */

typedef struct { unsigned base, size; unsigned long long off; char kind[16], in[64]; } Reg;
typedef struct { unsigned base, size; char name[64]; } Mod;
typedef struct { unsigned addr, size, site, ord, role, arrival, role_op, frame, ms; } GlEnt;
typedef struct { unsigned u, p, size, site; } Blk;	/* p: where the payload starts */

typedef struct {
	char dir[MAX_PATH];
	Reg r[MAXR]; int nr;
	Mod m[MAXM]; int nm;
	const unsigned char *bin; unsigned long long binsz;
	Blk *b; int nb;
	unsigned exe_lo, exe_hi;
} Slot;

static Slot S[MAXS];
static int ns;

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

static const unsigned char *at(const Slot *s, unsigned a, unsigned n)
{
	int i;
	for (i = 0; i < s->nr; i++)
		if (a >= s->r[i].base && (unsigned long long)a + n <= (unsigned long long)s->r[i].base + s->r[i].size)
			return s->bin + s->r[i].off + (a - s->r[i].base);
	return NULL;
}

static int blk_cmp(const void *a, const void *b)
{
	unsigned x = ((const Blk *)a)->p, y = ((const Blk *)b)->p;
	return x < y ? -1 : x > y;
}

static int load(Slot *s, const char *dir)
{
	char path[MAX_PATH], line[4096], v[128];
	FILE *f;
	HANDLE h, mh;
	LARGE_INTEGER sz;
	const unsigned char *L;
	const GlEnt *ent;
	unsigned i, nroles, live;

	strcpy(s->dir, dir);
	sprintf(path, "%s\\d3d9sw_slot0.json", dir);
	if (!(f = fopen(path, "r"))) { fprintf(stderr, "skip %s: no json\n", dir); return 0; }
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "\"k\":\"region\"") && s->nr < MAXR) {
			Reg *g = &s->r[s->nr++];
			field(line, "base", v, sizeof(v)); g->base = strtoul(v, 0, 16);
			field(line, "size", v, sizeof(v)); g->size = strtoul(v, 0, 16);
			field(line, "off", v, sizeof(v)); g->off = _strtoui64(v, 0, 16);
			field(line, "kind", g->kind, sizeof(g->kind));
			field(line, "in", g->in, sizeof(g->in));
		} else if (strstr(line, "\"k\":\"module\"") && s->nm < MAXM) {
			Mod *m = &s->m[s->nm++];
			field(line, "base", v, sizeof(v)); m->base = strtoul(v, 0, 16);
			field(line, "size", v, sizeof(v)); m->size = strtoul(v, 0, 16);
			field(line, "name", m->name, sizeof(m->name));
			if (!_stricmp(m->name, "default.exe")) { s->exe_lo = m->base; s->exe_hi = m->base + m->size; }
		}
	}
	fclose(f);
	sprintf(path, "%s\\d3d9sw_slot0.bin", dir);
	h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "skip %s: no bin\n", dir); return 0; }
	GetFileSizeEx(h, &sz);
	mh = CreateFileMappingA(h, 0, PAGE_READONLY, 0, 0, 0);
	s->bin = mh ? MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0) : NULL;
	s->binsz = sz.QuadPart;
	if (!s->bin) { fprintf(stderr, "skip %s: map failed\n", dir); return 0; }
	if (!s->exe_lo) { fprintf(stderr, "skip %s: no default.exe in the module list\n", dir); return 0; }

	/* GhLedger: lock, arrival, live, full, ord_full, nroles, roles[64], ords[], ent[] */
	{
		size_t ent_off = 6 * 4 + GL_ROLES * (32 + 5 * 4) + GL_ORD * 5 * 4;
		L = at(s, LEDGER, (unsigned)(ent_off + GL_CAP * sizeof(GlEnt)));
		if (!L) { fprintf(stderr, "skip %s: no ledger at %08X\n", dir, LEDGER); return 0; }
		live = ((const unsigned *)L)[2];
		nroles = ((const unsigned *)L)[5];
		if (nroles > GL_ROLES || live > GL_CAP) { fprintf(stderr, "skip %s: ledger header implausible\n", dir); return 0; }
		ent = (const GlEnt *)(L + ent_off);
	}
	s->b = malloc(GL_CAP * sizeof(Blk));
	for (i = 0; i < GL_CAP; i++) {
		const GlEnt *e = &ent[i];
		Blk *b;
		unsigned p = e->addr;
		if (e->addr <= 1 || !e->size) continue;
		/* Renderer and CRT-tap blocks hand out an aligned address past the
		 * one the ledger records, with the recorded one stored just below. */
		if (e->site >= 0x60000000u && e->site < 0x61000000u) {
			unsigned a, q, found = 0;
			for (a = 64; a >= 16 && !found; a /= 4)
				for (q = (e->addr + 4 + a - 1) & ~(a - 1); q < e->addr + 4 + a && !found; q += a) {
					const unsigned *w = (const unsigned *)at(s, q - 4, 4);
					if (w && *w == e->addr) { p = q; found = 1; }
				}
			if (found) {
				const unsigned *h = (const unsigned *)at(s, p, 12);
				if (h && h[0] == 0x4E4C5053u && h[2] == ~(0x4E4C5053u ^ h[1])) p += 64;
			}
		}
		if (p - e->addr >= e->size) continue;
		b = &s->b[s->nb++];
		b->u = e->addr; b->p = p; b->size = e->size - (p - e->addr); b->site = e->site;
	}
	qsort(s->b, s->nb, sizeof(Blk), blk_cmp);
	printf("  %-48s default.exe at %08X, %d ledger block(s), %d region(s)\n", dir, s->exe_lo, s->nb, s->nr);
	return 1;
}

/* 64 KB granules touched by a module / a saved region, so the per-word checks
 * below are a table hit for nearly every value. */
static unsigned char *gran(const Slot *s, int mods)
{
	static unsigned char *g[MAXS][2];
	int k = (int)(s - S), i, n = mods ? s->nm : s->nr;
	if (g[k][mods]) return g[k][mods];
	g[k][mods] = calloc(65536, 1);
	for (i = 0; i < n; i++) {
		unsigned lo = mods ? s->m[i].base : s->r[i].base, sz = mods ? s->m[i].size : s->r[i].size;
		unsigned long long a;
		for (a = lo >> 16; a <= ((unsigned long long)lo + sz - 1) >> 16 && a < 65536; a++) g[k][mods][a] = 1;
	}
	return g[k][mods];
}

static const Mod *mod_of(const Slot *s, unsigned v)
{
	int i;
	if (!gran(s, 1)[v >> 16]) return NULL;
	for (i = 0; i < s->nm; i++) if (v >= s->m[i].base && v - s->m[i].base < s->m[i].size) return &s->m[i];
	return NULL;
}

static int in_mod(const Slot *s, unsigned v) { return mod_of(s, v) != NULL; }

static int in_saved(const Slot *s, unsigned v)
{
	int i;
	if (v < 0x10000 || !gran(s, 0)[v >> 16]) return 0;
	for (i = 0; i < s->nr; i++) if (v >= s->r[i].base && v - s->r[i].base < s->r[i].size) return 1;
	return 0;
}

/* (site, offset) -> tallies, open addressing. Ledger blocks use their
 * allocation site; module images use K_IMAGE plus a hash of the module name,
 * with the offset from the module base; anything else uses K_OTHER with the
 * address itself, which only lines up across launches for fixed placements. */
#define K_OTHER 0xE0000000u
#define K_IMAGE 0xF0000000u
/* fv/fb: the first default.exe-range value seen and the base it was seen at.
 * Later sightings at another base count as same_raw if the value did not move
 * with the module - data - or same_rel if it moved by exactly the base change. */
typedef struct { unsigned site, off, n, mod, blk, zero, other, used, fv, fb, same_raw, same_rel; } Cell;
#define HBITS 23
static Cell *H;
static unsigned char *Hhome;	/* bit per home index that a sparse cell hashes to */
static unsigned long long hfull;

static unsigned long long home(unsigned site, unsigned off)
{
	return ((site * 2654435761u) ^ (off * 0x9E3779B9u)) & ((1ull << HBITS) - 1);
}

static Cell *cell(unsigned site, unsigned off, int make)
{
	unsigned long long m = (1ull << HBITS) - 1, i = home(site, off);
	unsigned k;
	for (k = 0; k < 64; k++, i = (i + 1) & m) {
		Cell *c = &H[i];
		if (c->used && c->site == site && c->off == off) return c;
		if (!c->used) {
			if (!make) return NULL;
			c->site = site; c->off = off; c->used = 1;
			return c;
		}
	}
	if (make) hfull++;
	return NULL;
}

static unsigned name_key(const char *n)
{
	unsigned h = 2166136261u;
	for (; *n; n++) h = (h ^ (unsigned char)(*n | 0x20)) * 16777619u;
	return K_IMAGE | (h & 0x0FFFFFFFu);
}

enum { P_LEDGER, P_DEEP, P_IMAGE, P_OTHER, NP };

/* Calls fn for every saved word with where it sits and its learning key. */
typedef void (*WordFn)(const Slot *s, int place, unsigned site, unsigned off, unsigned a, unsigned v, void *ctx);
static void walk(const Slot *s, WordFn fn, void *ctx)
{
	int i;
	for (i = 0; i < s->nr; i++) {
		const Reg *g = &s->r[i];
		const unsigned *p = (const unsigned *)(s->bin + g->off);
		const Mod *m = !strcmp(g->kind, "image") ? mod_of(s, g->base) : NULL;
		unsigned mk = m ? name_key(m->name) : 0, w;
		int lo = 0, hi = s->nb;
		if (g->off + g->size > s->binsz) continue;
		while (lo < hi) {
			int mid = (lo + hi) / 2;
			if ((unsigned long long)s->b[mid].p + s->b[mid].size <= g->base) lo = mid + 1; else hi = mid;
		}
		for (w = 0; w < g->size / 4; w++) {
			unsigned a = g->base + w * 4;
			if (m) { fn(s, P_IMAGE, mk, a - m->base, a, p[w], ctx); continue; }
			while (lo < s->nb && (unsigned long long)s->b[lo].p + s->b[lo].size <= a) lo++;
			if (lo < s->nb && a >= s->b[lo].p) {
				unsigned off = a - s->b[lo].p;
				fn(s, off < LEARN_MAX ? P_LEDGER : P_DEEP, s->b[lo].site, off, a, p[w], ctx);
			} else fn(s, P_OTHER, K_OTHER, a, a, p[w], ctx);
		}
	}
}

static void tally(Cell *c, const Slot *s, unsigned v)
{
	c->n++;
	if (!v) c->zero++;
	else if (in_mod(s, v)) c->mod++;
	else if (in_saved(s, v)) c->blk++;
	else c->other++;
	if (v && c->fb && c->fb != s->exe_lo) {
		if (v == c->fv) c->same_raw++;
		else if (v - s->exe_lo == c->fv - c->fb) c->same_rel++;
	}
	if (!c->fb && v >= s->exe_lo && v < s->exe_hi) { c->fv = v; c->fb = s->exe_lo; }
}

static unsigned long long learned;

/* Pass 1: the first LEARN_MAX bytes of every ledger block are tallied whole;
 * everywhere else a cell is only opened where some slot holds a module address. */
static void learn1(const Slot *s, int place, unsigned site, unsigned off, unsigned a, unsigned v, void *ctx)
{
	Cell *c;
	if (place == P_LEDGER) { if ((c = cell(site, off, 1)) != NULL) { tally(c, s, v); learned++; } return; }
	if (v && in_mod(s, v) && cell(site, off, 1)) {
		unsigned long long h = home(site, off);
		Hhome[h >> 3] |= 1 << (h & 7);
	}
}

/* Pass 2: what every slot holds at those opened cells. */
static void learn2(const Slot *s, int place, unsigned site, unsigned off, unsigned a, unsigned v, void *ctx)
{
	unsigned long long h;
	Cell *c;
	if (place == P_LEDGER) return;
	h = home(site, off);
	if (!(Hhome[h >> 3] & (1 << (h & 7)))) return;
	if ((c = cell(site, off, 0)) != NULL) { tally(c, s, v); learned++; }
}

enum { V_PTR, V_MIXED, V_DATA, V_RARE, NV };

/* MIXED: only ever pointers, into modules and into saved memory both - a
 * generic pointer field, not data. A value that stayed put while default.exe
 * moved is data, whatever else the field held. */
static int verdict(const Cell *c)
{
	if (!c || !c->n) return V_RARE;
	if (c->same_raw || c->other) return V_DATA;
	/* A default.exe shift needs proof the field followed default.exe. */
	if (!c->same_rel) return V_RARE;
	if (c->blk) return c->mod ? V_MIXED : V_DATA;
	return c->mod >= 2 ? V_PTR : V_RARE;
}

static const Blk *blk_of(const Slot *s, unsigned a)
{
	int lo = 0, hi = s->nb - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const Blk *b = &s->b[mid];
		if (a < b->p) hi = mid - 1;
		else if (a - b->p >= b->size) lo = mid + 1;
		else return b;
	}
	return NULL;
}

static unsigned dsite[64], dblk[64], ndeep;
static unsigned long long dsize[64], dn[64];

/* -grade A B LEARN...: A and B are saves of the same moment from launches where
 * default.exe sat at different bases. Every A word holding a default.exe
 * address is a pointer if B holds it moved by the base difference, data if B
 * holds the same value; anything else is left out. The map is learned from
 * LEARN only. Scored per place: what blind shifting does, and what shifting
 * only learned pointer fields does. */
static const char *pname[NP] = { "game heap blocks", "deep in big blocks", "module images", "other memory" };

/* Site rule for deep words no field verdict covers: a site whose judged deep
 * fields are pointers at least nine times to one. */
#define MAXSITE 4096
static struct { unsigned site; unsigned long long ptr, data; } srule[MAXSITE];
static int nsrule;

static void site_rules(void)
{
	unsigned long long j;
	for (j = 0; j < (1ull << HBITS); j++) {
		const Cell *c = &H[j];
		int v, i;
		if (!c->used || c->site >= K_OTHER || c->off < LEARN_MAX) continue;
		v = verdict(c);
		if (v == V_RARE) continue;
		for (i = 0; i < nsrule && srule[i].site != c->site; i++);
		if (i == nsrule) { if (nsrule == MAXSITE) continue; srule[nsrule].site = c->site; nsrule++; }
		if (v == V_DATA) srule[i].data++; else srule[i].ptr++;
	}
}

static int site_says_ptr(unsigned site)
{
	int i;
	for (i = 0; i < nsrule; i++)
		if (srule[i].site == site) return srule[i].ptr >= 4 && srule[i].ptr >= 9 * srule[i].data;
	return 0;
}

/* default.exe's base relocations, as offsets from its base, sorted. */
static unsigned *rel, nrel;
static int u_cmp(const void *a, const void *b)
{
	unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
	return x < y ? -1 : x > y;
}

static void read_relocs(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;
	long n;
	unsigned pe, nsec, opt, rva, size, i, foff = 0;
	if (!f) { printf("  (no %s, relocation check skipped)\n", path); return; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	d = malloc(n);
	if (fread(d, 1, n, f) != (size_t)n) { fclose(f); return; }
	fclose(f);
	pe = *(unsigned *)(d + 0x3C);
	nsec = *(unsigned short *)(d + pe + 6);
	opt = pe + 24;
	rva = *(unsigned *)(d + opt + 96 + 5 * 8);
	size = *(unsigned *)(d + opt + 96 + 5 * 8 + 4);
	for (i = 0; i < nsec; i++) {
		const unsigned char *sh = d + opt + *(unsigned short *)(d + pe + 20) + i * 40;
		unsigned va = *(unsigned *)(sh + 12), vs = *(unsigned *)(sh + 8), raw = *(unsigned *)(sh + 20);
		if (rva >= va && rva < va + vs) foff = raw + (rva - va);
	}
	if (!foff) return;
	rel = malloc(size * 2 * sizeof(unsigned));
	for (i = 0; i + 8 <= size;) {
		unsigned page = *(unsigned *)(d + foff + i), bs = *(unsigned *)(d + foff + i + 4), k;
		if (bs < 8) break;
		for (k = 8; k + 2 <= bs; k += 2) {
			unsigned short e = *(unsigned short *)(d + foff + i + k);
			if ((e >> 12) == 3) rel[nrel++] = page + (e & 0xFFF);
		}
		i += bs;
	}
	qsort(rel, nrel, sizeof(unsigned), u_cmp);
	free(d);
}

static int in_relocs(unsigned off) { return nrel && bsearch(&off, rel, nrel, sizeof(unsigned), u_cmp) != NULL; }

typedef struct {
	const Slot *B;
	int delta;
	unsigned exe_key;
	unsigned long long ptr[NP], dat[NP], amb[NP], lfix[NP], ldmg[NP], sfix[NP], sdmg[NP];
	unsigned long long rel_ptr, rel_dat;	/* default.exe image words listed in its relocations */
	unsigned long long optr[NP], ofix[NP], ashift[NP];	/* pointers to another target; still unclear */
	unsigned long long bkind[4][2];	/* still unclear, by what B holds there: left alone, shifted */
	struct { char key[64]; unsigned long long n[5]; } who[512];
	int nwho, cur, ndmg;
	char dmg[40][200];
} Grade;

static void grade1(const Slot *A, int place, unsigned site, unsigned off, unsigned a, unsigned v, void *ctx)
{
	Grade *g = ctx;
	const Slot *B = g->B;
	const unsigned *pb;
	unsigned ab = a;
	int truth, vd, by_site = 0;
	if (v < A->exe_lo || v >= A->exe_hi) return;
	if (place == P_IMAGE) {
		int i;
		for (i = 0; i < B->nm && name_key(B->m[i].name) != site; i++);
		if (i == B->nm) return;
		ab = B->m[i].base + off;
	}
	if (!(pb = (const unsigned *)at(B, ab, 4))) return;
	if (*pb == v + (unsigned)g->delta) truth = 1;
	else if (*pb == v) truth = 0;
	else truth = -1;
	{
		/* per module for images, per site elsewhere: pointers, data, unclear, learned fixed, damaged */
		const Mod *m = place == P_IMAGE ? mod_of(A, a) : NULL;
		const char *who = m ? m->name : NULL;
		char key[64];
		int j;
		if (who) sprintf(key, "image %.50s", who); else sprintf(key, "%s %08X", place == P_OTHER ? "other" : "site", site);
		for (j = 0; j < g->nwho && strcmp(g->who[j].key, key); j++);
		if (j == g->nwho && g->nwho < 512) { strcpy(g->who[j].key, key); g->nwho++; }
		g->cur = j < 512 ? j : -1;
	}
	if (truth < 0) {
		/* Not the same value and not moved with the module, but inside
		 * default.exe on both sides: a pointer to another target there. */
		if (*pb >= B->exe_lo && *pb < B->exe_hi && (A->exe_hi <= B->exe_lo || B->exe_hi <= A->exe_lo)) {
			int vd2 = verdict(cell(site, off, 0));
			g->optr[place]++;
			if (vd2 == V_PTR || vd2 == V_MIXED) g->ofix[place]++;
		} else {
			const Cell *c2 = cell(site, off, 0);
			int vd2 = verdict(c2);
			int kind = !*pb ? 0 : in_saved(B, *pb) ? 1 : in_mod(B, *pb) ? 2 : 3;
			g->amb[place]++;
			g->bkind[kind][vd2 == V_PTR || vd2 == V_MIXED]++;
			if (vd2 == V_PTR || vd2 == V_MIXED) g->ashift[place]++;
		}
		if (g->cur >= 0) g->who[g->cur].n[2]++;
		return;
	}
	if (truth) g->ptr[place]++; else g->dat[place]++;
	if (g->cur >= 0) g->who[g->cur].n[truth ? 0 : 1]++;
	if (site == g->exe_key && in_relocs(off)) { if (truth) g->rel_ptr++; else g->rel_dat++; }
	vd = verdict(cell(site, off, 0));
	if (vd == V_RARE && place == P_DEEP && site_says_ptr(site)) by_site = 1;
	if (vd == V_PTR || vd == V_MIXED || by_site) {
		if (truth) g->lfix[place]++; else g->ldmg[place]++;
		if (by_site) { if (truth) g->sfix[place]++; else g->sdmg[place]++; }
		if (g->cur >= 0) g->who[g->cur].n[truth ? 3 : 4]++;
		if (!truth && g->ndmg < 40) {
			const Cell *c = cell(site, off, 0);
			sprintf(g->dmg[g->ndmg++], "%s+%X at %08X holds %08X in both; learned n=%u mod=%u blk=%u zero=%u other=%u",
				g->who[g->cur].key, off, a, v, c ? c->n : 0, c ? c->mod : 0, c ? c->blk : 0, c ? c->zero : 0, c ? c->other : 0);
		}
	}
	if (place == P_DEEP) {
		int j;
		for (j = 0; j < (int)ndeep && dsite[j] != site; j++);
		if (j == (int)ndeep && ndeep < 64) { dsite[j] = site; dn[j] = 0; dsize[j] = 0; ndeep++; }
		if (j < 64) { if (truth) dn[j]++; else dsize[j]++; }
	}
}

static void grade(void)
{
	const Slot *A = &S[0];
	static Grade g;
	int i;
	const char *exe = getenv("PTRLEARN_EXE");

	g.B = &S[1];
	g.delta = (int)(g.B->exe_lo - A->exe_lo);
	g.exe_key = name_key("default.exe");
	site_rules();
	read_relocs(exe ? exe : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DoDonPachi Resurrection\\default.exe");
	printf("\ngrading %s against %s: default.exe %08X vs %08X\n", A->dir, g.B->dir, A->exe_lo, g.B->exe_lo);
	walk(A, grade1, &g);
	printf("  %-20s %9s %9s %9s | blind shift: %9s %9s | learned map: %9s %9s %9s | by site rule: %7s %7s\n", "where",
	       "pointers", "data", "unclear", "fixed", "damaged", "fixed", "damaged", "missed", "fixed", "damaged");
	for (i = 0; i < NP; i++)
		printf("  %-20s %9llu %9llu %9llu | %22llu %9llu | %22llu %9llu %9llu | %21llu %7llu\n", pname[i], g.ptr[i], g.dat[i],
		       g.amb[i], g.ptr[i], g.dat[i], g.lfix[i], g.ldmg[i], g.ptr[i] - g.lfix[i], g.sfix[i], g.sdmg[i]);
	printf("\n  the unclear words: pointers to another default.exe target on each side | learned map shifts | the rest, still unclear | learned map shifts\n");
	for (i = 0; i < NP; i++)
		printf("  %-20s %9llu | %9llu | %9llu | %9llu\n", pname[i], g.optr[i], g.ofix[i], g.amb[i], g.ashift[i]);
	{
		static const char *kn[4] = { "zero", "a pointer into saved memory", "a pointer into another module", "anything else" };
		printf("\n  still unclear, by what the other save holds there: left alone / shifted by the learned map\n");
		for (i = 0; i < 4; i++) printf("    %-32s %9llu / %llu\n", kn[i], g.bkind[i][0], g.bkind[i][1]);
	}
	printf("\n  default.exe relocation table: %u entries; image words it lists: %llu pointer(s), %llu data\n", nrel, g.rel_ptr, g.rel_dat);
	printf("\n  by module or site (largest first): pointers, data, unclear | learned map fixed, damaged, missed\n");
	for (;;) {
		int best = -1, j;
		for (j = 0; j < g.nwho; j++) {
			unsigned long long t = g.who[j].n[0] + g.who[j].n[1] + g.who[j].n[2];
			if (t >= 50 && (best < 0 || t > g.who[best].n[0] + g.who[best].n[1] + g.who[best].n[2])) best = j;
		}
		if (best < 0) break;
		printf("    %-34s %8llu %8llu %8llu | %8llu %6llu %6llu\n", g.who[best].key, g.who[best].n[0], g.who[best].n[1],
		       g.who[best].n[2], g.who[best].n[3], g.who[best].n[4], g.who[best].n[0] - g.who[best].n[3]);
		g.who[best].n[0] = g.who[best].n[1] = g.who[best].n[2] = 0;
	}
	if (g.ndmg) printf("\n  words the learned map would damage:\n");
	for (i = 0; i < g.ndmg; i++) printf("    %s\n", g.dmg[i]);
	printf("\n  deep, by site: pointers / data in A, and the site rule learned from the other slots\n");
	for (i = 0; i < (int)ndeep; i++) {
		int j;
		for (j = 0; j < nsrule && srule[j].site != dsite[i]; j++);
		printf("    site %08X %8llu / %-8llu  rule: %llu pointer field(s), %llu data field(s) -> %s\n", dsite[i], dn[i], dsize[i],
		       j < nsrule ? srule[j].ptr : 0, j < nsrule ? srule[j].data : 0, site_says_ptr(dsite[i]) ? "shift" : "leave");
	}
}

/* -same A B: two saves from one boot (default.exe at the same base). Counts
 * the words of the game's own blocks - ledger blocks from default.exe sites -
 * that differ, block by block at the same address, so a replay can be held
 * against its recording and against a run with no input at all. */
typedef struct {
	const Slot *B;
	unsigned long long words, diff, missing;
	unsigned site[256];
	unsigned long long sdiff[256], swords[256];
	int nsite;
} Same;

static void same1(const Slot *A, int place, unsigned site, unsigned off, unsigned a, unsigned v, void *ctx)
{
	Same *s = ctx;
	const unsigned *pb;
	int i;
	if ((place != P_LEDGER && place != P_DEEP) || site >= 0x01000000u) return;
	s->words++;
	for (i = 0; i < s->nsite && s->site[i] != site; i++);
	if (i == s->nsite && s->nsite < 256) { s->site[i] = site; s->sdiff[i] = s->swords[i] = 0; s->nsite++; }
	if (i < 256) s->swords[i]++;
	if (!(pb = (const unsigned *)at(s->B, a, 4))) { s->missing++; return; }
	if (*pb != v) { s->diff++; if (i < 256) s->sdiff[i]++; }
}

static int same(const char *a, const char *b)
{
	static Same s;
	int i, j;
	if (!load(&S[0], a) || !load(&S[1], b)) return 1;
	if (S[0].exe_lo != S[1].exe_lo) printf("  (default.exe moved between these runs, so pointers differ too)\n");
	s.B = &S[1];
	walk(&S[0], same1, &s);
	printf("\ngame blocks: %llu word(s), %llu differ (%.4f%%), %llu not in the second save\n", s.words, s.diff,
	       s.words ? 100.0 * s.diff / s.words : 0.0, s.missing);
	for (j = 0; j < 40; j++) {
		int best = -1;
		for (i = 0; i < s.nsite; i++) if (s.sdiff[i] && (best < 0 || s.sdiff[i] > s.sdiff[best])) best = i;
		if (best < 0) break;
		printf("  site %08X %10llu of %10llu word(s) differ\n", s.site[best], s.sdiff[best], s.swords[best]);
		s.sdiff[best] = 0;
	}
	return 0;
}

/* -site A B SITE: the blocks of one allocation site in two saves, word by word,
 * default.exe pointers compared by their offset into each save's own copy.
 * Prints where in the blocks the differences sit (per 64 KB) and the first
 * differing words, so a whole-block shift, a few changed objects or a pool
 * filled in another order can be told apart. */
static int site_cmp(const char *a, const char *b, unsigned site)
{
	const Slot *A = &S[0], *B = &S[1];
	int i, shown = 0;
	if (!load(&S[0], a) || !load(&S[1], b)) return 1;
	for (i = 0; i < A->nb; i++) {
		const Blk *x = &A->b[i];
		const unsigned *pa, *pb;
		unsigned w, n, diff = 0, chunk_d = 0, nz_a = 0, nz_b = 0;
		char hist[1024];
		int hl = 0;
		if (x->site != site) continue;
		n = x->size / 4;
		pa = (const unsigned *)at(A, x->p, x->size & ~3u);
		pb = (const unsigned *)at(B, x->p, x->size & ~3u);
		if (!pa || !pb) { printf("block %08X (%u KB): not in both saves\n", x->p, x->size >> 10); continue; }
		hist[0] = 0;
		for (w = 0; w < n; w++) {
			unsigned va = pa[w], vb = pb[w];
			int ia = va >= A->exe_lo && va < A->exe_hi, ib = vb >= B->exe_lo && vb < B->exe_hi;
			int same = ia && ib ? va - A->exe_lo == vb - B->exe_lo : va == vb;
			if (va) nz_a++;
			if (vb) nz_b++;
			if (!same) {
				diff++; chunk_d++;
				if (shown < 24) {
					printf("    +%06X  %08X%s  vs  %08X%s\n", w * 4, va, ia ? " (exe)" : "", vb, ib ? " (exe)" : "");
					shown++;
				}
			}
			if ((w + 1) % 16384 == 0 || w + 1 == n) {
				if (hl < 1000) hl += sprintf(hist + hl, "%c", chunk_d == 0 ? '.' : chunk_d < 64 ? 'o' : 'X');
				chunk_d = 0;
			}
		}
		printf("block %08X (%u KB): %u of %u words differ; nonzero words %u vs %u\n  per 64 KB (. none, o a few, X many): %s\n",
		       x->p, x->size >> 10, diff, n, nz_a, nz_b, hist);
	}
	return 0;
}

/* -layout A B: the two saves' ledger blocks side by side in address order, from
 * the first one that differs in address, size or site. */
static int layout(const char *a, const char *b)
{
	const Slot *A = &S[0], *B = &S[1];
	int i = 0, j = 0, shown = 0;
	if (!load(&S[0], a) || !load(&S[1], b)) return 1;
	unsigned min = getenv("PTRLEARN_MIN") ? strtoul(getenv("PTRLEARN_MIN"), 0, 0) : 0;
	for (;;) {
		while (i < A->nb && A->b[i].size < min) i++;
		while (j < B->nb && B->b[j].size < min) j++;
		if (i >= A->nb || j >= B->nb || A->b[i].u != B->b[j].u || A->b[i].size != B->b[j].size) break;
		i++; j++;
	}
	printf("blocks of %u bytes or more match in address and size up to here:\n", min);
	printf("  %-36s | %s\n", "first save: address size site", "second save: address size site");
	for (; shown < 40 && (i < A->nb || j < B->nb); shown++, i++, j++) {
		while (i < A->nb && A->b[i].size < min) i++;
		while (j < B->nb && B->b[j].size < min) j++;
		printf("  %08X %10u %08X     | %08X %10u %08X\n", i < A->nb ? A->b[i].u : 0, i < A->nb ? A->b[i].size : 0,
		       i < A->nb ? A->b[i].site : 0, j < B->nb ? B->b[j].u : 0, j < B->nb ? B->b[j].size : 0,
		       j < B->nb ? B->b[j].site : 0);
	}
	return 0;
}

/* -chunks A BASE SIZE: the big-block chunk headers in a pinned span, as the save
 * holds them (gameheap.c GhBigArena: lock, cap, top, committed, free, live,
 * peak), with each free-list entry (GhBigBlk: magic, size, next, owner). */
static int chunks(const char *a, unsigned base, unsigned size)
{
	const Slot *A = &S[0];
	unsigned off = 0;

	if (!load(&S[0], a)) return 1;
	while (off + 4096 <= size) {
		const unsigned *h = (const unsigned *)at(A, base + off, 28);
		unsigned cap, top, com, fr, guard = 0;
		if (!h) { printf("  %08X: header not in the save\n", base + off); break; }
		cap = h[1]; top = h[2]; com = h[3]; fr = h[4];
		printf("  chunk %08X cap %08X top %08X committed %08X (ends %08X, committed to %08X) live %u KB%s\n",
		       base + off, cap, top, com, base + off + cap, base + off + com, h[5] >> 10,
		       top > com ? "   TOP PAST COMMIT" : "");
		while (fr && guard++ < 64) {
			const unsigned *b = (const unsigned *)at(A, fr, 16);
			if (!b) { printf("      free %08X: not in the save\n", fr); break; }
			printf("      free %08X size %08X (to %08X)%s\n", fr, b[1], fr + b[1],
			       fr + b[1] > base + off + com ? "   PAST COMMIT" : "");
			fr = b[2];
		}
		if (!cap || (cap & 0xFFFF) || cap > size - off) break;
		off += cap;
	}
	return 0;
}

/* -find A ADDR: the ledger blocks within 1 MB of an address. */
static int find_addr(const char *a, unsigned addr)
{
	const Slot *A = &S[0];
	int i;

	if (!load(&S[0], a)) return 1;
	for (i = 0; i < A->nb; i++) {
		const Blk *x = &A->b[i];
		if (x->u + x->size + 0x100000u > addr && x->u < addr + 0x100000u)
			printf("  %08X..%08X %10u site %08X%s\n", x->u, x->u + x->size, x->size, x->site,
			       addr >= x->u && addr < x->u + x->size ? "   <- holds it" : "");
	}
	return 0;
}

int main(int argc, char **argv)
{
	int i, k, first = 1, k0 = 0;

	if (argc < 2) { fprintf(stderr, "usage: ptrlearn SLOTDIR... | ptrlearn -grade A B LEARNDIR...\n"); return 1; }
	if (!strcmp(argv[1], "-same") && argc == 4) return same(argv[2], argv[3]);
	if (!strcmp(argv[1], "-chunks") && argc == 5)
		return chunks(argv[2], strtoul(argv[3], 0, 16), strtoul(argv[4], 0, 16));
	if (!strcmp(argv[1], "-find") && argc == 4) return find_addr(argv[2], strtoul(argv[3], 0, 16));
	if (!strcmp(argv[1], "-layout") && argc == 4) return layout(argv[2], argv[3]);
	if (!strcmp(argv[1], "-site") && argc == 5) return site_cmp(argv[2], argv[3], strtoul(argv[4], 0, 16));
	H = calloc(1ull << HBITS, sizeof(Cell));
	if (!strcmp(argv[1], "-grade")) { first = 2; k0 = 2; }
	printf("slots:\n");
	for (i = first; i < argc && ns < MAXS; i++) {
		if (load(&S[ns], argv[i])) ns++;
		else if (k0 && ns < 2) { fprintf(stderr, "grading needs both A and B\n"); return 1; }
	}
	if (!ns) return 1;

	Hhome = calloc(1ull << (HBITS - 3), 1);
	for (k = k0; k < ns; k++) walk(&S[k], learn1, NULL);
	for (k = k0; k < ns; k++) walk(&S[k], learn2, NULL);
	{
		unsigned long long cnt[NV] = { 0 }, j;
		for (j = 0; j < (1ull << HBITS); j++) if (H[j].used) cnt[verdict(&H[j])]++;
		printf("\nlearned from %llu word(s): %llu module-pointer field(s), %llu mixed pointer field(s), %llu data field(s), %llu too rare to judge%s\n",
		       learned, cnt[V_PTR], cnt[V_MIXED], cnt[V_DATA], cnt[V_RARE], hfull ? " (table full, some dropped)" : "");
	}
	if (k0) { grade(); return 0; }

	/* What the load would shift as default.exe addresses, slot by slot. Deep
	 * words (past what was learned from) are split by whose block they are in. */
	printf("\nwords the load would shift as default.exe addresses, by where they sit:\n");
	printf("  %-24s %8s | ledger: %8s %8s %8s %8s | deep: %8s %8s | outside\n", "slot", "total", "mod ptr", "mixed", "data", "rare", "game", "renderer");
	for (k = 0; k < ns; k++) {
		const Slot *s = &S[k];
		unsigned long long tot = 0, v[NV] = { 0 }, deep[2] = { 0 }, outside = 0;
		static struct { char key[80]; unsigned long long n; } out[64];
		int nout = 0, j;
		for (i = 0; i < s->nr; i++) {
			const Reg *g = &s->r[i];
			const unsigned *p = (const unsigned *)(s->bin + g->off);
			unsigned w;
			if (g->off + g->size > s->binsz) continue;
			for (w = 0; w < g->size / 4; w++) {
				unsigned a = g->base + w * 4;
				const Blk *b;
				if (p[w] < s->exe_lo || p[w] >= s->exe_hi) continue;
				tot++;
				if ((b = blk_of(s, a)) != NULL) {
					unsigned off = a - b->p;
					if (off >= LEARN_MAX) {
						deep[b->site >= 0x60000000u && b->site < 0x61000000u]++;
						if (k == 0) {
							for (j = 0; j < ndeep && (dsite[j] != b->site); j++);
							if (j == ndeep && ndeep < 64) { dsite[j] = b->site; dsize[j] = 0; dn[j] = 0; dblk[j] = 0; ndeep++; }
							if (j < 64) { dn[j]++; if (dblk[j] != b->p) { dblk[j] = b->p; dsize[j] += b->size; } }
						}
					}
					else v[verdict(cell(b->site, off, 0))]++;
				} else {
					char key[80];
					outside++;
					sprintf(key, "%s %.40s", g->kind, g->in);
					for (j = 0; j < nout && strcmp(out[j].key, key); j++);
					if (j == nout && nout < 64) { strcpy(out[nout].key, key); out[nout].n = 0; nout++; }
					if (j < 64) out[j].n++;
				}
			}
		}
		{
			const char *leaf = strrchr(s->dir, '\\');
			printf("  %-24.24s %8llu | %16llu %8llu %8llu %8llu | %14llu %8llu | %llu\n", leaf ? leaf + 1 : s->dir, tot,
			       v[V_PTR], v[V_MIXED], v[V_DATA], v[V_RARE], deep[0], deep[1], outside);
		}
		if (getenv("PTRLEARN_OUTSIDE"))
			for (j = 0; j < nout; j++)
				if (out[j].n * 50 >= outside)
					printf("      outside: %9llu  %s\n", out[j].n, out[j].key);
	}
	printf("\ndeep hits in the first slot by site (site, hits, bytes of the blocks they sit in):\n");
	for (i = 0; i < (int)ndeep; i++)
		if (dn[i] >= 100)
			printf("  site %08X %8llu hit(s) in %llu KB\n", dsite[i], dn[i], dsize[i] >> 10);
	return 0;
}
