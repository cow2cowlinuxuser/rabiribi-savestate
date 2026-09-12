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
void savestate_game_heap(HANDLE h);

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

static GhHead *ours(void *u)
{
	GhHead *h;

	if (!u || !g_heap || !in_range(u))
		return NULL;
	h = head_of(u);
	if (!h)
		g_stale++;
	return h;
}

static void *give(void *raw, size_t n)
{
	GhHead *h = (GhHead *)raw;
	void *u = (char *)raw + sizeof(GhHead);

	h->magic = GH_MAGIC ^ (uintptr_t)u;
	h->size = n;
	note_region(u);
	g_alloc++;
	return u;
}

static void *gh_malloc(size_t n)
{
	void *raw;

	if (!g_ready || n >= GH_BIG) {
		g_toobig += n >= GH_BIG;
		return r_malloc(n);
	}
	raw = HeapAlloc(g_heap, 0, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		return r_malloc(n);
	}
	return give(raw, n);
}

static void *gh_calloc(size_t c, size_t s)
{
	size_t n = c * s;
	void *raw;

	if (c && n / c != s)
		return NULL;
	if (!g_ready || n >= GH_BIG) {
		g_toobig += n >= GH_BIG;
		return r_calloc(c, s);
	}
	raw = HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n + sizeof(GhHead));
	if (!raw) {
		g_fellback++;
		return r_calloc(c, s);
	}
	return give(raw, n);
}

static void gh_free(void *p)
{
	GhHead *h = ours(p);

	if (!h) {
		if (p)
			g_freed_theirs++;
		r_free(p);
		return;
	}
	h->magic = 0;
	g_freed_ours++;
	HeapFree(g_heap, 0, h);
}

static void *gh_realloc(void *p, size_t n)
{
	GhHead *h = ours(p);
	void *raw, *q;
	size_t old;

	if (!p)
		return gh_malloc(n);
	if (!h) {
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
	if (n < GH_BIG) {
		raw = HeapReAlloc(g_heap, 0, h, n + sizeof(GhHead));
		if (raw)
			return give(raw, n);
	}
	/* Either it outgrew what we keep, or the heap could not extend it. Move
	 * it out to the runtime rather than fail: a realloc that returns NULL
	 * without freeing is correct C and a leak in most callers. */
	q = n >= GH_BIG ? r_malloc(n) : gh_malloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	gh_free(p);
	return q;
}

static size_t gh_msize(void *p)
{
	GhHead *h = ours(p);

	return h ? h->size : r_msize(p);
}

static void *gh_recalloc(void *p, size_t c, size_t s)
{
	size_t n = c * s;
	GhHead *h = ours(p);
	size_t old;
	void *q;

	if (c && n / c != s)
		return NULL;
	if (!p)
		return gh_calloc(c, s);
	if (!h)
		return r_recalloc(p, c, s);
	old = h->size;
	q = gh_realloc(p, n);
	if (q && n > old)
		memset((char *)q + old, 0, n - old);
	return q;
}

static void *gh_expand(void *p, size_t n)
{
	GhHead *h = ours(p);
	void *raw;

	if (!h)
		return r_expand(p, n);
	if (n >= GH_BIG)
		return NULL;
	raw = HeapReAlloc(g_heap, HEAP_REALLOC_IN_PLACE_ONLY, h, n + sizeof(GhHead));
	if (!raw)
		return NULL;
	h->size = n;
	return p;
}

struct Slot {
	const char *name;
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
 * _strdup allocates through the runtime's own malloc, which is not this one - it
 * is an internal call, not an import - so its blocks stay on the process heap
 * and come back to us through free, where the dispatch sends them home. */
static struct Slot g_slots[] = {
	{ "malloc", (void **)&r_malloc, (void *)(uintptr_t)0 },
	{ "calloc", (void **)&r_calloc, (void *)(uintptr_t)0 },
	{ "realloc", (void **)&r_realloc, (void *)(uintptr_t)0 },
	{ "free", (void **)&r_free, (void *)(uintptr_t)0 },
	{ "_msize", (void **)&r_msize, (void *)(uintptr_t)0 },
	{ "_recalloc", (void **)&r_recalloc, (void *)(uintptr_t)0 },
	{ "_expand", (void **)&r_expand, (void *)(uintptr_t)0 }
};

static const char *const kCrt[] = { "ucrtbase.dll", "msvcrt.dll", "msvcr100.dll",
				    "msvcr120.dll" };

static int on(void)
{
	char v[8];
	DWORD n = savestate_getenv("D3D9SW_GAMEHEAP", v, sizeof(v));

	return n > 0 && n < sizeof(v) && v[0] == '1';
}

int gameheap_install(void)
{
	HMODULE exe = GetModuleHandleA(NULL);
	HMODULE crt = NULL;
	size_t i;
	int patched = 0, missing = 0;
	const char *crtname = NULL;

	g_slots[0].ours = (void *)gh_malloc;
	g_slots[1].ours = (void *)gh_calloc;
	g_slots[2].ours = (void *)gh_realloc;
	g_slots[3].ours = (void *)gh_free;
	g_slots[4].ours = (void *)gh_msize;
	g_slots[5].ours = (void *)gh_recalloc;
	g_slots[6].ours = (void *)gh_expand;

	if (g_ready || !on())
		return 0;
	for (i = 0; i < sizeof(kCrt) / sizeof(kCrt[0]) && !crt; i++) {
		crt = GetModuleHandleA(kCrt[i]);
		crtname = kCrt[i];
	}
	if (!exe || !crt) {
		ss_log("gameheap: no C runtime module is loaded, so there is nothing to "
		       "redirect\n");
		return 0;
	}
	for (i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); i++) {
		*g_slots[i].real = (void *)GetProcAddress(crt, g_slots[i].name);
		if (!*g_slots[i].real)
			missing++;
	}
	/* Without all seven the dispatch has a hole in it, and a hole here is a
	 * block freed to the wrong allocator. */
	if (missing) {
		ss_log("gameheap: %s is missing %d of the %d entry point(s) this needs, "
		       "so nothing was redirected\n",
		       crtname, missing, (int)(sizeof(g_slots) / sizeof(g_slots[0])));
		return 0;
	}
	g_heap = HeapCreate(0, 1u << 20, 0);
	if (!g_heap) {
		ss_log("gameheap: HeapCreate failed, error %lu\n", GetLastError());
		return 0;
	}
	InitializeCriticalSection(&g_cs);
	/* Live before the first redirected call, because the first one can arrive
	 * on another thread while this one is still patching. */
	g_ready = 1;
	for (i = 0; i < sizeof(g_slots) / sizeof(g_slots[0]); i++) {
		int n = savestate_patch_iat(exe, *g_slots[i].real, g_slots[i].ours);

		patched += n;
		ss_log("gameheap: %-10s %d slot(s)\n", g_slots[i].name, n);
	}
	if (!patched) {
		/* The executable reaches the runtime some other way - a delay load,
		 * or a packer that resolves by hand. Leaving the heap in place with
		 * nothing pointing at it is harmless; claiming we moved the game's
		 * memory when we did not is not. */
		g_ready = 0;
		ss_log("gameheap: the executable's import table holds none of %s's "
		       "allocator addresses, so the redirect found no site. The game's "
		       "memory is where it was\n",
		       crtname);
		return 0;
	}
	savestate_game_heap(g_heap);
	ss_log("gameheap: %d import slot(s) redirected from %s into a private heap at "
	       "%p. Allocations under %lu KB the executable makes from here on have no "
	       "other tenant, which is the arrangement DDPR gets from MSVCR100 for "
	       "free\n",
	       patched, crtname, (void *)g_heap, (unsigned long)(GH_BIG >> 10));
	return patched;
}

void gameheap_report(void)
{
	if (!g_ready) {
		if (on())
			ss_log("gameheap: asked for, but not installed\n");
		return;
	}
	ss_log("gameheap: %lu allocation(s) served, %lu freed to us, %lu passed back "
	       "to the runtime, %lu too big to take, %lu region(s) mapped\n",
	       g_alloc, g_freed_ours, g_freed_theirs, g_toobig, (unsigned long)g_nreg);
	if (g_fellback || g_regfull || g_stale)
		ss_log("gameheap: %lu allocation(s) fell back to the runtime, %lu region(s) "
		       "past the table, %lu pointer(s) inside our range with no header - "
		       "any of the three is a hole worth reading about before trusting "
		       "this\n",
		       g_fellback, g_regfull, g_stale);
}
