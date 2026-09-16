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
 *
 * The above answers "is this allocator deterministic" in one shot. To ask where
 * a run stops being deterministic, and to ask it across two real sessions
 * rather than two replays, use the jig instead:
 *
 *   gh_replay.exe probe <trace> <strategy> <out.txt> [ops per checkpoint]
 *   gh_replay.exe compare <a.txt> <b.txt>
 *
 * probe is meant to be run once per process - two invocations, or two traces
 * captured from two game sessions - and compare then reports the first
 * operation at which the two layouts part company, and how much of the final
 * live set still sits at the same offset. Everything is relative to each run's
 * own base, because the base moving while the contents do not is the outcome
 * that makes a snapshot relocatable.
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
	/* The block's name, when the trace carries one: the call site as an RVA
	 * and how many times that site had already asked for that size. Traces
	 * captured before tagging existed leave both at ~0 and everything here
	 * falls back to comparing offsets. */
	unsigned site;
	unsigned ord;
} Op;

static Op *g_ops;
static long g_nops;
static long g_named;
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
		{
			unsigned site = 0xFFFFFFFFu, ord = 0xFFFFFFFFu;
			int got = sscanf(line, "%c %x %x %x %x", &op, &size, &off,
					 &site, &ord);

			if (got < 3)
				continue;
			if (g_nops >= MAX_OPS)
				break;
			g_ops[g_nops].op = op;
			g_ops[g_nops].size = size;
			g_ops[g_nops].off = off;
			g_ops[g_nops].site = got >= 5 ? site : 0xFFFFFFFFu;
			g_ops[g_nops].ord = got >= 5 ? ord : 0xFFFFFFFFu;
			g_nops++;
			if (g_ops[g_nops - 1].site != 0xFFFFFFFFu)
				g_named++;
		}
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
 * meaningful against the game's own heap, so the replay pairs them itself.
 *
 * Two ways to pair, and the better one is now usually available. If the trace
 * carries names, a free takes the block with the matching name, which is the
 * game's own pairing exactly. Without names it falls back to taking the most
 * recent live block of the same size - not the game's pairing, but it preserves
 * the property that matters, the population of live blocks and their sizes over
 * time. The fallback is what every trace captured before tagging gets. */
static void *s_live[LIVE_CAP];
static unsigned s_live_size[LIVE_CAP];
static unsigned s_live_site[LIVE_CAP];
static unsigned s_live_ord[LIVE_CAP];
static long s_nlive;
static long s_paired_by_name;

static void drop_live(long i, void **out)
{
	*out = s_live[i];
	s_live[i] = s_live[s_nlive - 1];
	s_live_size[i] = s_live_size[s_nlive - 1];
	s_live_site[i] = s_live_site[s_nlive - 1];
	s_live_ord[i] = s_live_ord[s_nlive - 1];
	s_nlive--;
}

static int take_live(unsigned size, unsigned site, unsigned ord, void **out)
{
	long i;

	if (site != 0xFFFFFFFFu)
		for (i = s_nlive - 1; i >= 0; i--)
			if (s_live_site[i] == site && s_live_ord[i] == ord) {
				drop_live(i, out);
				s_paired_by_name++;
				return 1;
			}
	for (i = s_nlive - 1; i >= 0; i--)
		if (s_live_size[i] == size) {
			drop_live(i, out);
			return 1;
		}
	return 0;
}

/* ------------------------------------------------------------------- the jig
 *
 * The question this exists to answer is not "is the allocator deterministic"
 * but "how far into a run does it stay deterministic, and what breaks it" -
 * which needs the layout sampled repeatedly rather than compared once at the
 * end. A single digest says yes or no. Checkpoints say where.
 *
 * The comparison is deliberately between FILES rather than between two replays
 * inside one process. Running both in one process cannot test the thing that
 * actually fails in the game: the second run starts in an address space the
 * first one has already disturbed, so its base is picked under conditions the
 * real second launch never sees. Two separate invocations, each writing a file,
 * reproduce the real arrangement - fresh process, fresh address space - and the
 * compare step then has nothing to do with allocation at all.
 *
 * What the offsets are measured FROM matters as much as the offsets. Everything
 * here is relative to the strategy's own base, because that is the form the
 * question takes in the game: the heap moved half a megabyte between sessions
 * while 95% of the blocks inside it did not move at all. A digest of absolute
 * addresses would report that as total failure and would be answering a
 * question nobody asked. */
typedef struct {
	unsigned off, size;
	unsigned site, ord; /* ~0 when the trace was captured before tagging */
} JigBlk;

static JigBlk g_jig[LIVE_CAP];
static FILE *g_ckpt_f;
static long g_ckpt_every;

static int jig_cmp(const void *a, const void *b)
{
	const JigBlk *x = (const JigBlk *)a, *y = (const JigBlk *)b;

	if (x->off != y->off)
		return x->off < y->off ? -1 : 1;
	return x->size < y->size ? -1 : x->size > y->size ? 1 : 0;
}

/* Sorted, because the live set is an unordered collection and two runs that
 * agree completely can still hold it in a different order - the array follows
 * allocation and free order, not address order. Hashing it unsorted would
 * report a difference that is not one. */
static long jig_take(uintptr_t base)
{
	long i, n = 0;

	for (i = 0; i < s_nlive && n < LIVE_CAP; i++) {
		g_jig[n].off = (unsigned)((uintptr_t)s_live[i] - base);
		g_jig[n].size = (unsigned)s_live_size[i];
		g_jig[n].site = s_live_site[i];
		g_jig[n].ord = s_live_ord[i];
		n++;
	}
	qsort(g_jig, (size_t)n, sizeof(g_jig[0]), jig_cmp);
	return n;
}

static unsigned jig_hash(long n)
{
	unsigned h = 2166136261u;
	long i;

	for (i = 0; i < n; i++) {
		unsigned v = g_jig[i].off ^ (g_jig[i].size * 16777619u);
		int b;

		for (b = 0; b < 4; b++) {
			h ^= (v >> (b * 8)) & 0xFF;
			h *= 16777619u;
		}
	}
	return h;
}

static void jig_checkpoint(long at, uintptr_t base)
{
	long n = jig_take(base);
	unsigned long long bytes = 0;
	long i;

	for (i = 0; i < n; i++)
		bytes += g_jig[i].size;
	fprintf(g_ckpt_f, "K %ld %ld %llu %08X\n", at, n, bytes, jig_hash(n));
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
				s_live_site[s_nlive] = o->site;
				s_live_ord[s_nlive] = o->ord;
				s_nlive++;
			}
			live_bytes += o->size;
			if (live_bytes > r->peak)
				r->peak = live_bytes;
			r->off[r->n++] = (unsigned)((uintptr_t)p - r->base);
			break;
		case 'F':
			if (take_live(o->size, o->site, o->ord, &p)) {
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
		/* After the operation, not before, so a checkpoint describes a
		 * settled state rather than one mid-change. */
		if (g_ckpt_f && g_ckpt_every > 0 && ((i + 1) % g_ckpt_every) == 0)
			jig_checkpoint(i + 1, r->base);
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

/* Replay one trace under one strategy and write the layout out, checkpoint by
 * checkpoint, plus the whole live set at the end. */
static int cmd_probe(int argc, char **argv)
{
	const char *trace = argc > 2 ? argv[2] : "gh_trace.txt";
	const char *which = argc > 3 ? argv[3] : "pinned";
	const char *out = argc > 4 ? argv[4] : "jig.txt";
	long every = argc > 5 ? atol(argv[5]) : 250;
	Strategy *st = NULL;
	Result r;
	long n, i;

	for (i = 0; i < NSTRAT; i++)
		if (strcmp(which, g_strat[i].name) == 0)
			st = &g_strat[i];
	if (!st) {
		printf("no strategy called \"%s\"\n", which);
		return 1;
	}
	if (!load_trace(trace))
		return 1;
	g_ckpt_f = fopen(out, "w");
	if (!g_ckpt_f) {
		printf("cannot write %s\n", out);
		return 1;
	}
	g_ckpt_every = every;
	fprintf(g_ckpt_f, "# jig trace=%s strategy=%s every=%ld\n", trace, which, every);
	if (!replay(st, &r)) {
		printf("  could not run\n");
		fclose(g_ckpt_f);
		st->done();
		return 1;
	}
	/* Before done(), which destroys the heap the live pointers name. */
	n = jig_take(r.base);
	fprintf(g_ckpt_f, "# base %08X served %ld refused %ld live %ld\n",
		(unsigned)r.base, r.n, r.refused, n);
	for (i = 0; i < n; i++)
		fprintf(g_ckpt_f, "L %08X %08X %08X %08X\n", g_jig[i].off,
			g_jig[i].size, g_jig[i].site, g_jig[i].ord);
	fclose(g_ckpt_f);
	g_ckpt_f = NULL;
	st->done();
	printf("wrote %s: base %08X, %ld live block(s), checkpoint every %ld op(s)\n",
	       out, (unsigned)r.base, n, every);
	printf("  %ld of %ld allocation(s) named; %ld free(s) paired by name "
	       "rather than by size\n",
	       g_named, g_nops, s_paired_by_name);
	printf("\nRun this again - a second process, not a second replay - and compare:\n"
	       "    gh_replay32.exe compare <first> <second>\n");
	return 0;
}

/* ---- comparing two probe files ---- */

#define JIG_CK 4096

typedef struct {
	long at, n;
	unsigned hash;
} Ckpt;

static Ckpt g_ck[2][JIG_CK];
static long g_nck[2];
static JigBlk g_lv[2][LIVE_CAP];
static long g_nlv[2];
static unsigned g_base[2];

static int jig_read(const char *path, int slot)
{
	char line[256];
	FILE *f = fopen(path, "r");

	if (!f) {
		printf("cannot read %s\n", path);
		return 0;
	}
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == 'K' && g_nck[slot] < JIG_CK) {
			Ckpt *c = &g_ck[slot][g_nck[slot]];
			unsigned long long bytes;

			if (sscanf(line, "K %ld %ld %llu %X", &c->at, &c->n, &bytes,
				   &c->hash) == 4)
				g_nck[slot]++;
		} else if (line[0] == 'L' && g_nlv[slot] < LIVE_CAP) {
			JigBlk *b = &g_lv[slot][g_nlv[slot]];
			int got;

			b->site = b->ord = 0xFFFFFFFFu;
			got = sscanf(line, "L %X %X %X %X", &b->off, &b->size,
				     &b->site, &b->ord);
			if (got >= 2) {
				if (got < 4)
					b->site = b->ord = 0xFFFFFFFFu;
				g_nlv[slot]++;
			}
		} else if (line[0] == '#') {
			unsigned b;

			if (sscanf(line, "# base %X", &b) == 1)
				g_base[slot] = b;
		}
	}
	fclose(f);
	return 1;
}

static int cmd_compare(int argc, char **argv)
{
	long i, j, same = 0, big_same = 0, big = 0;
	long first_bad = -1;

	if (argc < 4) {
		printf("compare needs two probe files\n");
		return 1;
	}
	memset(g_nck, 0, sizeof(g_nck));
	memset(g_nlv, 0, sizeof(g_nlv));
	if (!jig_read(argv[2], 0) || !jig_read(argv[3], 1))
		return 1;

	printf("base            %08X vs %08X%s\n", g_base[0], g_base[1],
	       g_base[0] == g_base[1] ? "   SAME" : "   moved");
	printf("checkpoints     %ld vs %ld\n", g_nck[0], g_nck[1]);

	/* Where they part company, which is the whole point of sampling rather
	 * than digesting once. A run that diverges at operation 8000 of 8300 is a
	 * different problem from one that diverges at operation 12. */
	for (i = 0; i < g_nck[0] && i < g_nck[1]; i++) {
		if (g_ck[0][i].at != g_ck[1][i].at)
			break;
		if (g_ck[0][i].hash != g_ck[1][i].hash) {
			first_bad = g_ck[0][i].at;
			printf("first divergence at operation %ld "
			       "(live %ld vs %ld)\n",
			       g_ck[0][i].at, g_ck[0][i].n, g_ck[1][i].n);
			break;
		}
	}
	if (first_bad < 0)
		printf("every checkpoint agreed - the layout is reproducible "
		       "for the whole run\n");

	/* Both lists are sorted by offset, so this is a merge rather than a
	 * search. Size is compared too: an offset that matches while the size
	 * does not is a different block that happens to start in the same
	 * place, and counting it as agreement would flatter the result. */
	for (i = 0, j = 0; i < g_nlv[0] && j < g_nlv[1];) {
		if (g_lv[0][i].off == g_lv[1][j].off) {
			if (g_lv[0][i].size == g_lv[1][j].size) {
				same++;
				if (g_lv[0][i].size >= 4096)
					big_same++;
			}
			i++;
			j++;
		} else if (g_lv[0][i].off < g_lv[1][j].off)
			i++;
		else
			j++;
	}
	for (i = 0; i < g_nlv[0]; i++)
		if (g_lv[0][i].size >= 4096)
			big++;

	printf("live at the end %ld vs %ld, %ld at the same offset and size (%.1f%%)\n",
	       g_nlv[0], g_nlv[1], same,
	       g_nlv[0] ? 100.0 * (double)same / (double)g_nlv[0] : 0.0);
	printf("of the blocks >= 4 KB: %ld of %ld agree\n", big_same, big);

	/* Everything above counts BINS: a place of a given size that something
	 * landed in. That is not the same as counting BALLS, and in the game the
	 * two disagree violently - 74% of bins matched across two sessions while
	 * under 5% of them held the same block. A run can hand out an identical
	 * set of addresses and put different blocks in every one of them, and a
	 * snapshot restored on that basis would put each block back into some
	 * other block's memory.
	 *
	 * So when the traces carry names, the figure below is the one to read.
	 * The one above is kept because it is what the earlier measurements
	 * reported, and seeing the two side by side is the point. */
	{
		long named = 0, ball = 0;

		for (i = 0; i < g_nlv[0]; i++) {
			if (g_lv[0][i].site == 0xFFFFFFFFu)
				continue;
			named++;
			for (j = 0; j < g_nlv[1]; j++)
				if (g_lv[1][j].site == g_lv[0][i].site &&
				    g_lv[1][j].ord == g_lv[0][i].ord &&
				    g_lv[1][j].size == g_lv[0][i].size) {
					if (g_lv[1][j].off == g_lv[0][i].off)
						ball++;
					break;
				}
		}
		if (!named)
			printf("\nNeither trace carries names, so only bins could be "
			       "compared. Recapture\nwith a build that writes the site "
			       "and ordinal columns to see block identity.\n");
		else
			printf("\nby NAME: %ld of %ld named block(s) came back to the "
			       "same offset (%.1f%%)\n",
			       ball, named, 100.0 * (double)ball / (double)named);
	}
	printf("\nOffsets are measured from each run's own base, so a moved base with\n"
	       "agreeing offsets is the result that matters: it means the snapshot is\n"
	       "relocatable and only the base has to be pinned.\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "gh_trace.txt";
	const char *which = argc > 2 ? argv[2] : "all";
	int i;

	if (argc > 1 && strcmp(argv[1], "probe") == 0)
		return cmd_probe(argc, argv);
	if (argc > 1 && strcmp(argv[1], "compare") == 0)
		return cmd_compare(argc, argv);

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
