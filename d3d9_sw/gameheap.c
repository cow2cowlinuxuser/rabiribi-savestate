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
	g_heap = HeapCreate(0, 1u << 20, 0);
	if (!g_heap) {
		ss_log("gameheap: HeapCreate failed, error %lu\n", GetLastError());
		VirtualFree(pool, 0, MEM_RELEASE);
		return 0;
	}
	InitializeCriticalSection(&g_cs);

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
	savestate_game_heap(g_heap);
	ss_log("gameheap: %d of %d allocator site(s) in the game's own code now run "
	       "on a private heap at %p. The runtime is linked statically, so these "
	       "are the game's copies and nothing Windows uses passes through them. "
	       "Allocations under %lu KB from here on have no other tenant, which is "
	       "the arrangement DDPR gets from MSVCR100 for free\n",
	       wired, live, (void *)g_heap, (unsigned long)(GH_BIG >> 10));
	return wired;
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
