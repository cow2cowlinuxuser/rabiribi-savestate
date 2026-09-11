/* The shape of the Rabi-Ribi failure, with no game in the way.
 *
 * Two harnesses already exist and neither can reach this bug. ss_harness has
 * the oracles and the heap-identity work but no COM and no system threads;
 * ds_harness has the DirectSound half but nine threads and 2.8 MB, which is far
 * too small for anything to qualify for block restore. Their logs say so
 * plainly: "0 by block" on both 32-bit builds, against 15-16 in the game. The
 * machinery that vetoes 2269 blocks as Windows' and leaves 11458 in unheld
 * heaps has therefore never run in a 32-bit test.
 *
 * The game's failure needs four things at the same time, and no existing
 * harness has more than two of them:
 *
 *   1. an object on the SHARED process heap, so BLKOWNER's ownership contest
 *      actually engages - a private heap is uncontested and always restored
 *   2. a PRESENT-TIME system thread holding a pointer to it across the rewind,
 *      because those threads' stacks are excluded and keep running forward
 *   3. the pointer reached through a VTABLE or CALLBACK, so a stale value is
 *      executed rather than merely read
 *   4. the object restored by the BY-BLOCK path, which only engages at scale
 *
 * This builds all four at once, dialled to the numbers measured in the game:
 * 46 threads with 12 system threads excluded (7 ntdll pool, 3 dsound, 1 dinput,
 * 1 inputhost), nine heaps, ~11400 blocks and ~92 MB going through block
 * restore, and a few MB of MEM_MAPPED that region_wanted drops on the floor.
 *
 * Every held object has an oracle, and each oracle names which of the four
 * conditions it belongs to, so a failure says what broke rather than that
 * something did. The oracles compare against fingerprints taken before the save
 * and kept in memory the snapshot does not cover - the trap this project has
 * fallen into repeatedly is putting the expected value somewhere that gets
 * wound back with the thing it is supposed to be checking.
 *
 *   rr_harness32.exe [cycles] [restores] [call-anyway]
 *
 * call-anyway=1 dereferences a callback pointer the oracle has already declared
 * bad, which is what the game does and how it dies. Left off by default, so a
 * run produces counts across many cycles instead of one corpse on the first.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

int savestate_save(int slot);
int savestate_load(int slot);
void savestate_guard(void);
void savestate_exclude(void *p, size_t bytes);
int savestate_last_was_restore(void);
int savestate_thread_set(int *fresh, int *recycled, int *gone);

/* No rasteriser here, so this is honest rather than merely convenient - the
 * same stub both existing harnesses carry. */
void swrast_pool_shutdown(void)
{
}

/* ------------------------------------------------------------------ sizing
 *
 * Chosen to land on the game's measured shape rather than to be round. The
 * game restores 11444 blocks and 91.86 MB by block, so anything much smaller
 * leaves condition 4 untested, which is the whole reason this file exists. */
#define SMALL_N 16384
#define SMALL_SZ 56	 /* one LFH bucket, the size ss_harness found the crash in */
#define BULK_N 4096
#define BULK_SZ 16384	 /* 64 MB, standing in for the game's own 62.5 MB heap */
#define VIEW_BYTES (4 * 1024 * 1024) /* the MEM_MAPPED gap, ~4.7 MB in the game */
#define POOL_ITEMS 7	 /* the game excludes exactly 7 ntdll pool threads */

/* Threads of our own, which the engine rewinds. The first version had none:
 * every worker was a thread-pool thread, the engine excludes those by design,
 * and the log read "1 context(s) restored" against the game's 34. A rewound
 * stack full of pointers to objects that may or may not have come back is a
 * seam the harness could not express at all. */
#define WORKERS 30

/* The game has 700 tracked DirectSound buffers. The first version had none -
 * it created its buffer with DirectSoundCreate8 before the hook armed, so
 * CreateSoundBuffer was never intercepted and the engine's entire audio path
 * went untested while appearing to be under test. */
#define BUFS 64

#define NODE_MAGIC 0x4E485252u /* 'RRHN' */

struct Node;
typedef void(*NodeFn)(struct Node *);

/* Deliberately 56 bytes, so every one of these goes through the low
 * fragmentation front end - the allocator path that has produced every heap
 * fastfail this project has seen. */
typedef struct Node {
	unsigned magic;
	unsigned serial;
	struct Node *self; /* proves identity, not just contents, across a restore */
	NodeFn fn;	   /* condition 3: what a present-time thread calls through */
	unsigned payload[10];
} Node;

/* ------------------------------------------------------- held bookkeeping
 *
 * Excluded from the snapshot, because a counter that rewinds cannot count
 * restores and an expected value that rewinds cannot be an expected value.
 * Three separate bugs in this project came from getting that wrong. */
typedef struct {
	/* COM objects, held in the present on purpose: dsound is not rewound, so
	 * winding our pointer to it backwards would be a different bug. */
	void *dev, *buf;

	HANDLE map;
	unsigned char *view;
	HANDLE bulk_heap;

	volatile LONG stop;
	volatile LONG pool_live;
	int call_anyway;

	int cycles, restores_done;

	/* condition 1+4: the nodes themselves */
	int node_checked, node_bad_magic, node_bad_self, node_bad_fn;
	unsigned node_first_bad_serial;
	void *node_first_bad_at;

	/* condition 2+3: what the present-time threads saw */
	long pool_calls, pool_stale, pool_skipped, pool_called_anyway;
	void *pool_first_stale_at;

	/* the MEM_MAPPED gap */
	unsigned long long view_hash_at_save;
	int view_hash_valid, view_bad;

	/* our own rewound threads, the window pump and the file reader */
	long worker_calls, worker_bad, pump_ticks, file_reads;

	/* the DirectSound cursor split, inherited from ds_harness */
	int nbuf;
	void *bufs[BUFS];
	int com_ticks, com_lost;
	long com_worst_gap;
	int check_gap;

	/* heap identity, inherited from ss_harness scenario 3 */
	int heap_bad;

	/* thread-set invariant (invariant 4 / restore_invariants.md) */
	int tset_checks, tset_fresh, tset_gone, tset_recycled;
	int tset_worst_gone; /* high watermark of gone threads in a single restore */

	/* fingerprints of the immutable half of every node, taken before the save.
	 * Only magic/serial/self/fn are covered: the payload is written by worker
	 * threads continuously, so including it would report tearing rather than
	 * restore failure, and the engine's own verify already covers bytes. */
	unsigned long long fp[SMALL_N];
	int fp_valid;
} Held;

static Held *g_held;

/* Ordinary globals, so the rewind takes them back. That is the point: these are
 * the game's side of every split under test. */
static Node **g_nodes;	   /* on the process heap, shared with Windows */
static void **g_bulk;	   /* on a private heap, standing in for the game's own */
static DWORD g_write;	   /* the ring write cursor the hardware cursor outruns */
static double g_phase;
static unsigned g_serial_next;

/* --------------------------------------------------------------- node work */

static void node_touch_a(Node *n) { n->payload[0]++; }
static void node_touch_b(Node *n) { n->payload[1] += 2; }
static void node_touch_c(Node *n) { n->payload[2] ^= 0x5A5A5A5Au; }

static NodeFn const g_fns[3] = { node_touch_a, node_touch_b, node_touch_c };

static int fn_known(NodeFn f)
{
	int i;

	for (i = 0; i < 3; i++)
		if (f == g_fns[i])
			return 1;
	return 0;
}

static unsigned long long node_fp(const Node *n)
{
	unsigned long long h = 1469598103934665603ULL;

	h = (h ^ n->magic) * 1099511628211ULL;
	h = (h ^ n->serial) * 1099511628211ULL;
	h = (h ^ (unsigned long long)(uintptr_t)n->self) * 1099511628211ULL;
	h = (h ^ (unsigned long long)(uintptr_t)n->fn) * 1099511628211ULL;
	return h;
}

static void node_init(Node *n, unsigned serial)
{
	n->magic = NODE_MAGIC;
	n->serial = serial;
	n->self = n;
	n->fn = g_fns[serial % 3];
	memset(n->payload, 0, sizeof(n->payload));
}

/* ------------------------------------------------- condition 2: pool threads
 *
 * A work item submitted to the Windows thread pool runs on an ntdll worker,
 * which is one of the twelve threads the engine excludes by name. It picks a
 * node, holds the pointer in a local - on a stack that is NOT rewound - sleeps
 * long enough for a save and a restore to happen underneath it, and only then
 * dereferences and calls through the function pointer.
 *
 * That delay is the entire experiment. It is the difference between a thread
 * that reads a rewound object and a thread that was already holding a pointer
 * to it when the world moved, and the game's faults are all the second kind. */
static DWORD WINAPI pool_item(LPVOID arg)
{
	(void)arg;
	InterlockedIncrement(&g_held->pool_live);
	while (!g_held->stop) {
		Node *n;
		Node **tab = g_nodes;
		int bad = 0;

		if (!tab) {
			Sleep(4);
			continue;
		}
		n = tab[(unsigned)(GetTickCount() * 2654435761u) % SMALL_N];
		if (!n) {
			Sleep(4);
			continue;
		}
		/* Held across the window a save and a restore fit inside. */
		Sleep(40);

		InterlockedIncrement(&g_held->pool_calls);
		if (IsBadReadPtr(n, sizeof(*n)))
			bad = 1;
		else if (n->magic != NODE_MAGIC || n->self != n || !fn_known(n->fn))
			bad = 1;
		if (bad) {
			InterlockedIncrement(&g_held->pool_stale);
			if (!g_held->pool_first_stale_at)
				g_held->pool_first_stale_at = n;
			if (!g_held->call_anyway) {
				InterlockedIncrement(&g_held->pool_skipped);
				continue;
			}
			/* What the game does. The engine's freeze-on-fault will stop
			 * the process here with every register intact. */
			InterlockedIncrement(&g_held->pool_called_anyway);
		}
		n->fn(n);
	}
	InterlockedDecrement(&g_held->pool_live);
	return 0;
}

/* ------------------------------------------------------- condition 3: COM
 *
 * Reduced from ds_harness, which established that the arrangement matters: a
 * play cursor owned by hardware that cannot be suspended, against a write
 * cursor that is an ordinary global and gets wound back. Kept here because the
 * three dsound worker threads are a third of the excluded twelve, and they are
 * present whether or not this harness calls into them. */
#define RATE 44100
#define CHANS 2
#define BYTES_PER_FRAME (CHANS * 2)
#define RING_BYTES (RATE * BYTES_PER_FRAME)

#define DSSCL_PRIORITY 2
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBCAPS_GLOBALFOCUS 0x00008000
#define DSBPLAY_LOOPING 0x00000001

typedef struct {
	DWORD dwSize, dwFlags, dwBufferBytes, dwReserved;
	WAVEFORMATEX *lpwfxFormat;
	GUID guid3DAlgorithm;
} DSBUFDESC;

typedef HRESULT(WINAPI *PFN_CREATE8)(const GUID *, void **, void *);

struct DSVtbl {
	HRESULT(WINAPI *QueryInterface)(void *, const GUID *, void **);
	ULONG(WINAPI *AddRef)(void *);
	ULONG(WINAPI *Release)(void *);
	HRESULT(WINAPI *CreateSoundBuffer)(void *, const DSBUFDESC *, void **, void *);
	HRESULT(WINAPI *GetCaps)(void *, void *);
	HRESULT(WINAPI *DuplicateSoundBuffer)(void *, void *, void **);
	HRESULT(WINAPI *SetCooperativeLevel)(void *, HWND, DWORD);
};

struct BufVtbl {
	HRESULT(WINAPI *QueryInterface)(void *, const GUID *, void **);
	ULONG(WINAPI *AddRef)(void *);
	ULONG(WINAPI *Release)(void *);
	HRESULT(WINAPI *GetCaps)(void *, void *);
	HRESULT(WINAPI *GetCurrentPosition)(void *, DWORD *, DWORD *);
	HRESULT(WINAPI *GetFormat)(void *, void *, DWORD, DWORD *);
	HRESULT(WINAPI *GetVolume)(void *, LONG *);
	HRESULT(WINAPI *GetPan)(void *, LONG *);
	HRESULT(WINAPI *GetFrequency)(void *, DWORD *);
	HRESULT(WINAPI *GetStatus)(void *, DWORD *);
	HRESULT(WINAPI *Initialize)(void *, void *, const DSBUFDESC *);
	HRESULT(WINAPI *Lock)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD);
	HRESULT(WINAPI *Play)(void *, DWORD, DWORD, DWORD);
	HRESULT(WINAPI *SetCurrentPosition)(void *, DWORD);
};

static void fill(unsigned char *dst, DWORD bytes)
{
	DWORD i;

	for (i = 0; i + BYTES_PER_FRAME <= bytes; i += BYTES_PER_FRAME) {
		short s = (short)(8000.0 * sin(g_phase));

		g_phase += 2.0 * 3.14159265358979 * 440.0 / RATE;
		if (g_phase > 6.283185307179586)
			g_phase -= 6.283185307179586;
		dst[i + 0] = (unsigned char)(s & 0xFF);
		dst[i + 1] = (unsigned char)((s >> 8) & 0xFF);
		dst[i + 2] = dst[i + 0];
		dst[i + 3] = dst[i + 1];
	}
}

static void com_step(void)
{
	struct BufVtbl *v;
	DWORD play = 0, safe = 0, want;
	long raw;
	void *p1 = NULL, *p2 = NULL;
	DWORD n1 = 0, n2 = 0;

	if (!g_held->buf)
		return;
	v = *(struct BufVtbl **)g_held->buf;
	if (FAILED(v->GetCurrentPosition(g_held->buf, &play, &safe)))
		return;
	g_held->com_ticks++;

	raw = (long)play - (long)g_write;
	if (raw < 0)
		raw += RING_BYTES;
	/* A gap approaching the whole ring means the cursor lapped while we were
	 * stopped, so the distance carries no information any more. */
	if (g_held->check_gap && raw > (long)(RING_BYTES / 2)) {
		g_held->com_lost++;
		if (-raw < g_held->com_worst_gap)
			g_held->com_worst_gap = -raw;
	}
	g_held->check_gap = 0;

	want = (DWORD)raw;
	if (want > RING_BYTES / 4)
		want = RING_BYTES / 4;
	if (!want)
		return;
	if (FAILED(v->Lock(g_held->buf, g_write, want, &p1, &n1, &p2, &n2, 0)))
		return;
	fill((unsigned char *)p1, n1);
	if (p2)
		fill((unsigned char *)p2, n2);
	((HRESULT(WINAPI *)(void *, void *, DWORD, void *, DWORD))(((void **)v)[19]))(
		g_held->buf, p1, n1, p2, n2); /* Unlock */
	g_write = (g_write + n1 + n2) % RING_BYTES;
}

/* ------------------------------------------------------------- the oracles */

static unsigned long long view_hash(void)
{
	const unsigned long long *q = (const unsigned long long *)g_held->view;
	unsigned long long h = 1469598103934665603ULL;
	unsigned i;

	if (!q)
		return 0;
	for (i = 0; i < VIEW_BYTES / sizeof(*q); i++)
		h = (h ^ q[i]) * 1099511628211ULL;
	return h;
}

static void fingerprint_nodes(void)
{
	int i;

	if (!g_nodes)
		return;
	for (i = 0; i < SMALL_N; i++)
		g_held->fp[i] = g_nodes[i] ? node_fp(g_nodes[i]) : 0;
	g_held->fp_valid = 1;
}

/* Condition 1 and 4: did the shared-heap objects come back as themselves?
 *
 * Contents are already covered by the engine's own verify, which reported zero
 * wrong words. This asks the question that verify cannot: whether the object at
 * this address is still the object we were promised, by its own account. */
static int oracle_nodes(void)
{
	int i, bad = 0;

	if (!g_nodes || !g_held->fp_valid)
		return 0;
	for (i = 0; i < SMALL_N; i++) {
		Node *n = g_nodes[i];

		if (!n)
			continue;
		g_held->node_checked++;
		if (IsBadReadPtr(n, sizeof(*n))) {
			bad++;
			continue;
		}
		if (n->magic != NODE_MAGIC)
			g_held->node_bad_magic++;
		else if (n->self != n)
			g_held->node_bad_self++;
		else if (!fn_known(n->fn))
			g_held->node_bad_fn++;
		else if (node_fp(n) == g_held->fp[i])
			continue;
		else
			g_held->node_bad_self++;
		if (!g_held->node_first_bad_at) {
			g_held->node_first_bad_at = n;
			g_held->node_first_bad_serial = n->magic == NODE_MAGIC ? n->serial : 0;
		}
		bad++;
	}
	return bad;
}

/* The MEM_MAPPED gap. region_wanted accepts MEM_PRIVATE and MEM_IMAGE only, so
 * a mapped view is never captured and never restored. Expected to fail: this
 * exists to put a number on a gap we already know about, and to prove the
 * harness is sensitive enough to see it. */
static int oracle_view(void)
{
	if (!g_held->view || !g_held->view_hash_valid)
		return 0;
	if (view_hash() == g_held->view_hash_at_save)
		return 0;
	g_held->view_bad++;
	return 1;
}

/* Heap identity, the question cdb raised and ss_harness scenario 3 answered:
 * does Windows still believe these heaps exist? */
static int oracle_heaps(void)
{
	HANDLE list[256];
	HANDLE ph = GetProcessHeap();
	DWORD n, i;
	int bad = 0, seen_ph = 0, seen_bulk = 0;
	void *p;

	n = GetProcessHeaps(256, list);
	if (n > 256)
		n = 256;
	for (i = 0; i < n; i++) {
		if (list[i] == ph)
			seen_ph = 1;
		if (list[i] == g_held->bulk_heap)
			seen_bulk = 1;
	}
	if (!seen_ph) {
		printf("  ORACLE heaps: the process heap is NOT in the process heap "
		       "list - Windows has forgotten it\n");
		bad++;
	}
	if (g_held->bulk_heap && !seen_bulk) {
		printf("  ORACLE heaps: the bulk heap %p is NOT in the process heap "
		       "list - Windows has forgotten it\n",
		       (void *)g_held->bulk_heap);
		bad++;
	}
	if (seen_ph && !HeapValidate(ph, 0, NULL)) {
		printf("  ORACLE heaps: HeapValidate says the process heap is "
		       "inconsistent\n");
		bad++;
	}
	/* The operation that actually died in the game: a small allocation through
	 * the low fragmentation front end. Done last, so an earlier failure is
	 * reported rather than taking the harness down proving it. */
	if (seen_ph && !bad) {
		p = HeapAlloc(ph, 0, SMALL_SZ);
		if (!p) {
			printf("  ORACLE heaps: HeapAlloc from the process heap returned "
			       "nothing\n");
			bad++;
		} else {
			HeapFree(ph, 0, p);
		}
	}
	g_held->heap_bad += bad;
	return bad;
}

static int oracle_thread_set(void)
{
	int fresh = 0, recycled = 0, gone = 0;

	savestate_thread_set(&fresh, &recycled, &gone);
	g_held->tset_checks++;
	g_held->tset_fresh += fresh;
	g_held->tset_gone += gone;
	g_held->tset_recycled += recycled;
	if (gone > g_held->tset_worst_gone)
		g_held->tset_worst_gone = gone;
	return gone;
}

static void oracles(int cycle)
{
	int nb = oracle_nodes();
	int vb = oracle_view();
	int hb = oracle_heaps();
	int tb = oracle_thread_set();

	printf("  ORACLE cycle %d: nodes %d bad of %d checked (magic %d, identity %d, "
	       "callback %d), mapped view %s, heaps %s, threads %s\n",
	       cycle, nb, g_held->node_checked, g_held->node_bad_magic,
	       g_held->node_bad_self, g_held->node_bad_fn,
	       vb ? "DID NOT come back" : "matches", hb ? "BROKEN" : "sane",
	       tb ? "GONE threads exist" : "set matches");
	printf("  ORACLE pool: %ld call(s) from ntdll worker(s), %ld held a pointer "
	       "that no longer describes its object%s\n",
	       g_held->pool_calls, g_held->pool_stale,
	       g_held->pool_first_stale_at ? "" : " (none yet)");
}

/* ------------------------------------------------------------- world churn
 *
 * Between a save and a restore the game keeps allocating and freeing, which is
 * what lets a block be handed out again at the same address. Reproducing that
 * is the only way the by-block path's identity heuristic gets tested rather
 * than assumed.
 *
 * The churn deliberately does NOT touch the nodes the pool workers can see.
 * The first version of this freed every seventh node while seven ntdll workers
 * were holding pointers into that table, and the harness died at cycle 66 with
 * a use-after-free before a save had even been taken. That is a real bug, but
 * it is this file's bug and it happens with the savestate engine switched off,
 * so it can only mask the thing being measured. A node has to become stale
 * because the world was wound back, or the result means nothing. Same heap,
 * same bucket, same recycling pressure - different blocks. */
#define CHURN_N 8192

static void **g_churn;

static void churn(int rounds)
{
	HANDLE ph = GetProcessHeap();
	int r, i;

	if (!g_churn)
		return;
	for (r = 0; r < rounds; r++) {
		for (i = 0; i < CHURN_N; i++) {
			if (g_churn[i])
				HeapFree(ph, 0, g_churn[i]);
			g_churn[i] = HeapAlloc(ph, 0, sizeof(Node));
			if (g_churn[i])
				memset(g_churn[i], 0xA5, sizeof(Node));
		}
	}
}

/* ------------------------------------- making our blocks contested
 *
 * The measurement that made this necessary: the game has 10884 of 25165 blocks
 * contested, and the first version of this harness had 633 of 29147. The block
 * COUNT was already right; what was missing was any reason for Windows to be
 * able to reach one. BLKOWNER=3 vetoes a block when the system side can reach
 * it, so with nothing pointing in from the system side the veto never fired and
 * the mechanism at the centre of the game's failure was idle.
 *
 * These are the four ways a Windows structure ends up holding a pointer to an
 * ordinary application object, and all four are things the game does:
 * a TLS slot lives in the TEB, which is excluded; a window's user data lives in
 * the window station's heap; a timer-queue timer's context is held by ntdll's
 * timer thread; and a registered wait's context is held by the wait thread. */
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static HWND g_wnd;
static HANDLE g_timer_q, g_timer, g_wait, g_wait_ev;
static CRITICAL_SECTION g_node_cs;
static volatile LONG g_cs_ready;

static VOID CALLBACK timer_cb(PVOID ctx, BOOLEAN fired)
{
	Node *n = (Node *)ctx;

	(void)fired;
	if (n && !IsBadReadPtr(n, sizeof(*n)) && n->magic == NODE_MAGIC && fn_known(n->fn))
		n->fn(n);
}

static VOID CALLBACK wait_cb(PVOID ctx, BOOLEAN fired)
{
	timer_cb(ctx, fired);
}

/* One of our own threads, so its stack and context are rewound.
 *
 * Holds a node in a TLS slot, which is the part Windows can see, and takes a
 * critical section around the work so that some fraction of saves land while a
 * lock is held. The engine forcibly resets held critical sections at restore
 * time and nothing has ever exercised that path. */
static DWORD WINAPI worker(LPVOID arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 12345u;

	while (!g_held->stop) {
		Node *n;

		if (!g_nodes || !g_cs_ready) {
			Sleep(4);
			continue;
		}
		seed = seed * 1103515245u + 12345u;
		n = g_nodes[(seed >> 8) % SMALL_N];
		if (!n)
			continue;
		/* Published where the TEB can see it, which is what makes the block
		 * contested rather than plainly ours. */
		TlsSetValue(g_tls, n);

		EnterCriticalSection(&g_node_cs);
		if (n->magic == NODE_MAGIC && fn_known(n->fn))
			n->fn(n);
		else
			InterlockedIncrement(&g_held->worker_bad);
		LeaveCriticalSection(&g_node_cs);
		InterlockedIncrement(&g_held->worker_calls);
		Sleep(1);
	}
	return 0;
}

/* A real message loop on a real window, because the game has one and because
 * the window's user data is a system-side pointer to one of our nodes. */
static DWORD WINAPI pumper(LPVOID arg)
{
	MSG m;

	(void)arg;
	g_wnd = CreateWindowExA(0, "STATIC", "rr_harness", 0, 0, 0, 16, 16, NULL, NULL,
				NULL, NULL);
	if (g_wnd && g_nodes && g_nodes[0])
		SetWindowLongPtrA(g_wnd, GWLP_USERDATA, (LONG_PTR)g_nodes[0]);
	while (!g_held->stop) {
		while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageA(&m);
		}
		InterlockedIncrement(&g_held->pump_ticks);
		Sleep(4);
	}
	return 0;
}

/* Open file handles whose positions have to survive a rewind. The game had ten
 * open at every save and this had none of its own. */
static DWORD WINAPI reader(LPVOID arg)
{
	HANDLE f;
	char path[MAX_PATH];
	DWORD n;

	(void)arg;
	if (!GetModuleFileNameA(NULL, path, sizeof(path)))
		return 0;
	f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	while (!g_held->stop) {
		char b[512];

		if (!ReadFile(f, b, sizeof(b), &n, NULL) || !n)
			SetFilePointer(f, 0, NULL, FILE_BEGIN);
		InterlockedIncrement(&g_held->file_reads);
		Sleep(3);
	}
	CloseHandle(f);
	return 0;
}

/* What did we actually allocate?
 *
 * The engine reported the 64 MB bulk heap as one segment of 0.06 MB and
 * captured 4.8 MB of 98.9 MB writable. Either the heap is not shaped the way
 * this file assumes or the engine's segment walk cannot see it, and those need
 * telling apart before anything is changed. VirtualQuery answers from our side
 * with no theory in the way. */
static void report_shape(const char *tag)
{
	MEMORY_BASIC_INFORMATION mbi;
	void *probe[6];
	const char *name[6] = { "node table", "first node",  "last node",
				"bulk table", "first bulk",  "last bulk" };
	int i;

	probe[0] = g_nodes;
	probe[1] = g_nodes ? (void *)g_nodes[0] : NULL;
	probe[2] = g_nodes ? (void *)g_nodes[SMALL_N - 1] : NULL;
	probe[3] = g_bulk;
	probe[4] = g_bulk ? g_bulk[0] : NULL;
	probe[5] = g_bulk ? g_bulk[BULK_N - 1] : NULL;

	printf("  shape (%s):\n", tag);
	for (i = 0; i < 6; i++) {
		if (!probe[i] || VirtualQuery(probe[i], &mbi, sizeof(mbi)) != sizeof(mbi)) {
			printf("    %-11s unavailable\n", name[i]);
			continue;
		}
		printf("    %-11s at %08lX  alloc base %08lX  region %8.2f MB  "
		       "state %lX type %lX prot %lX\n",
		       name[i], (unsigned long)(uintptr_t)probe[i],
		       (unsigned long)(uintptr_t)mbi.AllocationBase,
		       (double)mbi.RegionSize / (1024.0 * 1024.0),
		       (unsigned long)mbi.State, (unsigned long)mbi.Type,
		       (unsigned long)mbi.Protect);
	}
}

/* ------------------------------------------------------------------- setup */

static int build_world(void)
{
	HANDLE ph = GetProcessHeap();
	int i;

	g_nodes = (Node **)HeapAlloc(ph, 0, SMALL_N * sizeof(Node *));
	if (!g_nodes)
		return 0;
	for (i = 0; i < SMALL_N; i++) {
		g_nodes[i] = (Node *)HeapAlloc(ph, 0, sizeof(Node));
		if (!g_nodes[i])
			return 0;
		node_init(g_nodes[i], g_serial_next++);
	}
	g_churn = (void **)HeapAlloc(ph, 0, CHURN_N * sizeof(void *));
	if (!g_churn)
		return 0;
	memset(g_churn, 0, CHURN_N * sizeof(void *));
	/* Four rounds of free-and-reallocate through one bucket, which is what it
	 * takes for the front end to engage. Copied from ss_harness because that is
	 * where it was established, not because it looks thorough. */
	for (i = 0; i < 4; i++)
		churn(1);

	g_held->bulk_heap = HeapCreate(0, 0, 0);
	if (!g_held->bulk_heap)
		return 0;
	g_bulk = (void **)HeapAlloc(ph, 0, BULK_N * sizeof(void *));
	if (!g_bulk)
		return 0;
	for (i = 0; i < BULK_N; i++) {
		g_bulk[i] = HeapAlloc(g_held->bulk_heap, 0, BULK_SZ);
		if (!g_bulk[i])
			return 0;
		memset(g_bulk[i], (unsigned char)i, BULK_SZ);
	}

	g_held->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
					VIEW_BYTES, NULL);
	if (g_held->map)
		g_held->view = (unsigned char *)MapViewOfFile(g_held->map, FILE_MAP_ALL_ACCESS,
							     0, 0, VIEW_BYTES);
	if (g_held->view)
		memset(g_held->view, 0x11, VIEW_BYTES);
	return 1;
}

static int start_audio(void)
{
	HMODULE ds = LoadLibraryA("dsound.dll");
	PFN_CREATE8 create8 =
		ds ? (PFN_CREATE8)(void *)GetProcAddress(ds, "DirectSoundCreate8") : NULL;
	struct DSVtbl *dv;
	DSBUFDESC desc;
	WAVEFORMATEX wf;
	HWND wnd;

	if (!create8)
		return 0;
	if (FAILED(create8(NULL, &g_held->dev, NULL)) || !g_held->dev)
		return 0;
	dv = *(struct DSVtbl **)g_held->dev;
	wnd = CreateWindowExA(0, "STATIC", "rr_harness", 0, 0, 0, 1, 1, NULL, NULL, NULL,
			      NULL);
	dv->SetCooperativeLevel(g_held->dev, wnd ? wnd : GetDesktopWindow(), DSSCL_PRIORITY);

	memset(&wf, 0, sizeof(wf));
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = CHANS;
	wf.nSamplesPerSec = RATE;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = BYTES_PER_FRAME;
	wf.nAvgBytesPerSec = RATE * BYTES_PER_FRAME;
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
	desc.dwBufferBytes = RING_BYTES;
	desc.lpwfxFormat = &wf;
	/* Many buffers, not one. The engine tracks each through its own
	 * CreateSoundBuffer hook, stops all of them for the copy and seeks all of
	 * them before the resume; with a single buffer none of that arithmetic is
	 * under any strain, and with a buffer created before the hook armed none of
	 * it ran at all. */
	{
		int i;

		for (i = 0; i < BUFS; i++) {
			void *b = NULL;

			if (FAILED(dv->CreateSoundBuffer(g_held->dev, &desc, &b, NULL)) || !b)
				break;
			g_held->bufs[g_held->nbuf++] = b;
		}
	}
	if (!g_held->nbuf)
		return 0;
	g_held->buf = g_held->bufs[0];
	g_write = 0;
	com_step();
	/* All of them playing, so all of them have a live cursor that does not
	 * rewind - the split under test, multiplied by the buffer count. */
	{
		int i;

		for (i = 0; i < g_held->nbuf; i++)
			((struct BufVtbl *)*(void ***)g_held->bufs[i])
				->Play(g_held->bufs[i], 0, 0, DSBPLAY_LOOPING);
	}
	return 1;
}

int main(int argc, char **argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 400;
	int restores = argc > 2 ? atoi(argv[2]) : 3;
	int call_anyway = argc > 3 ? atoi(argv[3]) : 0;
	int i, audio;

	setvbuf(stdout, NULL, _IONBF, 0);

	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!g_held) {
		printf("could not reserve the held block\n");
		return 1;
	}
	savestate_exclude(g_held, sizeof(Held));
	g_held->call_anyway = call_anyway;

	if (!build_world()) {
		printf("could not build the world - out of memory?\n");
		return 1;
	}
	report_shape("after build_world");

	/* Load dsound BEFORE the warmup, then warm up, then create anything.
	 *
	 * The hook arms from inside savestate_guard by creating a probe device and
	 * swapping two slots in dsound's shared vtable, and it gives up quietly for
	 * that frame if dsound.dll is not loaded yet. Warming up first and loading
	 * dsound inside start_audio meant every warmup call hit the not-loaded
	 * path, the hook armed only after all 64 buffers existed, and the log said
	 * "0 buffer(s) tracked" for the second time - the same mistake in a new
	 * place, and only visible because the log prints the count. */
	LoadLibraryA("dsound.dll");
	for (i = 0; i < 8; i++) {
		savestate_guard();
		Sleep(20);
	}
	audio = start_audio();

	InitializeCriticalSection(&g_node_cs);
	g_cs_ready = 1;
	g_tls = TlsAlloc();

	for (i = 0; i < POOL_ITEMS; i++)
		QueueUserWorkItem(pool_item, NULL, WT_EXECUTELONGFUNCTION);
	for (i = 0; i < WORKERS; i++) {
		HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)i, 0, NULL);

		if (t)
			CloseHandle(t);
	}
	{
		HANDLE t = CreateThread(NULL, 0, pumper, NULL, 0, NULL);

		if (t)
			CloseHandle(t);
		t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
		if (t)
			CloseHandle(t);
	}
	/* Two more system-side holders of our pointers: ntdll's timer thread and
	 * its wait thread each keep a context pointer to one of our nodes. */
	g_timer_q = CreateTimerQueue();
	if (g_timer_q && g_nodes)
		CreateTimerQueueTimer(&g_timer, g_timer_q, timer_cb, g_nodes[1], 20, 20,
				      WT_EXECUTEDEFAULT);
	g_wait_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (g_wait_ev && g_nodes)
		RegisterWaitForSingleObject(&g_wait, g_wait_ev, wait_cb, g_nodes[2], 25,
					    WT_EXECUTEDEFAULT);
	Sleep(400);

	printf("rr harness: %d cycle(s), %d restore(s), call-anyway %s\n", cycles,
	       restores, call_anyway ? "ON (will fault like the game)" : "off");
	printf("  condition 1  %d node(s) of %d bytes on the SHARED process heap\n",
	       SMALL_N, (int)sizeof(Node));
	printf("  condition 2  %ld pool worker(s) live on ntdll threads\n",
	       g_held->pool_live);
	printf("  condition 2b %d thread(s) of our own, which DO rewind, each "
	       "publishing a node into a TLS slot\n",
	       WORKERS);
	printf("  condition 3  dsound %s with %d buffer(s) created after the hook "
	       "armed, callback pointers inside every node\n",
	       audio ? "streaming" : "UNAVAILABLE (no device)", g_held->nbuf);
	printf("  contested    node pointers published into TLS, window user data, "
	       "a timer-queue context and a registered wait\n");
	printf("  condition 4  %d bulk block(s) of %d KB on a private heap (%d MB), "
	       "plus a %d MB mapped view\n\n",
	       BULK_N, BULK_SZ / 1024, (BULK_N * BULK_SZ) / (1024 * 1024),
	       VIEW_BYTES / (1024 * 1024));

	for (i = 0; i < cycles; i++) {
		savestate_guard();
		com_step();
		churn(1);
		Sleep(4);

		if (i && i % (cycles / (restores + 1)) == 0 &&
		    g_held->restores_done < restores) {
			printf("\n[%d] fingerprinting and saving\n", i);
			fingerprint_nodes();
			g_held->view_hash_at_save = view_hash();
			g_held->view_hash_valid = 1;

			if (!savestate_save(0)) {
				printf("  save failed\n");
				continue;
			}
			/* savestate_save returns twice: the second return IS the
			 * restore arriving on this thread. Written as a straight line
			 * the first time, ds_harness restored fourteen times from one
			 * save and then took a fastfail. */
			if (savestate_last_was_restore()) {
				g_held->restores_done++;
				g_held->check_gap = 1;
				printf("[%d] back from restore\n", i);
				oracles(i);
				continue;
			}
			/* Let the world move on before winding it back. Long enough for
			 * the ring to lap and for the pool workers to pick up pointers
			 * that will be stale by the time they use them. */
			printf("[%d] churning for 1.2 s, then restoring\n", i);
			churn(3);
			Sleep(1200);
			if (!savestate_load(0)) {
				printf("  restore failed\n");
				g_held->restores_done++;
			}
		}
	}

	g_held->stop = 1;
	Sleep(300);

	printf("\n================ result ================\n");
	printf("  restores               %d\n", g_held->restores_done);
	printf("  nodes checked          %d\n", g_held->node_checked);
	printf("  nodes wrong magic      %d\n", g_held->node_bad_magic);
	printf("  nodes wrong identity   %d\n", g_held->node_bad_self);
	printf("  nodes wrong callback   %d\n", g_held->node_bad_fn);
	if (g_held->node_first_bad_at)
		printf("  first bad node         %p (serial %u)\n",
		       g_held->node_first_bad_at, g_held->node_first_bad_serial);
	printf("  pool calls             %ld\n", g_held->pool_calls);
	printf("  pool stale pointers    %ld\n", g_held->pool_stale);
	if (g_held->pool_first_stale_at)
		printf("  first stale seen at    %p\n", g_held->pool_first_stale_at);
	printf("  pool skipped the call  %ld\n", g_held->pool_skipped);
	printf("  pool called anyway     %ld\n", g_held->pool_called_anyway);
	printf("  mapped view mismatches %d\n", g_held->view_bad);
	printf("  heap oracle failures   %d\n", g_held->heap_bad);
	printf("  dsound buffers / ticks %d / %d, lost %d\n", g_held->nbuf,
	       g_held->com_ticks, g_held->com_lost);
	printf("  our worker calls / bad %ld / %ld\n", g_held->worker_calls,
	       g_held->worker_bad);
	printf("  pump ticks / file reads %ld / %ld\n", g_held->pump_ticks,
	       g_held->file_reads);
	printf("  thread-set checks      %d\n", g_held->tset_checks);
	printf("  threads gone total     %d (worst %d in one restore)\n",
	       g_held->tset_gone, g_held->tset_worst_gone);
	printf("  threads fresh total    %d (recycled %d)\n",
	       g_held->tset_fresh, g_held->tset_recycled);

	if (g_held->node_bad_magic || g_held->node_bad_self || g_held->node_bad_fn)
		printf("\n  REPRODUCED. Objects on the shared process heap did not come\n"
		       "  back as themselves, which is the game's failure with no game\n"
		       "  in it. Fix it here.\n");
	else if (g_held->pool_stale)
		printf("\n  PARTIALLY REPRODUCED. Every object came back intact, but a\n"
		       "  present-time ntdll worker was left holding a pointer that no\n"
		       "  longer describes one. That is condition 2 on its own.\n");
	else if (g_held->view_bad)
		printf("\n  Only the mapped view failed, which is expected and already\n"
		       "  known: region_wanted takes MEM_PRIVATE and MEM_IMAGE only.\n"
		       "  The four conditions together did NOT break the engine.\n");
	else
		printf("\n  NOT REPRODUCED. All four conditions held at the game's scale\n"
		       "  and every oracle passed, so the engine handles this shape and\n"
		       "  the remaining difference is specific to the game.\n");
	return 0;
}
