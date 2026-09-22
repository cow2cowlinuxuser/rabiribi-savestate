/* Giving the game a heap of its own.
 *
 * Every ownership heuristic in savestate.c exists for one reason: Rabi-Ribi
 * links ucrtbase, and ucrtbase does not create a heap. It calls GetProcessHeap
 * and allocates out of the same arena ntdll, GDI and combase are using. So the
 * game's state and Windows' state are interleaved block by block in one heap,
 * and every restore has to guess which is which. BLKOWNER, HELDVETO, VTABVETO,
 * the reachability closure and the recycled-block hazard are all consequences
 * of that single fact.
 *
 * DoDonPachi Resurrection is the control group, and it says what the other side
 * looks like. It links MSVCR100, which calls HeapCreate, so the game's runtime
 * heap has no other tenant. Its restore reported 208 regions, none by block, and
 * captured 1076 MB of 1089 - 98.8% of the process, with no ownership vote taken
 * anywhere. Rabi-Ribi's captures 59% and negotiates for the rest.
 *
 * We cannot change which runtime the game links. We can change where its
 * allocations land: create a heap, and redirect the executable's imports of
 * malloc and friends into it. What the game allocates from then on has no other
 * tenant, exactly as if it had linked MSVCR100.
 *
 * Three things make this safe rather than merely appealing.
 *
 * The redirect goes in after the process has started, so everything allocated
 * before it still lives on the process heap and will be freed through a pointer
 * we did not issue. free therefore dispatches: ours goes to our heap, everything
 * else goes to the runtime's own free, unchanged. The mixed period is correct by
 * construction rather than by timing.
 *
 * Ownership is not decided by address alone. A range table says which regions
 * our heap has been seen to occupy, and every block we issue carries an eight
 * byte header whose magic is keyed to the pointer it precedes. The range check
 * comes first because it cannot fault; the header is only read once the range
 * says the memory was ours, and it is what actually decides. A region that our
 * heap later hands back to the operating system can therefore never be mistaken
 * for ours if somebody else is given the same address.
 *
 * Allocations at or above GH_BIG stay with the runtime. A private heap satisfies
 * those with a dedicated VirtualAlloc that is released on free, which is the one
 * way a range in our table goes stale. They are also rare and long-lived, which
 * is to say they are not the state that fails to travel.
 *
 * Off unless D3D9SW_GAMEHEAP=1. This moves where the game's memory lives, and
 * nothing else in the engine has ever done that. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "savestate.h"

void savestate_log_line(const char *s);
int savestate_patch_iat(HMODULE mod, void *from, void *to);
int savestate_patch_iat_named(HMODULE mod, const char *dll, const char *fn, void *to,
			      void **prev);
void savestate_game_heap(HANDLE h);
void savestate_exclude(void *p, size_t bytes);
/* Defined further down with the note on why a fixed base is what makes a layout
 * reproducible. Declared here because the installer runs above it. */
static HANDLE gh_create_heap(void);
static void clock_probe_install(HMODULE exe);

static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

/* Big enough that a private heap serves it from its own reservation rather than
 * out of a segment, which is the case where a freed block releases address space
 * that somebody else can then be given. */
#define GH_BIG (256u * 1024u)
#define GH_REG 512
#define GH_MAGIC ((uintptr_t)0x9E3779B9u)

typedef struct {
	uintptr_t magic;
	size_t size;
} GhHead;

static HANDLE g_heap;
static CRITICAL_SECTION g_cs;
static int g_ready;

static uintptr_t g_lo[GH_REG], g_hi[GH_REG];
static volatile LONG g_nreg;

static unsigned long g_alloc, g_freed_ours, g_freed_theirs, g_toobig, g_regfull;
static unsigned long g_stale, g_fellback;

/* Offsets the operator asked to watch, parsed at install. See gh_peek_one. */
#define GH_PEEK_MAX 8
static unsigned g_peek_off[GH_PEEK_MAX];
static int g_peek_n;
static void gh_peek_free(void *u);
static void gh_peek_parse(void);
static void gh_vorbis_free(const void *u, unsigned size);

/* -------------------------------------------------- the allocation trace
 *
 * D3D9SW_GHTRACE=N records the first N allocator operations - what kind, how
 * big, and where in the heap the block landed - and writes them to gh_trace.txt
 * beside the log.
 *
 * The reason to have it is an observation that is currently resting on two data
 * points. The player entity has been found at heap base + 0x7F4B8 in two
 * separate sessions whose heap bases differed (0DFB0000 and 0DC70000), which
 * says the heap's internal placement is a function of the allocation sequence
 * and nothing else - the base moves, the contents do not. If that holds over
 * tens of thousands of operations rather than one lucky pointer, then a
 * snapshot is relocatable and an address in it can be derived from a single
 * anchor. If it does not hold, everything built on it falls over, and it is far
 * cheaper to find that out from a diff of two traces than from a restore that
 * faults.
 *
 * Two runs, diff the offset column. Identical means deterministic.
 *
 * Constraints this sits under: it runs inside the game's malloc, on every call,
 * so it must not allocate, must not lock, and must not touch a file. A slot is
 * claimed with one interlocked increment into a buffer reserved up front, and
 * the whole thing is written out later from the census, which already runs on a
 * frame boundary. A lock here would change the timing of the very thing being
 * measured.
 */
#define GH_TR_ALLOC 'A'  /* served from our heap */
#define GH_TR_CALLOC 'C' /* served from our heap, zeroed */
#define GH_TR_REALLOC 'R'
#define GH_TR_FREE 'F'
#define GH_TR_BIG 'B'	 /* over GH_BIG, handed to the runtime */
#define GH_TR_OTHER 'O'	 /* freed something that was never ours */

typedef struct {
	unsigned op;
	unsigned size;
	unsigned off; /* from the heap base, or ~0 when it is not in our heap */
	unsigned site; /* the code that asked, as an RVA, or ~0 */
	unsigned ord;  /* how many that site had already asked for, or ~0 */
} GhTr;

static GhTr *g_tr;
static volatile LONG g_tr_n;
static LONG g_tr_cap;

static void gh_trace_at(unsigned op, size_t n, const void *u, unsigned site, unsigned ord)
{
	LONG i;

	if (!g_tr)
		return;
	i = InterlockedIncrement(&g_tr_n) - 1;
	if (i >= g_tr_cap)
		return;
	g_tr[i].op = op;
	g_tr[i].size = (unsigned)n;
	/* A Windows heap handle is the address of its first segment, so the handle
	 * doubles as the base the offsets are measured from. */
	g_tr[i].off = (u && g_heap && (uintptr_t)u > (uintptr_t)g_heap)
			      ? (unsigned)((uintptr_t)u - (uintptr_t)g_heap)
			      : 0xFFFFFFFFu;
	g_tr[i].site = site;
	g_tr[i].ord = ord;
}

static void gh_trace(unsigned op, size_t n, const void *u)
{
	gh_trace_at(op, n, u, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- block tags
 *
 * An address says where a block landed, not which block it is. Two sessions put
 * 96% of blocks at matching offsets under the stock heap, but a matching offset
 * is not evidence by itself: with a thousand blocks in a bounded arena some of
 * them coincide, and nothing in an address distinguishes the coincidences from
 * the real matches.
 *
 * So a block is named when it is asked for rather than recognised afterwards.
 * The name is the call site that asked plus how many times that site has asked
 * for that size already - the n-th block down this chute. The site is stable
 * because it is a code offset in an executable that does not change between
 * launches; the count is stable for as long as the game takes the same path,
 * which is the property actually under test.
 *
 * Size belongs in the key, not just the payload. A call site reached from two
 * paths allocating two different things would otherwise interleave its counts
 * and produce an ordinal that means nothing in either.
 *
 * The tags live beside the heap in a table keyed by address, NOT in GhHead.
 * Widening that header would push every block along and destroy the offset
 * agreement these tags exist to measure - the measurement would change the
 * thing being measured.
 *
 * Same constraints as the trace: this runs inside the game's malloc, on every
 * call, so it must not allocate, must not lock and must not touch a file. Both
 * tables are reserved up front and reached with interlocked operations only.
 */
/* Sized for a long run rather than for the live set. Retired entries are
 * tombstoned so that probe chains stay walkable, and although an insert reuses
 * the first tombstone it meets, a lookup has to walk past them - so what fills
 * this table is the total churn of a session, not its high-water mark. A
 * ten-minute run allocates tens of thousands of blocks against a live set of
 * about a thousand. */
#define GH_TAG_SLOTS 262144u
#define GH_TAG_MASK (GH_TAG_SLOTS - 1u)
#define GH_TAG_DEAD ((LONG)1) /* freed, but the probe chain must run through it */
/* Far larger than the ~1,200 blocks live at once, because retired slots are
 * tombstoned and only reused opportunistically: the table has to absorb the
 * whole churn of a run, not the high-water mark. */
#define GH_SITE_SLOTS 16384u
#define GH_SITE_MASK (GH_SITE_SLOTS - 1u)
#define GH_PROBE 64

typedef struct {
	volatile LONG addr; /* 0 free, 1 tombstone, otherwise the user pointer */
	unsigned site;
	unsigned size;
	unsigned ord;
} GhTag;

typedef struct {
	volatile LONG key; /* 0 free, otherwise the mixed (site, size) */
	unsigned site;
	unsigned size;
	volatile LONG next; /* how many this chute has produced */
} GhSite;

static GhTag *g_tag;
static GhSite *g_site;
static uintptr_t g_exe;	    /* so a site is an RVA and survives relocation */
static volatile LONG g_tag_full, g_site_full, g_tag_congested;

static unsigned gh_mix(unsigned x)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

/* One frame, via the compiler, rather than a stack walk: the game is built with
 * frame pointers omitted in places, so the EBP chain cannot be trusted, and one
 * frame is enough to name the chute. That frame is genuinely the game's own code
 * - it links its runtime statically and we detour its private copies, so there
 * is no runtime thunk sitting between the caller and here. */
static unsigned gh_site_of(void *ra)
{
	uintptr_t a = (uintptr_t)ra;

	if (!g_exe || a < g_exe)
		return 0xFFFFFFFFu;
	return (unsigned)(a - g_exe);
}

/* Matching on the mixed key alone, not on (site, size) as well, because the two
 * fields are written after the slot is claimed and a reader could otherwise see
 * them half set. A key collision would merge two chutes' counts; at 32 bits
 * mixed that is rare enough to accept in a diagnostic, and g_site_full reports
 * the case that actually matters, which is the table filling up. */
static unsigned gh_ordinal(unsigned site, unsigned size)
{
	unsigned key = gh_mix(site ^ (size * 2654435761u));
	unsigned h, i;

	if (!g_site)
		return 0xFFFFFFFFu;
	if (!key)
		key = 1;
	h = key & GH_SITE_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_SITE_MASK;
		LONG was = InterlockedCompareExchange(&g_site[s].key, (LONG)key, 0);

		if (was == 0) {
			g_site[s].site = site;
			g_site[s].size = size;
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
		}
		if (was == (LONG)key)
			return (unsigned)(InterlockedIncrement(&g_site[s].next) - 1);
	}
	InterlockedIncrement(&g_site_full);
	return 0xFFFFFFFFu;
}

static void gh_tag_put(const void *u, unsigned site, unsigned size, unsigned ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG was = InterlockedCompareExchange(&g_tag[s].addr,
						      (LONG)(uintptr_t)u, 0);

		/* A tombstone is reusable: an address is live at most once, so
		 * there cannot be an entry for this one further down the chain. */
		if (was != 0) {
			was = InterlockedCompareExchange(&g_tag[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_TAG_DEAD);
			if (was != GH_TAG_DEAD)
				continue;
		}
		g_tag[s].site = site;
		g_tag[s].size = size;
		g_tag[s].ord = ord;
		return;
	}
	InterlockedIncrement(&g_tag_full);
}

/* Reads the tag and retires the slot in one go, because every caller is either
 * freeing the block or moving it. Leaving the entry behind would let the next
 * allocation to land on this address inherit a name that belongs to a block
 * that no longer exists, which is worse than having no name at all. */
static int gh_tag_take(const void *u, unsigned *site, unsigned *size, unsigned *ord)
{
	unsigned h, i;

	if (!g_tag || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_TAG_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_TAG_MASK;
		LONG a = g_tag[s].addr;

		if (a == 0)
			return 0; /* chain ended: this block was never tagged */
		if (a != (LONG)(uintptr_t)u)
			continue; /* including tombstones, which do not end it */
		*site = g_tag[s].site;
		*size = g_tag[s].size;
		*ord = g_tag[s].ord;
		InterlockedExchange(&g_tag[s].addr, GH_TAG_DEAD);
		return 1;
	}
	/* Reaching here means the probe ran out without meeting an empty slot,
	 * which is the table being too congested to search rather than the block
	 * being unknown - a different thing from the chain simply ending, and the
	 * one that means the numbers are degrading. Counted separately for that
	 * reason: walking off the end of a chain is normal and happens on every
	 * realloc that moved a block, since the tag was already taken. */
	InterlockedIncrement(&g_tag_congested);
	return 0;
}

/* Busy-block sidetable: heapwalk without invoking the heap.
 *
 * HeapWalk / HEAP_SEGMENT is a closed door on Wine. Scanning the saved
 * image for GhHead magic is a walk of the heap's bytes, not of the live
 * set: one session found 2 blocks against hundreds live, because the
 * scan jumped by aligned size and missed. We already intercept every
 * game malloc and free. The live (user, size) pairs are therefore a
 * thing we already know, kept in a table that is not itself on the heap.
 *
 * The live table is present-tense. A snapshot is taken after suspend_all
 * at save, packed into a second excluded array, and that is what restore
 * walks. After the copy, the live table is rewound to the snapshot so
 * free/realloc see the set the game believes is live.
 *
 * Same constraints as the tags: this runs inside the game's malloc, so
 * it must not allocate, must not lock, and must not touch a file. */
#define GH_BUSY_SLOTS 16384u
#define GH_BUSY_MASK (GH_BUSY_SLOTS - 1u)
#define GH_BUSY_DEAD ((LONG)1)

typedef struct {
	volatile LONG addr; /* 0 empty, 1 tomb, else the user pointer */
	size_t size;
} GhBusy;

typedef struct {
	void *head;
	size_t total;
} GhBusySnap;

static GhBusy *g_busy;
static volatile LONG g_busy_n, g_busy_full, g_busy_congested;
static GhBusySnap *g_busy_snap;
static unsigned g_busy_snap_n, g_busy_snap_cap;

static void gh_busy_put(const void *u, size_t n)
{
	unsigned h, i;

	if (!g_busy || !u)
		return;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG was = InterlockedCompareExchange(&g_busy[s].addr,
						      (LONG)(uintptr_t)u, 0);

		if (was != 0) {
			was = InterlockedCompareExchange(&g_busy[s].addr,
							 (LONG)(uintptr_t)u,
							 GH_BUSY_DEAD);
			if (was != GH_BUSY_DEAD)
				continue;
		}
		g_busy[s].size = n;
		InterlockedIncrement(&g_busy_n);
		return;
	}
	InterlockedIncrement(&g_busy_full);
}

static int gh_busy_take(const void *u)
{
	unsigned h, i;

	if (!g_busy || !u)
		return 0;
	h = gh_mix((unsigned)(uintptr_t)u) & GH_BUSY_MASK;
	for (i = 0; i < GH_PROBE; i++) {
		unsigned s = (h + i) & GH_BUSY_MASK;
		LONG a = g_busy[s].addr;

		if (a == 0)
			return 0;
		if (a != (LONG)(uintptr_t)u)
			continue;
		InterlockedExchange(&g_busy[s].addr, GH_BUSY_DEAD);
		InterlockedDecrement(&g_busy_n);
		return 1;
	}
	InterlockedIncrement(&g_busy_congested);
	return 0;
}

typedef void *(__cdecl *PFN_malloc)(size_t);
typedef void *(__cdecl *PFN_calloc)(size_t, size_t);
typedef void *(__cdecl *PFN_realloc)(void *, size_t);
typedef void(__cdecl *PFN_free)(void *);
typedef size_t(__cdecl *PFN_msize)(void *);
typedef void *(__cdecl *PFN_recalloc)(void *, size_t, size_t);
typedef void *(__cdecl *PFN_expand)(void *, size_t);

static PFN_malloc r_malloc;
static PFN_calloc r_calloc;
static PFN_realloc r_realloc;
static PFN_free r_free;
static PFN_msize r_msize;
static PFN_recalloc r_recalloc;
static PFN_expand r_expand;

/* Address only, and deliberately no dereference: this runs on every free in the
 * process, including pointers that were never ours. */
static int in_range(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	LONG n = g_nreg;
	LONG i;

	for (i = 0; i < n; i++)
		if (a >= g_lo[i] && a < g_hi[i])
			return 1;
	return 0;
}

/* The whole reservation the block sits in, so one lookup covers a segment rather
 * than a page. Only ever called when a new allocation lands outside everything
 * we already know about, which after warm-up is almost never. */
static void note_region(const void *p)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t base, end;
	LONG n;

	if (in_range(p))
		return;
	EnterCriticalSection(&g_cs);
	if (in_range(p) || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) ||
	    !mbi.AllocationBase) {
		LeaveCriticalSection(&g_cs);
		return;
	}
	base = (uintptr_t)mbi.AllocationBase;
	end = base;
	for (;;) {
		if (VirtualQuery((void *)end, &mbi, sizeof(mbi)) != sizeof(mbi))
			break;
		if ((uintptr_t)mbi.AllocationBase != base)
			break;
		end += mbi.RegionSize;
	}
	n = g_nreg;
	if (n < GH_REG && end > base) {
		g_lo[n] = base;
		g_hi[n] = end;
		/* Published last, so a reader that sees the count sees the bounds
		 * that go with it. */
		InterlockedIncrement(&g_nreg);
	} else if (n >= GH_REG) {
		g_regfull++;
	}
	LeaveCriticalSection(&g_cs);
}

/* The header, or nothing. Read only after in_range has agreed, so the memory is
 * inside a reservation our heap took and is safe to touch. The magic is keyed to
 * the pointer it precedes, so a stale range cannot launder somebody else's
 * block into ours. */
static GhHead *head_of(void *u)
{
	GhHead *h = (GhHead *)((char *)u - sizeof(GhHead));

	if (h->magic != (GH_MAGIC ^ (uintptr_t)u))
		return NULL;
	return h;
}

/* Two ways to not be ours, and they could not be further apart.
 *
 * A pointer outside every reservation we took belongs to somebody else and must
 * go back to the runtime untouched. A pointer INSIDE one of our reservations
 * whose header does not check out is a block we issued whose header has since
 * been overwritten - and handing that to the runtime means calling RtlFreeHeap
 * with the process heap's handle and an address the process heap never issued,
 * which is how a heap gets corrupted rather than how one gets freed.
 *
 * That is not hypothetical. A restore rewinds our heap wholesale, so a block the
 * game allocated after the save has, the moment the copy lands, a header from
 * whatever older thing occupied those bytes. The game frees it a few hundred
 * frames later - tearing down audio it thinks is still playing - the magic does
 * not match, and the block goes out through the runtime to a heap that never
 * owned it. The fault lands inside ntdll's free path reading an uncommitted page
 * in OUR arena, which reads as a Windows bug until you know where the pointer
 * came from.
 *
 * g_stale has been counting exactly this case since the day it was written, and
 * the report has been calling it "a hole worth reading about". It was.
 *
 * The caller learns which kind of miss it had so it can refuse rather than
 * forward. Split out here rather than re-tested at each site because in_range is
 * a linear walk and this runs on every free in the process. */
static GhHead *ours_why(void *u, int *orphan)
{
	GhHead *h;

	*orphan = 0;
	if (!u || !g_heap || !in_range(u))
		return NULL;
	h = head_of(u);
	if (!h) {
		g_stale++;
		*orphan = 1;
	}
	return h;
}

static void *give(void *raw, size_t n)
{
	GhHead *h = (GhHead *)raw;
	void *u = (char *)raw + sizeof(GhHead);

	h->magic = GH_MAGIC ^ (uintptr_t)u;
	h->size = n;
	note_region(u);
	gh_busy_put(u, n);
	g_alloc++;
	return u;
}

/* The detour targets are the thin wrappers below. The work is in the _at forms
 * so that gameheap's own internal calls - realloc moving a block, recalloc -
 * can pass the site they were given instead of naming a line in this file. */
static void *gh_malloc_at(size_t n, unsigned site)
{
	void *raw;

	if (!g_ready || n >= GH_BIG) {
		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		return r_malloc(n);
	}
	raw = HeapAlloc(g_heap, 0, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		return r_malloc(n);
	}
	{
		void *u = give(raw, n);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_ALLOC, n, u, site, ord);
		return u;
	}
}

static void *gh_malloc(size_t n)
{
	return gh_malloc_at(n, gh_site_of(__builtin_return_address(0)));
}

static void *gh_calloc_at(size_t c, size_t s, unsigned site)
{
	size_t n = c * s;
	void *raw;

	if (c && n / c != s)
		return NULL;
	if (!g_ready || n >= GH_BIG) {
		g_toobig += n >= GH_BIG;
		gh_trace(GH_TR_BIG, n, NULL);
		return r_calloc(c, s);
	}
	raw = HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		return r_calloc(c, s);
	}
	{
		void *u = give(raw, n);
		unsigned ord = gh_ordinal(site, (unsigned)n);

		gh_tag_put(u, site, (unsigned)n, ord);
		gh_trace_at(GH_TR_CALLOC, n, u, site, ord);
		return u;
	}
}

static void *gh_calloc(size_t c, size_t s)
{
	return gh_calloc_at(c, s, gh_site_of(__builtin_return_address(0)));
}

/* Leaking a block is a bounded cost. Corrupting the process heap is not, and it
 * is not even paid by the code that caused it - the damage surfaces later, in an
 * unrelated allocation, as a fault with nothing in its stack to explain itself.
 * So an orphan is dropped on the floor and counted, here and at every other
 * route out. The memory stays reserved in our arena, which is where it already
 * was and where nothing else will be given it. */
static unsigned long g_orphan_dropped;

static void gh_orphan_note(const char *what, void *p)
{
	if (!g_orphan_dropped++)
		ss_log("gameheap: %s of %p refused - the address is inside our arena "
		       "but its header is gone, so it is a block of ours whose head "
		       "was overwritten (a restore rewinding memory the game allocated "
		       "after the save does exactly this). Passing it to the runtime "
		       "would free it against a heap that never issued it. Dropped "
		       "instead; this leaks the block and keeps the heap intact\n",
		       what, p);
}

static void gh_free(void *p)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);

	if (!h) {
		if (orphan) {
			gh_orphan_note("free", p);
			return;
		}
		if (p)
			g_freed_theirs++;
		if (p)
			gh_trace(GH_TR_OTHER, 0, NULL);
		r_free(p);
		return;
	}
	h->magic = 0;
	g_freed_ours++;
	{
		unsigned site = 0xFFFFFFFFu, size = 0, ord = 0xFFFFFFFFu;

		if (!gh_tag_take(p, &site, &size, &ord))
			site = ord = 0xFFFFFFFFu;
		gh_trace_at(GH_TR_FREE, h->size, p, site, ord);
	}
	gh_peek_free(p);
	gh_vorbis_free(p, (unsigned)h->size);
	gh_busy_take(p);
	HeapFree(g_heap, 0, h);
}

static void *gh_realloc_at(void *p, size_t n, unsigned site)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw, *q;
	size_t old;
	/* A realloc does not create an object, it resizes one, so the block keeps
	 * the name it was given when it was first asked for. Minting a fresh tag
	 * here would make one logical block look like two different ones across
	 * sessions that happened to grow it at different moments. */
	unsigned osite = 0xFFFFFFFFu, osize = 0, oord = 0xFFFFFFFFu;
	int named = 0;

	if (!p)
		return gh_malloc_at(n, site);
	if (!h) {
		if (orphan) {
			/* No header means no size, so there is nothing to copy
			 * forward. A fresh block of the size asked for is the only
			 * answer available, and it is the same answer the texture
			 * path already gives for a payload it cannot read back:
			 * rebuild empty rather than invent contents. */
			gh_orphan_note("realloc", p);
			return gh_malloc_at(n, site);
		}
		/* Not ours, so it stays where it is. Growing it into our heap
		 * would mean copying a block whose true size only the runtime
		 * knows. */
		return r_realloc(p, n);
	}
	if (!n) {
		gh_free(p);
		return NULL;
	}
	old = h->size;
	named = gh_tag_take(p, &osite, &osize, &oord);
	if (n < GH_BIG) {
		raw = HeapReAlloc(g_heap, 0, h, n + sizeof(GhHead));
		if (raw)
			{
				void *u;

				gh_busy_take(p);
				u = give(raw, n);

				if (!named) {
					osite = site;
					oord = gh_ordinal(site, (unsigned)n);
				}
				gh_tag_put(u, osite, osize ? osize : (unsigned)n,
					   oord);
				gh_trace_at(GH_TR_REALLOC, n, u, osite, oord);
				return u;
			}
	}
	/* Either it outgrew what we keep, or the heap could not extend it. Move
	 * it out to the runtime rather than fail: a realloc that returns NULL
	 * without freeing is correct C and a leak in most callers. */
	q = n >= GH_BIG ? r_malloc(n) : gh_malloc_at(n, site);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	/* gh_malloc_at has just tagged q under this call site. Replace that with
	 * the name the block already had, so a move is invisible to identity. */
	if (named && n < GH_BIG) {
		unsigned d1, d2, d3;

		gh_tag_take(q, &d1, &d2, &d3);
		gh_tag_put(q, osite, osize, oord);
	}
	gh_free(p);
	return q;
}

static void *gh_realloc(void *p, size_t n)
{
	return gh_realloc_at(p, n, gh_site_of(__builtin_return_address(0)));
}

static size_t gh_msize(void *p)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);

	if (h)
		return h->size;
	if (orphan) {
		/* Asking the runtime would have it read a header in our arena as
		 * though it were one of its own. Zero is a poor answer and a safe
		 * one. */
		gh_orphan_note("_msize", p);
		return 0;
	}
	return r_msize(p);
}

static void *gh_recalloc(void *p, size_t c, size_t s)
{
	size_t n = c * s;
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	size_t old;
	void *q;
	/* Taken here and passed down, so the tag names the game's call site rather
	 * than the line below that forwards to it. */
	unsigned site = gh_site_of(__builtin_return_address(0));

	if (c && n / c != s)
		return NULL;
	if (!p)
		return gh_calloc_at(c, s, site);
	if (!h) {
		if (orphan) {
			gh_orphan_note("_recalloc", p);
			return gh_calloc_at(c, s, site);
		}
		return r_recalloc(p, c, s);
	}
	old = h->size;
	q = gh_realloc_at(p, n, site);
	if (q && n > old)
		memset((char *)q + old, 0, n - old);
	return q;
}

static void *gh_expand(void *p, size_t n)
{
	int orphan;
	GhHead *h = ours_why(p, &orphan);
	void *raw;

	if (!h) {
		if (orphan) {
			/* _expand never moves a block, so refusing is a normal
			 * answer the runtime already documents. */
			gh_orphan_note("_expand", p);
			return NULL;
		}
		return r_expand(p, n);
	}
	if (n >= GH_BIG)
		return NULL;
	raw = HeapReAlloc(g_heap, HEAP_REALLOC_IN_PLACE_ONLY, h, n + sizeof(GhHead));
	if (!raw)
		return NULL;
	h->size = n;
	gh_busy_take(p);
	gh_busy_put(p, n);
	return p;
}

/* Where the allocator actually is.
 *
 * The premise above is wrong in one detail that turns out to decide the method.
 * Rabi-Ribi does not link ucrtbase; it links the runtime statically. Its import
 * list is KERNEL32, USER32, GDI32, SHELL32 and STEAM_API and nothing else, so
 * the ucrtbase in the process belongs to Windows and the game never calls it.
 * That is why redirecting imports found nothing to redirect: there is no import
 * to redirect. The conclusion the log drew - that the game's runtime heap comes
 * from ucrtbase - named the right heap for the wrong reason.
 *
 * The allocator is in the game's own .text, and Ghidra found the whole of it by
 * working back from the floor that every allocator has to reach. Five functions
 * touch the runtime's heap handle, and only five:
 *
 *   malloc   0036E6B2 -> HeapAlloc      54 callers
 *   calloc   0037EBF7 -> HeapAlloc       2 callers   (__calloc_impl)
 *   realloc  0036E744 -> HeapReAlloc    15 callers
 *   free     00369754 -> HeapFree      107 callers
 *   _msize   0037805A -> HeapSize        2 callers
 *
 * All five read the handle from one global at 0118E588, which is __acrt_heap. A
 * static UCRT sets it to GetProcessHeap(), and that single assignment is the
 * origin of every ownership heuristic in savestate.c.
 *
 * The three other free-shaped functions in the image were the reason to go
 * looking. None of them touches HeapFree; they tail-call this one. The set is
 * closed, which is the property that makes hooking it safe - a block allocated
 * through us and released through a routine we missed would go to HeapFree on a
 * heap it does not belong to.
 *
 * Nothing outside the runtime shares these. The four other Heap* callers in the
 * image ask GetProcessHeap for themselves and free what they themselves
 * allocated; they never see a pointer of ours.
 *
 * So we detour the game's private copies rather than redirecting an import.
 * That is better than the original plan, not a fallback from it: the copies
 * belong to the game alone, so the redirect cannot reach Windows even by
 * accident. Windows keeps ucrtbase; the game gets a heap with no other tenant.
 *
 * Tempting and rejected: write our heap handle into __acrt_heap and change no
 * code at all. Every allocation already live at that moment came from the
 * process heap, and the next free would hand it to HeapFree against a heap that
 * never issued it. The swap is only safe before the runtime initialises, and we
 * are loaded by LoadLibrary long after. Detours dispatch per pointer, so the
 * mixed period is correct by construction. */
#define GH_PRO 7

/* Seven bytes because five is the jump and the eighth would split an
 * instruction. Both shapes end on an instruction boundary and neither contains
 * a relative operand, so the bytes can be moved to a trampoline unchanged. */
static const unsigned char kProSaveEsi[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x56,
						   0x8B, 0x75, 0x08 };
static const unsigned char kProTestArg[GH_PRO] = { 0x55, 0x8B, 0xEC, 0x83,
						   0x7D, 0x08, 0x00 };

struct Site {
	const char *name;
	unsigned rva; /* 0 where this target has no such function */
	const unsigned char *pro;
	void **real;
	void *ours;
};

/* Deliberately not the aligned family, and not _strdup.
 *
 * _aligned_malloc and _aligned_free are a closed pair: leave both alone and
 * every aligned block is allocated and released by the same allocator, which is
 * self-consistent. Hook one and not the other and it is not. Aligning by hand on
 * our own heap is possible but it is more surface than the state it would buy.
 *
 * _strdup allocates through malloc, so its blocks arrive here on their own.
 *
 * _recalloc and _expand carry no RVA because this executable does not contain
 * them. They stay in the table so the log says we looked rather than leaving the
 * reader to wonder, and so their handlers are not quietly dropped if a later
 * target does have them. */
static struct Site g_sites[] = {
	{ "malloc", 0x0036E6B2, kProSaveEsi, (void **)&r_malloc, NULL },
	{ "calloc", 0x0037EBF7, kProSaveEsi, (void **)&r_calloc, NULL },
	{ "realloc", 0x0036E744, kProTestArg, (void **)&r_realloc, NULL },
	{ "free", 0x00369754, kProTestArg, (void **)&r_free, NULL },
	{ "_msize", 0x0037805A, kProTestArg, (void **)&r_msize, NULL },
	{ "_recalloc", 0, NULL, (void **)&r_recalloc, NULL },
	{ "_expand", 0, NULL, (void **)&r_expand, NULL }
};

#define GH_SITES ((int)(sizeof(g_sites) / sizeof(g_sites[0])))

/* The bytes have to be the ones we were promised.
 *
 * An RVA is only meaningful against the build it was read from. Point it at a
 * patched executable, a different version, or a target that is not this game at
 * all, and five bytes of jump land in the middle of an instruction - which does
 * not fail, it misbehaves. So every site is checked against the prologue Ghidra
 * recorded, and one mismatch stops the whole install. Patching four of five is
 * worse than patching none. */
static int site_ok(HMODULE exe, const struct Site *s)
{
	const unsigned char *t = (const unsigned char *)exe + s->rva;
	MEMORY_BASIC_INFORMATION mbi;

	if (!VirtualQuery(t, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return 0;
	return memcmp(t, s->pro, GH_PRO) == 0;
}

/* The displaced bytes, then a jump back to what follows them. Built for every
 * site before any target is written, because the moment the first jump goes in
 * another thread can arrive in our free and needs the real one to forward to. */
static void site_tramp(HMODULE exe, struct Site *s, unsigned char *tr)
{
	unsigned char *t = (unsigned char *)exe + s->rva;

	memcpy(tr, t, GH_PRO);
	tr[GH_PRO] = 0xE9;
	*(LONG *)(tr + GH_PRO + 1) = (LONG)(t + GH_PRO - (tr + GH_PRO + 5));
	*s->real = tr;
}

static int site_wire(HMODULE exe, struct Site *s)
{
	unsigned char *t = (unsigned char *)exe + s->rva;
	DWORD old;

	if (!VirtualProtect(t, GH_PRO, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	t[0] = 0xE9;
	*(LONG *)(t + 1) = (LONG)((unsigned char *)s->ours - (t + 5));
	/* The two bytes the jump does not cover. Never executed, because nothing
	 * branches between a function's entry and its fourth byte, but a decoder
	 * reading them should see one instruction rather than half of one. */
	t[5] = 0x90;
	t[6] = 0x90;
	VirtualProtect(t, GH_PRO, old, &old);
	FlushInstructionCache(GetCurrentProcess(), t, GH_PRO);
	return 1;
}

/* The floor, under everything.
 *
 * The five detours cover the runtime's allocator, and the runtime's allocator
 * is not the only thing in this executable that frees memory. Three other
 * functions call HeapFree against GetProcessHeap() directly - a custom pair and
 * a teardown path - and they are not allocators we can replace: they take no
 * pointer argument, they read globals and release what they computed. There is
 * no entry point to detour.
 *
 * Whether any of them can ever be handed a block of ours is a question two
 * design notes wanted settled before arming anything. It does not need
 * settling, because it can be made moot. Every one of them reaches HeapFree
 * through a single import slot, and so does the runtime's own free. One patch
 * there puts an ownership check underneath every freer in the process,
 * including the ones nobody has identified.
 *
 * A count of zero here across a long session is the measurement those notes
 * asked for. A count above zero is a wrong-heap free that would have been
 * silent corruption. Either answer is worth having; only one of them needed
 * code to prevent. */
static BOOL(WINAPI *r_heapfree)(HANDLE, DWORD, LPVOID);
static unsigned long g_caught;

static BOOL WINAPI gh_heapfree(HANDLE heap, DWORD flags, LPVOID p)
{
	GhHead *h;

	int orphan = 0;

	/* Our own HeapFree calls do not come back through here - those go direct
	 * to the real one - so this only ever sees the game's. */
	if (g_ready && (h = ours_why(p, &orphan)) != NULL) {
		h->magic = 0;
		g_caught++;
		g_freed_ours++;
		gh_peek_free(p);
		gh_vorbis_free(p, (unsigned)h->size);
		gh_busy_take(p);
		return HeapFree(g_heap, flags, h);
	}
	/* The floor has to hold for the orphan case too, and this is the route the
	 * observed crash actually took: RtlFreeHeap called with the process heap's
	 * handle and an address inside our arena. */
	if (orphan) {
		gh_orphan_note("HeapFree", p);
		return TRUE;
	}
	return r_heapfree(heap, flags, p);
}

static int on(void)
{
	char v[8];
	DWORD n = savestate_getenv("D3D9SW_GAMEHEAP", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

int gameheap_install(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	unsigned char *pool;
	int live = 0, bad = 0, wired = 0;
	int i;

	g_sites[0].ours = (void *)gh_malloc;
	g_sites[1].ours = (void *)gh_calloc;
	g_sites[2].ours = (void *)gh_realloc;
	g_sites[3].ours = (void *)gh_free;
	g_sites[4].ours = (void *)gh_msize;
	g_sites[5].ours = (void *)gh_recalloc;
	g_sites[6].ours = (void *)gh_expand;

	if (g_ready || !on())
		return 0;
	if (!exe)
		return 0;
	/* Before any allocation is served, because the free path reads the result
	 * and the block we most want to watch is freed during startup. */
	gh_peek_parse();

	/* Look before touching anything. */
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		live++;
		if (site_ok(exe, &g_sites[i]))
			continue;
		bad++;
		ss_log("gameheap: %-10s at +%08X does not begin with the bytes this "
		       "build was told to expect\n",
		       g_sites[i].name, g_sites[i].rva);
	}
	if (!live || bad) {
		ss_log("gameheap: %d of %d allocator site(s) did not match, so nothing "
		       "was touched. These offsets were read from one build of one "
		       "executable and mean nothing against another\n",
		       bad, live);
		return 0;
	}

	/* Executable, and ours, so nothing the game does can reclaim it. */
	pool = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
					     PAGE_EXECUTE_READWRITE);
	if (!pool) {
		ss_log("gameheap: no memory for trampolines, error %lu\n", GetLastError());
		return 0;
	}
	{
		char v[16];
		unsigned cap = savestate_getenv("D3D9SW_GHTRACE", v, sizeof(v));
		long want = 0;
		unsigned k;

		for (k = 0; k < cap && v[k] >= '0' && v[k] <= '9'; k++)
			want = want * 10 + (v[k] - '0');
		if (want > 0) {
			/* Reserved before the first allocation is served, because a
			 * trace that starts late starts after the layout it is meant
			 * to explain has already been decided. */
			g_tr = (GhTr *)VirtualAlloc(NULL, (SIZE_T)want * sizeof(GhTr),
						    MEM_COMMIT | MEM_RESERVE,
						    PAGE_READWRITE);
			g_tr_cap = g_tr ? (LONG)want : 0;
			ss_log("gameheap: tracing the first %ld allocator operation(s) "
			       "into gh_trace.txt%s\n",
			       want, g_tr ? "" : " - RESERVATION FAILED, tracing off");
		}
		/* Armed with the trace, and reserved just as early: a tag table
		 * that starts late cannot name the blocks allocated before it,
		 * and those are exactly the long-lived ones worth naming. */
		if (g_tr) {
			g_exe = (uintptr_t)GetModuleHandleA(NULL);
			g_tag = (GhTag *)VirtualAlloc(NULL,
						      GH_TAG_SLOTS * sizeof(GhTag),
						      MEM_COMMIT | MEM_RESERVE,
						      PAGE_READWRITE);
			g_site = (GhSite *)VirtualAlloc(NULL,
							GH_SITE_SLOTS * sizeof(GhSite),
							MEM_COMMIT | MEM_RESERVE,
							PAGE_READWRITE);
			ss_log("gameheap: naming blocks by (call site, ordinal) "
			       "against image base %08lX%s\n",
			       (unsigned long)g_exe,
			       (g_tag && g_site) ? ""
						 : " - TABLE RESERVATION FAILED, "
						   "blocks will be unnamed");
		}
	}
	g_heap = gh_create_heap();
	if (!g_heap) {
		ss_log("gameheap: HeapCreate failed, error %lu\n", GetLastError());
		VirtualFree(pool, 0, MEM_RELEASE);
		return 0;
	}
	InitializeCriticalSection(&g_cs);

	/* Always, not only under GHTRACE: the tag table names blocks for a
	 * diagnostic; this table IS the live set restore walks. Excluded so a
	 * rewind cannot take the journal away while it is being used to rewind
	 * the heap the journal describes. */
	g_busy = (GhBusy *)VirtualAlloc(NULL, GH_BUSY_SLOTS * sizeof(GhBusy),
					MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_busy_snap = (GhBusySnap *)VirtualAlloc(NULL,
						GH_BUSY_SLOTS * sizeof(GhBusySnap),
						MEM_COMMIT | MEM_RESERVE,
						PAGE_READWRITE);
	if (g_busy && g_busy_snap) {
		g_busy_snap_cap = GH_BUSY_SLOTS;
		savestate_exclude(g_busy, GH_BUSY_SLOTS * sizeof(GhBusy));
		savestate_exclude(g_busy_snap,
				  GH_BUSY_SLOTS * sizeof(GhBusySnap));
		ss_log("gameheap: busy-block sidetable at %p (%u slots), snapshot "
		       "at %p - Wine restore walks this instead of HeapWalk or a "
		       "linear GhHead scan\n",
		       (void *)g_busy, GH_BUSY_SLOTS, (void *)g_busy_snap);
	} else {
		if (g_busy) {
			VirtualFree(g_busy, 0, MEM_RELEASE);
			g_busy = NULL;
		}
		if (g_busy_snap) {
			VirtualFree(g_busy_snap, 0, MEM_RELEASE);
			g_busy_snap = NULL;
		}
		ss_log("gameheap: busy-block sidetable reservation failed - restore "
		       "falls back to scanning saved bytes for GhHead magic\n");
	}

	/* Every forwarding path complete before any jump exists, then ready, then
	 * the jumps. A call arriving midway through the last step reaches a
	 * handler that can already forward. */
	for (i = 0; i < GH_SITES; i++)
		if (g_sites[i].rva)
			site_tramp(exe, &g_sites[i], pool + (size_t)i * 16);
	g_ready = 1;
	for (i = 0; i < GH_SITES; i++) {
		if (!g_sites[i].rva)
			continue;
		if (site_wire(exe, &g_sites[i]))
			wired++;
		else
			ss_log("gameheap: %-10s could not be made writable\n",
			       g_sites[i].name);
	}
	{
		void *prev = NULL;
		/* By name. kernel32!HeapFree is a forwarder to ntdll!RtlFreeHeap,
		 * so the address GetProcAddress returns is not necessarily the one
		 * in the import slot - which is why matching by address found
		 * nothing and said so. */
		int n = savestate_patch_iat_named(exe, "KERNEL32.dll", "HeapFree",
						  (void *)gh_heapfree, &prev);

		if (!n)
			n = savestate_patch_iat_named(exe, NULL, "HeapFree",
						      (void *)gh_heapfree, &prev);
		r_heapfree = n ? (BOOL(WINAPI *)(HANDLE, DWORD, LPVOID))prev : NULL;
		ss_log("gameheap: HeapFree %d import slot(s) patched as a floor under "
		       "every freer in the executable, the three that are not "
		       "allocators included%s\n",
		       n, n ? "" : " - the census below cannot measure what it claims");
	}
	clock_probe_install(exe);
	savestate_game_heap(g_heap);
	ss_log("gameheap: %d of %d allocator site(s) in the game's own code now run "
	       "on a private heap at %p. The runtime is linked statically, so these "
	       "are the game's copies and nothing Windows uses passes through them. "
	       "Allocations under %lu KB from here on have no other tenant, which is "
	       "the arrangement DDPR gets from MSVCR100 for free\n",
	       wired, live, (void *)g_heap, (unsigned long)(GH_BIG >> 10));
	return wired;
}

/* ------------------------------------------------- where the heap itself goes
 *
 * D3D9SW_GHPIN puts the private heap at an address we choose instead of one
 * Windows chooses.
 *
 * This is the change that makes a layout reproducible, and the evidence for it
 * is unusually direct. Two sessions were made to issue byte-identical
 * allocation sequences - same 2,133 operations, same order - and replayed
 * against four allocators. HeapCreate put 93% of the blocks at different
 * offsets both times. Disabling the low-fragmentation heap changed nothing,
 * which killed the obvious theory. RtlCreateHeap at a fixed base reproduced
 * every block, at every checkpoint, exactly.
 *
 * The only difference between those cases is the base, so the base is the
 * cause: HeapCreate has no address parameter, Windows picks one, and the
 * internal layout follows from it.
 *
 * Worth being clear about why an address matters when a snapshot could simply
 * record offsets. It is not the snapshot that needs the address, it is the
 * contents: the blocks are full of pointers to each other, and a pointer is
 * only meaningful against the layout it was written for. Restoring the bytes
 * faithfully into a heap that moved is how a restored pointer ends up aimed at
 * whatever now occupies that address.
 *
 * A caller-supplied base means the heap cannot grow - there is nothing after
 * the reservation it is entitled to take. That is deliberate. Peak live in this
 * heap has been measured at 0.47 MB and everything at or above 256 KB is handed
 * to the runtime anyway, so the reservation below is orders of magnitude clear;
 * and if it is ever wrong, gh_malloc already falls back to the runtime and
 * g_fellback counts it, so the failure is a report rather than a crash.
 */
#define GH_PIN_BASE 0x50000000u

static PVOID(NTAPI *p_RtlCreateHeap)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID, PVOID);

static unsigned gh_knob(const char *name, unsigned def)
{
	char v[24];
	unsigned n = savestate_getenv(name, v, sizeof(v));
	unsigned got = 0, k = 0;
	int hex = (n > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'));

	if (!n)
		return def;
	for (k = hex ? 2 : 0; k < n; k++) {
		if (v[k] >= '0' && v[k] <= '9')
			got = got * (hex ? 16 : 10) + (unsigned)(v[k] - '0');
		else if (hex && v[k] >= 'a' && v[k] <= 'f')
			got = got * 16 + (unsigned)(v[k] - 'a') + 10;
		else if (hex && v[k] >= 'A' && v[k] <= 'F')
			got = got * 16 + (unsigned)(v[k] - 'A') + 10;
		else
			break;
	}
	return got;
}

static HANDLE gh_create_heap(void)
{
	unsigned want = gh_knob("D3D9SW_GHPIN", 0);
	unsigned mb = gh_knob("D3D9SW_GHPIN_MB", 64);
	uintptr_t base;
	SIZE_T size;
	void *res;
	HMODULE nt;
	HANDLE h;

	if (!want)
		return HeapCreate(0, 1u << 20, 0);
	/* 1 means "pin it, you pick"; anything larger is an address. */
	base = (want == 1) ? GH_PIN_BASE : (uintptr_t)want;
	size = (SIZE_T)mb * 1024u * 1024u;

	nt = GetModuleHandleA("ntdll.dll");
	p_RtlCreateHeap = nt ? (PVOID(NTAPI *)(ULONG, PVOID, SIZE_T, SIZE_T, PVOID,
					       PVOID))GetProcAddress(nt, "RtlCreateHeap")
			     : NULL;
	if (!p_RtlCreateHeap) {
		ss_log("gameheap: RtlCreateHeap is not exported, so the heap cannot "
		       "be pinned - falling back to HeapCreate and a layout that "
		       "will not reproduce\n");
		return HeapCreate(0, 1u << 20, 0);
	}
	/* Reserved first and checked, rather than trusting RtlCreateHeap to honour
	 * the address: landing somewhere else silently would look like success and
	 * produce a session whose traces quietly do not compare. */
	res = VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_READWRITE);
	if (!res || (uintptr_t)res != base) {
		ss_log("gameheap: cannot reserve %u MB at %08lX (got %p, error %lu) - "
		       "falling back to HeapCreate\n",
		       mb, (unsigned long)base, res, GetLastError());
		if (res)
			VirtualFree(res, 0, MEM_RELEASE);
		return HeapCreate(0, 1u << 20, 0);
	}
	h = (HANDLE)p_RtlCreateHeap(0, res, size, 1u << 20, NULL, NULL);
	if (!h) {
		ss_log("gameheap: RtlCreateHeap refused the base %08lX - falling back "
		       "to HeapCreate\n",
		       (unsigned long)base);
		VirtualFree(res, 0, MEM_RELEASE);
		return HeapCreate(0, 1u << 20, 0);
	}
	ss_log("gameheap: heap PINNED at %p, %u MB reserved, not growable. Two "
	       "sessions that issue the same allocations should now place every "
	       "block at the same address, not merely at the same set of "
	       "addresses\n",
	       (void *)h, mb);
	if ((uintptr_t)h != base)
		ss_log("gameheap: NOTE the handle %p is not the base %08lX we asked "
		       "for - offsets are measured from the handle\n",
		       (void *)h, (unsigned long)base);
	return h;
}

/* Written from the census rather than from the allocator, because this opens a
 * file and formats text and neither belongs on a path the game takes millions of
 * times. Rewritten in full each time rather than appended, so the file always
 * describes one run from its first allocation and two runs can be diffed
 * directly. */
static void gh_trace_write(const char *path)
{
	HANDLE f;
	char line[64];
	LONG n = g_tr_n, i;
	DWORD wrote = 0;

	if (!g_tr)
		return;
	if (n > g_tr_cap)
		n = g_tr_cap;
	f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		ss_log("gameheap: cannot write %s, error %lu\n", path, GetLastError());
		return;
	}
	/* The base goes in a comment rather than into the offsets, which are what a
	 * diff compares. Two runs with different bases and identical offset columns
	 * is the whole result. */
	wsprintfA(line, "# heap %08lX  %ld op(s)%s\r\n", (unsigned long)(uintptr_t)g_heap,
		  (long)n, (g_tr_n > g_tr_cap) ? "  TRUNCATED" : "");
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	/* site is an RVA into the executable and ordinal is the count that site
	 * had already reached, so the pair names the block independently of where
	 * it landed. Readers that only want the first three columns still work:
	 * the extra two are appended, not inserted. */
	wsprintfA(line, "# image %08lX\r\n", (unsigned long)g_exe);
	WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
	WriteFile(f, "# op size offset site ordinal\r\n", 31, &wrote, NULL);
	for (i = 0; i < n; i++) {
		int k = wsprintfA(line, "%c %08lX %08lX %08lX %08lX\r\n",
				  (char)g_tr[i].op, (unsigned long)g_tr[i].size,
				  (unsigned long)g_tr[i].off,
				  (unsigned long)g_tr[i].site,
				  (unsigned long)g_tr[i].ord);
		WriteFile(f, line, (DWORD)k, &wrote, NULL);
	}
	CloseHandle(f);
	ss_log("gameheap: %s written, %ld of %ld operation(s)%s\n", path, (long)n,
	       (long)g_tr_n,
	       (g_tr_n > g_tr_cap) ? " - the buffer filled, raise D3D9SW_GHTRACE" : "");
	if (g_tag_full || g_site_full || g_tag_congested)
		ss_log("gameheap: %ld block(s) and %ld chute(s) went unnamed, %ld "
		       "lookup(s) gave up searching - the tag tables are too small "
		       "for a run this long, so names after that point are short\n",
		       (long)g_tag_full, (long)g_site_full, (long)g_tag_congested);
}

/* --------------------------------------------- where the session seed enters
 *
 * D3D9SW_CLOCKPROBE=1 reports every call the game makes to the three
 * calendar-time functions it imports, with the caller's RVA and - the part that
 * makes it useful - the allocator operation number at the time of the call.
 *
 * The question it answers. Two sessions from an identical save allocate
 * identically for 3,363 operations and then differ in the length of one string.
 * Something the game knows must therefore differ before that point while
 * leaving no trace in the heap until it is formatted, and a value read from the
 * calendar is the obvious candidate: it changes every run by construction, it
 * is usually stored in a fixed-size structure or a register rather than an
 * allocation, and it becomes visible exactly when somebody prints it.
 *
 * The operation number is what turns this from a list into evidence. Clock
 * calls and allocations are two streams with no common clock of their own; the
 * trace index is a counter both can be read against, so a call reported at
 * operation 3,300 is one that happened in the window where the divergence was
 * introduced, and a call at operation 7,000 is not.
 *
 * Only the calendar functions are hooked, not QueryPerformanceCounter or
 * timeGetTime. Those are read every frame and would bury the rare calls that
 * matter under thousands that do not - and a per-frame timer is a poor seed
 * anyway, being exactly what the game would use for animation.
 *
 * These are imports, so patching an import slot is enough. That is not true of
 * the allocator, which the game links statically and which had to be detoured
 * instruction by instruction. */
static void(WINAPI *r_GetLocalTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTime)(LPSYSTEMTIME);
static void(WINAPI *r_GetSystemTimeAsFileTime)(LPFILETIME);
static volatile LONG g_clk_n;

static void clk_note(const char *what, void *ra)
{
	LONG i = InterlockedIncrement(&g_clk_n);
	uintptr_t a = (uintptr_t)ra;

	/* Bounded, because a call in a loop would otherwise fill the log with the
	 * one thing we already know. The first calls are the ones that can have
	 * seeded anything. */
	if (i > 48)
		return;
	if (g_exe && a >= g_exe)
		ss_log("clock: %s called from +%08lX at allocator operation %ld\n",
		       what, (unsigned long)(a - g_exe), (long)g_tr_n);
	else
		ss_log("clock: %s called from %p (outside the executable) at "
		       "allocator operation %ld\n",
		       what, ra, (long)g_tr_n);
}

static void WINAPI gh_GetLocalTime(LPSYSTEMTIME t)
{
	clk_note("GetLocalTime", __builtin_return_address(0));
	r_GetLocalTime(t);
}

static void WINAPI gh_GetSystemTime(LPSYSTEMTIME t)
{
	clk_note("GetSystemTime", __builtin_return_address(0));
	r_GetSystemTime(t);
}

static void WINAPI gh_GetSystemTimeAsFileTime(LPFILETIME t)
{
	clk_note("GetSystemTimeAsFileTime", __builtin_return_address(0));
	r_GetSystemTimeAsFileTime(t);
}

static void clock_probe_install(HMODULE exe)
{
	void *prev;
	int n = 0;

	if (!gh_knob("D3D9SW_CLOCKPROBE", 0))
		return;
	if (!g_exe)
		g_exe = (uintptr_t)exe;
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetLocalTime",
				      (void *)gh_GetLocalTime, &prev) &&
	    prev) {
		r_GetLocalTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTime",
				      (void *)gh_GetSystemTime, &prev) &&
	    prev) {
		r_GetSystemTime = (void(WINAPI *)(LPSYSTEMTIME))prev;
		n++;
	}
	prev = NULL;
	if (savestate_patch_iat_named(exe, "KERNEL32.dll", "GetSystemTimeAsFileTime",
				      (void *)gh_GetSystemTimeAsFileTime, &prev) &&
	    prev) {
		r_GetSystemTimeAsFileTime = (void(WINAPI *)(LPFILETIME))prev;
		n++;
	}
	ss_log("clock: %d of 3 calendar import(s) patched - every call will be "
	       "reported with the allocator operation number it happened at, so "
	       "it can be lined up against where two runs part\n",
	       n);
}

/* D3D9SW_GHPEEK=off,off,... prints what is actually stored at those heap
 * offsets, as hex and as text.
 *
 * Added for one specific question. Two runs of the game, from an identical save
 * and visibly doing the same things, allocate identically for 3,363 operations
 * and then differ in the size of a single block: 0x25 bytes in one, 0x2E in the
 * other, from the same call site with the same ordinal at the same offset. That
 * 9-byte difference rounds up to an 8-byte shift and drags every later block
 * along with it, which is the whole of the disagreement between those runs.
 *
 * A size that changes between runs at a fixed point in the sequence is almost
 * always a string, and a string can be read. The trace can say a block changed
 * size; only its contents can say why.
 *
 * Reading is guarded by VirtualQuery rather than assumed safe: the offset comes
 * from a human typing into a config file, and a wrong one would otherwise take
 * the game down inside a diagnostic. */
static void gh_peek_one(unsigned off)
{
	MEMORY_BASIC_INFORMATION mbi;
	const unsigned char *p = (const unsigned char *)g_heap + off;
	char line[128];
	unsigned row;

	if (!g_heap)
		return;
	if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
	    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
		ss_log("gameheap: peek +%08lX is not readable\n", (unsigned long)off);
		return;
	}
	for (row = 0; row < 4; row++) {
		const unsigned char *r = p + row * 16;
		char *o = line;
		unsigned k;

		o += wsprintfA(o, "gameheap: peek +%08lX ", (unsigned long)(off + row * 16));
		for (k = 0; k < 16; k++)
			o += wsprintfA(o, "%02X ", r[k]);
		*o++ = '|';
		for (k = 0; k < 16; k++)
			*o++ = (r[k] >= 32 && r[k] < 127) ? (char)r[k] : '.';
		*o++ = '|';
		*o = 0;
		ss_log("%s\n", line);
	}
}

/* Parsed once at install rather than read at each use. The free path calls into
 * this, and reading a knob from there would put config machinery underneath
 * every free in the process. */
static void gh_peek_parse(void)
{
	char v[96];
	unsigned n = savestate_getenv("D3D9SW_GHPEEK", v, sizeof(v));
	unsigned k, cur = 0;
	int any = 0;

	g_peek_n = 0;
	if (!n || n >= sizeof(v))
		return;
	for (k = 0; k <= n; k++) {
		char c = (k < n) ? v[k] : 0;

		if (c >= '0' && c <= '9') {
			cur = cur * 16 + (unsigned)(c - '0');
			any = 1;
		} else if (c >= 'a' && c <= 'f') {
			cur = cur * 16 + (unsigned)(c - 'a') + 10;
			any = 1;
		} else if (c >= 'A' && c <= 'F') {
			cur = cur * 16 + (unsigned)(c - 'A') + 10;
			any = 1;
		} else if (c == 'x' || c == 'X') {
			cur = 0; /* tolerate a 0x prefix */
		} else {
			if (any && g_peek_n < GH_PEEK_MAX)
				g_peek_off[g_peek_n++] = cur;
			cur = 0;
			any = 0;
		}
	}
}

static void gh_peek(void)
{
	int i;

	for (i = 0; i < g_peek_n; i++)
		gh_peek_one(g_peek_off[i]);
}

/* The save is the wrong moment for a transient.
 *
 * The block this was built to read - the 37-vs-46 byte string at +0xB8610 that
 * splits two otherwise identical sessions - is allocated during startup and
 * freed long before any save. Peeking at report time found four saves' worth of
 * zeroes, which says only that nothing had reused the address yet.
 *
 * A freed block still holds what was written to it right up until the allocator
 * takes it back, so the last honest moment to read one is the instant before it
 * goes. That is here. */
/* -------------------------------------------------- the vorbis witness
 *
 * The block whose size differs between two runs turned out to be the Vorbis
 * vendor string: 0x25 holds "Xiph.Org libVorbis I 20150105 (????)" and 0x2E
 * holds the 20140122 one, whose codename is Latin-1 and so 45 bytes rather than
 * the 47 an ASCII transliteration of it would suggest. This comment named the
 * 20101101 build before the run that settled it, which is a trap worth leaving
 * marked: that build's vendor string is also 45 bytes, so the arithmetic alone
 * cannot tell the two apart and only the logged string can. The music was not
 * all encoded with the same
 * library version, so the allocator's stream depends on which track is being
 * decoded - that is game state, not chance. Naming the track at the operation
 * number it loads at turns the divergence from a nuisance into a coordinate.
 *
 * Reads only, on the free path, capped so a long session cannot bury the log. */
#define GH_VORB_LINES 96
static int g_vorb_on = -1;
static LONG g_vorb_said;

static void gh_vorb_text(char *out, int cap, const unsigned char *p, unsigned n)
{
	unsigned i;

	if ((unsigned)(cap - 1) < n)
		n = (unsigned)(cap - 1);
	for (i = 0; i < n; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
	}
	out[i] = 0;
}

static void gh_vorbis_free(const void *u, unsigned size)
{
	const unsigned char *p = (const unsigned char *)u;
	char text[128];
	unsigned i;
	int tag = 0;

	if (g_vorb_on < 0)
		g_vorb_on = (int)gh_knob("D3D9SW_GHVORBIS", 0);
	if (!g_vorb_on || !u || size < 8 || size > 512)
		return;
	if (g_vorb_said >= GH_VORB_LINES)
		return;

	if (size >= 19 && !memcmp(p, "Xiph.Org libVorbis", 18)) {
		gh_vorb_text(text, sizeof(text), p, size);
		InterlockedIncrement(&g_vorb_said);
		ss_log("vorbis: comment header freed at allocator operation %ld - "
		       "%lu byte(s), 0x%lX, vendor \"%s\"\n",
		       (long)g_tr_n, (unsigned long)size, (unsigned long)size, text);
		return;
	}

	/* A comment is KEY=value in plain ASCII. TITLE is the one that would name
	 * the track; the others are kept because this encoder wrote very few and
	 * whichever it did write is the only handle on identity we have. */
	for (i = 0; i < size; i++) {
		unsigned char c = p[i];
		if (!c)
			break;
		if (c == '=' && i)
			tag = 1;
		else if (c < 0x20 || c >= 0x7F)
			return;
	}
	if (!tag || i < 3)
		return;
	gh_vorb_text(text, sizeof(text), p, size);
	InterlockedIncrement(&g_vorb_said);
	ss_log("vorbis: tag freed at allocator operation %ld - \"%s\"\n",
	       (long)g_tr_n, text);
}

static void gh_peek_free(void *u)
{
	unsigned off;
	int i;

	if (g_peek_n <= 0 || !u || !g_heap)
		return;
	off = (unsigned)((char *)u - (char *)g_heap);
	for (i = 0; i < g_peek_n; i++) {
		if (g_peek_off[i] != off)
			continue;
		ss_log("gameheap: the watched block at +%08lX is being freed - this is "
		       "what it held:\n",
		       (unsigned long)off);
		gh_peek_one(off);
		return;
	}
}

/* Numbered as well as named, because a run can now be cut at several frames and
 * the comparison is per cut: gh_trace2.txt from one session against gh_trace2.txt
 * from another. gh_trace.txt keeps holding the latest, so anything that already
 * reads it carries on working. */
static void gh_trace_dump(void)
{
	static int cut;
	char name[32];

	if (!g_tr)
		return;
	/* The frame-1 save lands in the same frame the heap is installed, so it
	 * would otherwise write an empty trace over gh_trace.txt and peek at a
	 * heap that has served nothing. An empty dump answers no question. */
	if (g_tr_n <= 0) {
		ss_log("gameheap: nothing traced yet, the dump is skipped so the "
		       "last real one survives\n");
		return;
	}
	cut++;
	wsprintfA(name, "gh_trace%d.txt", cut);
	gh_trace_write(name);
	gh_trace_write("gh_trace.txt");
	gh_peek();
}

void gameheap_report(void)
{
	gh_trace_dump();
	if (!g_ready) {
		if (on())
			ss_log("gameheap: asked for, but not installed\n");
		return;
	}
	ss_log("gameheap: %lu allocation(s) served, %lu freed to us, %lu passed back "
	       "to the runtime, %lu too big to take, %lu region(s) mapped, %ld "
	       "busy live (sidetable)%s\n",
	       g_alloc, g_freed_ours, g_freed_theirs, g_toobig, (unsigned long)g_nreg,
	       (long)g_busy_n, g_busy ? "" : " - NO TABLE");
	if (g_busy_full || g_busy_congested)
		ss_log("gameheap: %ld busy insert(s) missed the table, %ld take(s) "
		       "gave up searching - the sidetable is too small or too "
		       "full of tombstones\n",
		       (long)g_busy_full, (long)g_busy_congested);
	/* The number two design notes wanted before anything was armed. Zero says
	 * the runtime's allocator and the executable's other Heap callers never
	 * traded a block; anything else says they do, and says we caught it. */
	ss_log("gameheap: %lu block(s) of ours reached HeapFree by a route that is "
	       "not the runtime's free%s\n",
	       g_caught,
	       r_heapfree ? "" : " - UNMEASURED, the HeapFree floor is not in");
	if (g_orphan_dropped)
		ss_log("gameheap: %lu block(s) dropped rather than freed - inside our "
		       "arena with no header, so each one would have gone to a heap "
		       "that never issued it%s\n",
		       g_orphan_dropped,
		       " <<< every one of those was a heap corruption avoided");
	if (g_fellback || g_regfull || g_stale)
		ss_log("gameheap: %lu allocation(s) fell back to the runtime, %lu region(s) "
		       "past the table, %lu pointer(s) inside our range with no header - "
		       "any of the three is a hole worth reading about before trusting "
		       "this\n",
		       g_fellback, g_regfull, g_stale);
}

/* HEAPBLOCKS for this heap under Wine, without HeapWalk. Wine has no Windows
 * HEAP_SEGMENT, so the engine used to memcpy the whole region - lists, TLS
 * objects a LEFT RUNNING mmdevapi thread still holds, everything. The magic
 * is keyed to the user pointer, so a word that merely looks busy does not
 * match. */
int gameheap_saved_block(const void *saved, void *live_head, size_t remain, size_t *n)
{
	const GhHead *h;
	uintptr_t user;
	size_t total;

	if (!saved || !live_head || !n || remain < sizeof(GhHead))
		return 0;
	h = (const GhHead *)saved;
	user = (uintptr_t)live_head + sizeof(GhHead);
	if (h->magic != (GH_MAGIC ^ user))
		return 0;
	if (!h->size || h->size >= GH_BIG)
		return 0;
	total = sizeof(GhHead) + h->size;
	if (total > remain)
		return 0;
	*n = total;
	return 1;
}

unsigned gameheap_busy_snapshot(void)
{
	unsigned i, n = 0;

	g_busy_snap_n = 0;
	if (!g_busy || !g_busy_snap)
		return 0;
	for (i = 0; i < GH_BUSY_SLOTS; i++) {
		LONG a = g_busy[i].addr;
		size_t sz;

		if (a == 0 || a == GH_BUSY_DEAD)
			continue;
		sz = g_busy[i].size;
		if (!sz || sz >= GH_BIG)
			continue;
		if (n >= g_busy_snap_cap)
			break;
		g_busy_snap[n].head = (char *)(uintptr_t)a - sizeof(GhHead);
		g_busy_snap[n].total = sizeof(GhHead) + sz;
		n++;
	}
	g_busy_snap_n = n;
	return n;
}

unsigned gameheap_busy_saved_count(void)
{
	return g_busy_snap_n;
}

int gameheap_busy_saved_at(unsigned i, void **head, size_t *total)
{
	if (!head || !total || !g_busy_snap || i >= g_busy_snap_n)
		return 0;
	*head = g_busy_snap[i].head;
	*total = g_busy_snap[i].total;
	return 1;
}

void gameheap_busy_rewind(void)
{
	unsigned i;

	if (!g_busy || !g_busy_snap || !g_busy_snap_n)
		return;
	memset(g_busy, 0, GH_BUSY_SLOTS * sizeof(GhBusy));
	g_busy_n = 0;
	g_busy_full = 0;
	g_busy_congested = 0;
	for (i = 0; i < g_busy_snap_n; i++) {
		void *u = (char *)g_busy_snap[i].head + sizeof(GhHead);
		size_t sz = g_busy_snap[i].total - sizeof(GhHead);

		gh_busy_put(u, sz);
	}
}
