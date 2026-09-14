/* Replays a recorded allocation trace against candidate allocators.
 *
 * gh_trace.txt is what the game's own malloc did: one line per operation, with
 * the size and where the block landed relative to the heap base. This replays
 * that sequence outside the game and asks two questions of each strategy.
 *
 *   Is it deterministic?  Same sequence, same offsets, run after run. Without
 *                         this nothing else matters, because a snapshot taken
 *                         in one session is meaningless in the next.
 *   Is it derivable?      Can an object's address be computed from an anchor
 *                         and a rule, rather than only reproduced by replaying
 *                         every allocation that came before it?
 *
 * Those are different properties and the distinction is the point of this
 * program. A general-purpose heap can be perfectly deterministic and still not
 * derivable: placement depends on the whole history, so one extra allocation
 * early shifts everything after it, and any change to the game's behaviour -
 * a different room, a different frame count before the save - moves the object
 * we were looking for. Size-class buckets are derivable: an address is the
 * class base plus an index times a stride, which survives a changed history.
 *
 * The evidence that started this: the player entity was found at heap base +
 * 0x7F4B8 in two sessions whose heap bases differed. That is one pointer. This
 * checks the same claim across every allocation the game makes.
 *
 * Build: 32-bit, to match the process the trace came from. Allocator behaviour
 * is width-dependent - header sizes, alignment, and the low-fragmentation heap's
 * bucket boundaries all differ - so a 64-bit replay would answer a question
 * nobody asked.
 *
 *   gh_replay.exe gh_trace.txt [strategy]
 *
 *     heap     HeapCreate, exactly what gameheap.c does today
 *     pinned   RtlCreateHeap inside a reservation we choose
 *     bucket   size classes, each from its own fixed-base reservation
 *     all      every strategy in turn, with a comparison at the end
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPS (4 * 1000 * 1000)
#define LIVE_CAP (1 << 20)

typedef struct {
	char op;
	unsigned size;
	unsigned off; /* what the game got, for comparison */
} Op;

static Op *g_ops;
static long g_nops;
static unsigned g_trace_base;

/* ------------------------------------------------------------------ the trace */

static int load_trace(const char *path)
{
	FILE *f = fopen(path, "rb");
	char line[256];

	if (!f) {
		printf("cannot open %s\n", path);
		return 0;
	}
	g_ops = (Op *)VirtualAlloc(NULL, (SIZE_T)MAX_OPS * sizeof(Op),
				   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_ops) {
		printf("no memory for the trace\n");
		fclose(f);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		unsigned size, off;
		char op;

		if (line[0] == '#') {
			sscanf(line, "# heap %x", &g_trace_base);
			continue;
		}
		if (sscanf(line, "%c %x %x", &op, &size, &off) != 3)
			continue;
		if (g_nops >= MAX_OPS)
			break;
		g_ops[g_nops].op = op;
		g_ops[g_nops].size = size;
		g_ops[g_nops].off = off;
		g_nops++;
	}
	fclose(f);
	printf("trace: %ld operation(s), recorded against heap base %08X\n", g_nops,
	       g_trace_base);
	return g_nops > 0;
}

/* ---------------------------------------------------------------- strategies */

/* Every strategy answers the same two calls, so the replay loop does not know
 * which one it is driving. */
typedef struct {
	const char *name;
	int (*init)(void);
	void *(*alloc)(size_t n);
	void (*release)(void *p);
	uintptr_t (*base)(void);
	void (*done)(void);
} Strategy;

/* --- stock HeapCreate, what gameheap.c does today --- */

static HANDLE s_heap;

static int heap_init(void)
{
	s_heap = HeapCreate(0, 1u << 20, 0);
	return s_heap != NULL;
}
static void *heap_alloc(size_t n) { return HeapAlloc(s_heap, 0, n); }
static void heap_release(void *p)
{
	if (p)
		HeapFree(s_heap, 0, p);
}
static uintptr_t heap_base(void) { return (uintptr_t)s_heap; }
static void heap_done(void)
{
	if (s_heap)
		HeapDestroy(s_heap);
	s_heap = NULL;
}

/* --- the same heap with the low-fragmentation front end switched off ---
 *
 * Here to explain a result rather than to be a candidate. The stock heap above
 * produced a different offset digest on two consecutive runs of the same trace,
 * which contradicts what the game shows: the player entity has been found at the
 * same offset from the heap base in separate sessions. Both cannot be simply
 * true, and the difference decides whether a snapshot is relocatable.
 *
 * The LFH randomises where a block lands within a size bucket, on purpose, as a
 * heap-spray mitigation, and it engages once a bucket has seen enough traffic.
 * If disabling it makes the digest stable, then determinism is not a property of
 * Windows heaps at all - it is a property of heaps the LFH has not woken up in,
 * which would mean the game's two matching offsets were a block allocated before
 * the front end engaged, and not a general guarantee to build on.
 */
static HANDLE s_nolfh;

static int nolfh_init(void)
{
	ULONG off = 0;

	s_nolfh = HeapCreate(0, 1u << 20, 0);
	if (!s_nolfh)
		return 0;
	if (!HeapSetInformation(s_nolfh, HeapCompatibilityInformation, &off, sizeof(off)))
		printf("  could not switch the LFH off, error %lu - this run proves "
		       "nothing\n",
		       GetLastError());
	return 1;
}
static void *nolfh_alloc(size_t n) { return HeapAlloc(s_nolfh, 0, n); }
static void nolfh_release(void *p)
{
	if (p)
		HeapFree(s_nolfh, 0, p);
}
static uintptr_t nolfh_base(void) { return (uintptr_t)s_nolfh; }
static void nolfh_done(void)
{
	if (s_nolfh)
		HeapDestroy(s_nolfh);
	s_nolfh = NULL;
}

/* --- RtlCreateHeap inside a reservation we choose ---
 *
 * The only documented way to say where a heap goes. HeapCreate has no address
 * parameter, which is the entire reason the game's entity table lands somewhere
 * different every launch while its internal offsets stay identical.
 *
 * The reservation matters as much as the base. A growable heap that outgrows it
 * asks the kernel for a second segment wherever there is room, and everything in
 * that segment is back to being unpredictable - so this reserves generously and
 * reports if anything lands outside.
 */
#define PIN_BASE 0x50000000u
#define PIN_SIZE (32u * 1024u * 1024u)

typedef PVOID(NTAPI *PFN_RtlCreateHeap)(ULONG flags, PVOID base, SIZE_T reserve,
					SIZE_T commit, PVOID lock, PVOID parms);
typedef PVOID(NTAPI *PFN_RtlDestroyHeap)(PVOID heap);

static PFN_RtlCreateHeap p_RtlCreateHeap;
static PFN_RtlDestroyHeap p_RtlDestroyHeap;
static PVOID s_pinned;
static void *s_pin_res;
static long s_pin_outside;

static int pinned_init(void)
{
	HMODULE nt = GetModuleHandleA("ntdll.dll");

	p_RtlCreateHeap = (PFN_RtlCreateHeap)GetProcAddress(nt, "RtlCreateHeap");
	p_RtlDestroyHeap = (PFN_RtlDestroyHeap)GetProcAddress(nt, "RtlDestroyHeap");
	if (!p_RtlCreateHeap) {
		printf("  RtlCreateHeap not exported - cannot test this strategy\n");
		return 0;
	}
	s_pin_res = VirtualAlloc((LPVOID)PIN_BASE, PIN_SIZE, MEM_RESERVE, PAGE_READWRITE);
	if (!s_pin_res) {
		printf("  cannot reserve %u MB at %08X, error %lu\n", PIN_SIZE >> 20,
		       PIN_BASE, GetLastError());
		return 0;
	}
	if ((uintptr_t)s_pin_res != PIN_BASE) {
		printf("  reservation landed at %p, not %08X\n", s_pin_res, PIN_BASE);
		return 0;
	}
	/* HEAP_GROWABLE is deliberately not passed with a caller-supplied base:
	 * the point is to find out whether the sequence fits, not to let it
	 * escape quietly. */
	s_pinned = p_RtlCreateHeap(0, s_pin_res, PIN_SIZE, 1u << 20, NULL, NULL);
	if (!s_pinned) {
		printf("  RtlCreateHeap refused a caller-supplied base\n");
		return 0;
	}
	if ((uintptr_t)s_pinned != PIN_BASE)
		printf("  NOTE: heap handle %p differs from the base we asked for %08X\n",
		       s_pinned, PIN_BASE);
	s_pin_outside = 0;
	return 1;
}
static void *pinned_alloc(size_t n)
{
	void *p = HeapAlloc(s_pinned, 0, n);

	if (p && ((uintptr_t)p < PIN_BASE || (uintptr_t)p >= PIN_BASE + PIN_SIZE))
		s_pin_outside++;
	return p;
}
static void pinned_release(void *p)
{
	if (p)
		HeapFree(s_pinned, 0, p);
}
static uintptr_t pinned_base(void) { return (uintptr_t)s_pinned; }
static void pinned_done(void)
{
	if (s_pinned && p_RtlDestroyHeap)
		p_RtlDestroyHeap(s_pinned);
	if (s_pin_res)
		VirtualFree(s_pin_res, 0, MEM_RELEASE);
	s_pinned = NULL;
	s_pin_res = NULL;
}

/* --- size-class buckets from fixed-base reservations ---
 *
 * The strategy that is derivable rather than merely deterministic. A block's
 * address is its class base plus its slot index times the class stride, so an
 * address can be computed without replaying history. Freed slots go on a
 * per-class free list and are reused in LIFO order, which keeps the mapping
 * stable under the churn the trace contains.
 *
 * The cost is internal fragmentation - every request rounds up to its class -
 * and a fixed ceiling per class. Both are measured below rather than argued
 * about, because whether they are acceptable is a property of this game's
 * allocation mix and not of the idea.
 */
#define BUCKET_BASE 0x58000000u
#define NCLASS 10
static const unsigned g_class[NCLASS] = { 16,	32,   64,   128,  256,
					  512,	1024, 2048, 4096, 8192 };
/* A fixed number of slots per class does not survive a 2 GB address space: at
 * 65536 slots the 4 KB class alone wants 256 MB and the ten together want about
 * a gigabyte, which is how the first run of this failed - ERROR_INVALID_ADDRESS
 * on the 4 KB class at 67F00000. Reserving a fixed number of BYTES per class
 * instead keeps the total bounded and puts the slot count where it belongs,
 * which is higher for the small classes that actually see the traffic. */
#define CLASS_BYTES (8u * 1024u * 1024u)
#define CLASS_SLOTS_MAX 262144u
static unsigned g_slots[NCLASS];

static char *s_bucket[NCLASS];
static unsigned s_used[NCLASS];
static unsigned *s_free[NCLASS];
static unsigned s_nfree[NCLASS];
static long s_bucket_over, s_bucket_full;
static unsigned long long s_waste;

static int class_of(size_t n)
{
	int i;

	for (i = 0; i < NCLASS; i++)
		if (n <= g_class[i])
			return i;
	return -1;
}

static int bucket_init(void)
{
	uintptr_t at = BUCKET_BASE;
	int i;

	for (i = 0; i < NCLASS; i++) {
		SIZE_T span;

		g_slots[i] = CLASS_BYTES / g_class[i];
		if (g_slots[i] > CLASS_SLOTS_MAX)
			g_slots[i] = CLASS_SLOTS_MAX;
		span = (SIZE_T)g_class[i] * g_slots[i];

		s_bucket[i] = (char *)VirtualAlloc((LPVOID)at, span, MEM_RESERVE,
						   PAGE_READWRITE);
		if (!s_bucket[i]) {
			printf("  cannot reserve %u KB at %08X for the %u-byte class, "
			       "error %lu\n",
			       (unsigned)(span >> 10), (unsigned)at, g_class[i],
			       GetLastError());
			return 0;
		}
		s_free[i] = (unsigned *)VirtualAlloc(NULL, g_slots[i] * sizeof(unsigned),
						     MEM_COMMIT | MEM_RESERVE,
						     PAGE_READWRITE);
		s_used[i] = 0;
		s_nfree[i] = 0;
		at += span;
	}
	s_bucket_over = 0;
	s_bucket_full = 0;
	s_waste = 0;
	return 1;
}

static void *bucket_alloc(size_t n)
{
	int c = class_of(n);
	unsigned slot;
	char *p;

	if (c < 0) {
		/* Past the largest class. Counted, not served - a real
		 * implementation hands these to a general allocator, and how
		 * often that happens is one of the things worth knowing. */
		s_bucket_over++;
		return NULL;
	}
	s_waste += g_class[c] - n;
	if (s_nfree[c])
		slot = s_free[c][--s_nfree[c]];
	else if (s_used[c] < g_slots[c])
		slot = s_used[c]++;
	else {
		s_bucket_full++;
		return NULL;
	}
	p = s_bucket[c] + (SIZE_T)slot * g_class[c];
	/* Reserved up front, committed on demand, so an untouched class costs
	 * address space and not memory. */
	if (!VirtualAlloc(p, g_class[c], MEM_COMMIT, PAGE_READWRITE))
		return NULL;
	return p;
}

static void bucket_release(void *p)
{
	uintptr_t a = (uintptr_t)p;
	int i;

	if (!p)
		return;
	for (i = 0; i < NCLASS; i++) {
		uintptr_t lo = (uintptr_t)s_bucket[i];
		uintptr_t hi = lo + (uintptr_t)g_class[i] * g_slots[i];

		if (a >= lo && a < hi) {
			unsigned slot = (unsigned)((a - lo) / g_class[i]);

			if (s_nfree[i] < g_slots[i])
				s_free[i][s_nfree[i]++] = slot;
			return;
		}
	}
}

static uintptr_t bucket_base(void) { return BUCKET_BASE; }

static void bucket_done(void)
{
	int i;

	for (i = 0; i < NCLASS; i++) {
		if (s_bucket[i])
			VirtualFree(s_bucket[i], 0, MEM_RELEASE);
		if (s_free[i])
			VirtualFree(s_free[i], 0, MEM_RELEASE);
		s_bucket[i] = NULL;
		s_free[i] = NULL;
	}
}

static Strategy g_strat[] = {
	{ "heap", heap_init, heap_alloc, heap_release, heap_base, heap_done },
	{ "nolfh", nolfh_init, nolfh_alloc, nolfh_release, nolfh_base, nolfh_done },
	{ "pinned", pinned_init, pinned_alloc, pinned_release, pinned_base, pinned_done },
	{ "bucket", bucket_init, bucket_alloc, bucket_release, bucket_base, bucket_done },
};
#define NSTRAT (int)(sizeof(g_strat) / sizeof(g_strat[0]))

/* ------------------------------------------------------------------- the run */

typedef struct {
	unsigned *off;	  /* offset from base for each served allocation */
	long n;		  /* how many were served */
	long refused;	  /* the strategy could not serve it */
	unsigned long long peak;
	uintptr_t base;
} Result;

/* The live set, so a free can be given the pointer the matching allocation
 * returned. The trace records what the game freed by offset, which is only
 * meaningful against the game's own heap, so the replay pairs them itself: a
 * free takes the most recent live block whose size matches. That is not the
 * game's exact pairing, but it preserves the property that matters - the
 * population of live blocks and their sizes over time. */
static void *s_live[LIVE_CAP];
static unsigned s_live_size[LIVE_CAP];
static long s_nlive;

static int take_live(unsigned size, void **out)
{
	long i;

	for (i = s_nlive - 1; i >= 0; i--)
		if (s_live_size[i] == size) {
			*out = s_live[i];
			s_live[i] = s_live[s_nlive - 1];
			s_live_size[i] = s_live_size[s_nlive - 1];
			s_nlive--;
			return 1;
		}
	return 0;
}

static int replay(Strategy *st, Result *r)
{
	long i;
	unsigned long long live_bytes = 0;

	memset(r, 0, sizeof(*r));
	s_nlive = 0;
	if (!st->init())
		return 0;
	r->base = st->base();
	r->off = (unsigned *)VirtualAlloc(NULL, (SIZE_T)g_nops * sizeof(unsigned),
					  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!r->off)
		return 0;
	for (i = 0; i < g_nops; i++) {
		Op *o = &g_ops[i];
		void *p;

		switch (o->op) {
		case 'A':
		case 'C':
		case 'R':
			p = st->alloc(o->size);
			if (!p) {
				r->refused++;
				r->off[r->n++] = 0xFFFFFFFFu;
				break;
			}
			if (s_nlive < LIVE_CAP) {
				s_live[s_nlive] = p;
				s_live_size[s_nlive] = o->size;
				s_nlive++;
			}
			live_bytes += o->size;
			if (live_bytes > r->peak)
				r->peak = live_bytes;
			r->off[r->n++] = (unsigned)((uintptr_t)p - r->base);
			break;
		case 'F':
			if (take_live(o->size, &p)) {
				st->release(p);
				if (live_bytes >= o->size)
					live_bytes -= o->size;
			}
			break;
		default:
			/* 'B' too big for the private heap, 'O' never ours. Neither
			 * touches the allocator under test. */
			break;
		}
	}
	return 1;
}

static void report(Strategy *st, Result *r)
{
	printf("  base               %08X\n", (unsigned)r->base);
	printf("  served             %ld\n", r->n);
	printf("  refused            %ld\n", r->refused);
	printf("  peak live          %.2f MB\n", (double)r->peak / (1024.0 * 1024.0));
	/* One number standing in for the whole offset column, so two runs can be
	 * compared by eye instead of by diffing a megabyte of addresses. The base
	 * is deliberately not in it: the question is whether the layout INSIDE the
	 * allocation is reproducible, and a moving base with a steady digest is
	 * exactly the result that says a snapshot is relocatable. */
	{
		unsigned long h = 2166136261u;
		long k;

		for (k = 0; k < r->n; k++) {
			h ^= r->off[k];
			h *= 16777619u;
		}
		printf("  offset digest      %08lX\n", h);
	}
	/* The same digest with the first allocation treated as the origin.
	 *
	 * Windows randomises where a heap's first block sits inside its segment.
	 * If that is all that differs, every offset moves by one constant and the
	 * absolute digest changes while this one does not - which would mean the
	 * layout is reproducible but the anchor is not, and a snapshot is
	 * relocatable against any known object rather than against the heap base.
	 * That is a weaker guarantee than pinning and still a usable one. */
	{
		unsigned long h = 2166136261u;
		unsigned first = r->n ? r->off[0] : 0;
		long k;

		for (k = 0; k < r->n; k++) {
			unsigned d = r->off[k] - first;

			h ^= d;
			h *= 16777619u;
		}
		printf("  relative digest    %08lX  (origin +%08X)\n", h, first);
	}
	if (strcmp(st->name, "pinned") == 0 && s_pin_outside)
		printf("  OUTSIDE the pin    %ld  <<< the reservation did not hold\n",
		       s_pin_outside);
	if (strcmp(st->name, "bucket") == 0) {
		printf("  past largest class %ld\n", s_bucket_over);
		printf("  class exhausted    %ld\n", s_bucket_full);
		printf("  rounding waste     %.2f MB\n",
		       (double)s_waste / (1024.0 * 1024.0));
	}
}

/* Does this strategy put the nth allocation at the same offset the game's heap
 * did? Only meaningful for a strategy whose placement rule is the same one the
 * trace was recorded under, so it is reported rather than judged. */
static void compare_to_trace(Result *r)
{
	long i, j = 0, same = 0, seen = 0;

	for (i = 0; i < g_nops && j < r->n; i++) {
		if (g_ops[i].op != 'A' && g_ops[i].op != 'C' && g_ops[i].op != 'R')
			continue;
		if (g_ops[i].off != 0xFFFFFFFFu && r->off[j] != 0xFFFFFFFFu) {
			seen++;
			if (g_ops[i].off == r->off[j])
				same++;
		}
		j++;
	}
	if (seen)
		printf("  matches the trace  %ld of %ld (%.1f%%)\n", same, seen,
		       100.0 * (double)same / (double)seen);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "gh_trace.txt";
	const char *which = argc > 2 ? argv[2] : "all";
	int i;

	if (!load_trace(path))
		return 1;
	for (i = 0; i < NSTRAT; i++) {
		Result r;

		if (strcmp(which, "all") != 0 && strcmp(which, g_strat[i].name) != 0)
			continue;
		printf("\n%s\n", g_strat[i].name);
		if (!replay(&g_strat[i], &r)) {
			printf("  could not run\n");
			g_strat[i].done();
			continue;
		}
		report(&g_strat[i], &r);
		compare_to_trace(&r);
		g_strat[i].done();
	}
	printf("\nRun this twice and diff the output. Identical offsets across runs is\n"
	       "determinism; it is what makes a snapshot relocatable. Whether an address\n"
	       "can be DERIVED rather than replayed is a different property, and only the\n"
	       "bucket strategy has it.\n");
	return 0;
}
