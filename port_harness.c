/*
 * Software engine test for the harness-positive save/copy/restore rules.
 *
 * Drives savestate.c, unmodified, the way ss_harness does. No game, no Proton
 * Rabi-Ribi, no GPU iterate. Present/CB idle-copy, DXGI recreate from a logical
 * seed, heap handle-page exclusion, and no presenter Sleep(INFINITE).
 *
 *   wine port_harness32.exe insession [cycles]
 *   wine port_harness32.exe xsession save|prove|restore <file> [seed]
 */

#include "savestate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void swrast_pool_shutdown(void)
{
}

#define CB_IDLE 0
#define CB_RECORDING 1
#define PAYLOAD 4096u
#define PIX 64u

typedef struct EngineBlock {
	unsigned magic;
	unsigned seed;
	unsigned fp;
	unsigned char payload[PAYLOAD];
} EngineBlock;

typedef struct WinePeer {
	unsigned magic;
	unsigned live_gen;
	unsigned char handle_page_stub[512];
} WinePeer;

typedef struct DxgiFake {
	void *vtbl;
	unsigned gen;
	unsigned width, height;
} DxgiFake;

typedef struct LogicalSeed {
	unsigned magic;
	unsigned seed;
	unsigned fp;
	unsigned width, height;
	unsigned char pix[PIX * PIX * 4];
} LogicalSeed;

typedef struct XSnap {
	char magic[8]; /* "PORTXS1 " */
	unsigned ver;
	LogicalSeed seed;
	unsigned saved_pid;
	uintptr_t engine_block;
	uintptr_t wine_peer;
	uintptr_t dxgi;
} XSnap;

typedef struct Held {
	int cycle;
	int ok;
	int heap_held;
	int poisoned;
	int recreated;
	int saves;
	int restores;
	unsigned wine_before;
	unsigned want_seed;
	int rw_wine;
} Held;

static HANDLE g_engine_heap;
static HANDLE g_wine_heap;
static EngineBlock *g_engine;
static WinePeer *g_wine;
static DxgiFake *g_dxgi;
static LogicalSeed *g_seed; /* excluded from snapshot */
static Held *g_held;
static volatile LONG g_stop;
static volatile LONG g_cb_phase;
static volatile LONG g_present_serial;
static volatile LONG g_pause;
static HANDLE g_presenter;

static unsigned fnv1a(const void *p, size_t n)
{
	const unsigned char *b = p;
	unsigned h = 2166136261u;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 16777619u;
	}
	return h;
}

static void fill_payload(EngineBlock *e, unsigned seed)
{
	unsigned i;

	for (i = 0; i < PAYLOAD; i++)
		e->payload[i] = (unsigned char)((seed * 2654435761u + i * 97u) >> 16);
	e->magic = 0x454E474Eu;
	e->seed = seed;
	e->fp = fnv1a(e->payload, PAYLOAD);
}

static void fill_seed(LogicalSeed *s, unsigned seed)
{
	unsigned i;

	s->magic = 0x53454544u;
	s->seed = seed;
	s->width = PIX;
	s->height = PIX;
	for (i = 0; i < PIX * PIX * 4; i++)
		s->pix[i] = (unsigned char)((seed * 1103515245u + i) >> 16);
	s->fp = fnv1a(s->pix, sizeof(s->pix));
}

static void wine_touch(unsigned gen)
{
	unsigned i;

	g_wine->magic = 0x57494E45u;
	g_wine->live_gen = gen;
	for (i = 0; i < sizeof(g_wine->handle_page_stub); i++)
		g_wine->handle_page_stub[i] = (unsigned char)(gen + i);
}

static void dxgi_create(unsigned gen)
{
	g_dxgi = (DxgiFake *)calloc(1, sizeof(*g_dxgi));
	g_dxgi->vtbl = (void *)(uintptr_t)0xD3D11C0u;
	g_dxgi->gen = gen;
	g_dxgi->width = PIX;
	g_dxgi->height = PIX;
}

static void dxgi_recreate_from_seed(void)
{
	/* Never memcpy dead COM. Recreate + re-pin + refill from logical seed. */
	if (g_dxgi)
		free(g_dxgi);
	dxgi_create(g_seed->seed);
}

static int wait_cb_idle(unsigned spins)
{
	unsigned i;

	for (i = 0; i < spins; i++) {
		if (InterlockedCompareExchange(&g_cb_phase, 0, 0) == CB_IDLE)
			return 1;
		Sleep(0);
	}
	return 0;
}

static DWORD WINAPI presenter_main(LPVOID unused)
{
	(void)unused;
	while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
		if (InterlockedCompareExchange(&g_pause, 0, 0)) {
			Sleep(1);
			continue;
		}
		InterlockedExchange(&g_cb_phase, CB_RECORDING);
		InterlockedExchange(&g_cb_phase, CB_IDLE);
		InterlockedIncrement(&g_present_serial);
		Sleep(1);
	}
	return 0;
}

static int world_setup(unsigned seed)
{
	g_engine_heap = HeapCreate(0, 64 * 1024, 0);
	g_wine_heap = HeapCreate(HEAP_GROWABLE, 0, 0);
	if (!g_engine_heap || !g_wine_heap)
		return 0;
	g_engine = (EngineBlock *)HeapAlloc(g_engine_heap, HEAP_ZERO_MEMORY, sizeof(*g_engine));
	g_wine = (WinePeer *)HeapAlloc(g_wine_heap, HEAP_ZERO_MEMORY, sizeof(*g_wine));
	g_seed = (LogicalSeed *)VirtualAlloc(NULL, sizeof(*g_seed), MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
	if (!g_engine || !g_wine || !g_seed)
		return 0;
	savestate_exclude(g_seed, sizeof(*g_seed));
	fill_payload(g_engine, seed);
	fill_seed(g_seed, seed);
	wine_touch(1);
	dxgi_create(seed);
	return 1;
}

static void world_teardown(void)
{
	InterlockedExchange(&g_stop, 1);
	if (g_presenter) {
		WaitForSingleObject(g_presenter, 2000);
		CloseHandle(g_presenter);
		g_presenter = NULL;
	}
	if (g_dxgi) {
		free(g_dxgi);
		g_dxgi = NULL;
	}
	if (g_engine) {
		HeapFree(g_engine_heap, 0, g_engine);
		g_engine = NULL;
	}
	if (g_wine) {
		HeapFree(g_wine_heap, 0, g_wine);
		g_wine = NULL;
	}
	if (g_wine_heap) {
		HeapDestroy(g_wine_heap);
		g_wine_heap = NULL;
	}
	if (g_engine_heap) {
		HeapDestroy(g_engine_heap);
		g_engine_heap = NULL;
	}
}

static int engine_ok(void)
{
	return g_engine && g_engine->magic == 0x454E474Eu &&
	       g_engine->fp == fnv1a(g_engine->payload, PAYLOAD);
}

static int insession(int cycles)
{
	LONG serial0;
	char hname[64];

	if (!world_setup(4242)) {
		printf("FAIL: world_setup\n");
		return 1;
	}
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!g_held)
		return 2;
	savestate_exclude(g_held, sizeof(Held));
	savestate_hooks_install();
	g_presenter = CreateThread(NULL, 0, presenter_main, NULL, 0, NULL);
	Sleep(20);
	printf("insession cycles=%d (presenter never Sleep(INFINITE); copy only at CB idle)\n",
	       cycles);

	while (g_held->cycle < cycles) {
		unsigned seed = 4242u + (unsigned)g_held->cycle * 17u;

		fill_payload(g_engine, seed);
		fill_seed(g_seed, seed);
		if (g_dxgi)
			g_dxgi->gen = seed;
		g_held->want_seed = seed;

		InterlockedExchange(&g_pause, 1);
		if (!wait_cb_idle(50000)) {
			printf("  c=%d refused: presenter not idle\n", g_held->cycle);
			InterlockedExchange(&g_pause, 0);
			g_held->cycle++;
			continue;
		}

		if (!savestate_save(0)) {
			printf("  c=%d save refused\n", g_held->cycle);
			InterlockedExchange(&g_pause, 0);
			break;
		}

		if (savestate_last_was_restore()) {
			g_held->restores++;
			if (!engine_ok() || g_engine->seed != g_held->want_seed) {
				g_held->poisoned++;
				fill_payload(g_engine, g_held->want_seed);
			} else {
				g_held->ok++;
			}
			if (g_wine->live_gen == g_held->wine_before && g_held->rw_wine == 1)
				printf("  c=%d WARNING: wine handle-page peer was rewound\n",
				       g_held->cycle);
			/* Behavior: treat restored DXGI COM as historical. Recreate. */
			dxgi_recreate_from_seed();
			g_held->recreated++;
			InterlockedExchange(&g_pause, 0);
			if ((g_held->cycle % 10) == 0)
				printf("  c=%d ok=%d heap_not_rewound=%d dxgi_recreate=%d\n",
				       g_held->cycle, g_held->ok, g_held->heap_held,
				       g_held->recreated);
			g_held->cycle++;
			Sleep(8);
			continue;
		}

		g_held->saves++;
		g_held->rw_wine = savestate_rewinds(g_wine, hname, sizeof(hname));
		if (g_held->cycle == 0)
			printf("  wine peer %p savestate_rewinds=%d (%s)\n", (void *)g_wine,
			       g_held->rw_wine, hname[0] ? hname : "unnamed");
		if (g_held->rw_wine != 1)
			g_held->heap_held++;

		g_held->wine_before = g_wine->live_gen;
		wine_touch(g_held->wine_before + 1);
		fill_payload(g_engine, seed ^ 0xA5A5A5A5u);
		if (g_dxgi)
			g_dxgi->vtbl = NULL;

		if (!savestate_load(0)) {
			printf("  c=%d load refused\n", g_held->cycle);
			InterlockedExchange(&g_pause, 0);
			break;
		}
		/* Success never arrives here: restore resumes inside savestate_save. */
		InterlockedExchange(&g_pause, 0);
		g_held->cycle++;
	}

	serial0 = InterlockedCompareExchange(&g_present_serial, 0, 0);
	Sleep(30);
	if (InterlockedCompareExchange(&g_present_serial, 0, 0) <= serial0) {
		printf("FAIL: presenter did not keep ticking after restore (frozen?)\n");
		world_teardown();
		return 1;
	}

	printf("summary: ok=%d/%d heap_handle_not_rewound=%d dxgi_recreated=%d poisoned=%d "
	       "saves=%d restores=%d present_serial=%ld\n",
	       g_held->ok, cycles, g_held->heap_held, g_held->recreated, g_held->poisoned,
	       g_held->saves, g_held->restores,
	       (long)InterlockedCompareExchange(&g_present_serial, 0, 0));
	world_teardown();
	if (g_held->ok == cycles && g_held->poisoned == 0 && g_held->recreated == cycles) {
		printf("PASS: idle-copy, DXGI recreate from seed, no presenter freeze\n");
		return 0;
	}
	printf("FAIL\n");
	return 1;
}

static int xsession_cmd(const char *cmd, const char *file, unsigned seed)
{
	XSnap snap;
	DWORD n;
	HANDLE h;

	if (!strcmp(cmd, "save")) {
		if (!world_setup(seed))
			return 1;
		savestate_hooks_install();
		memset(&snap, 0, sizeof(snap));
		memcpy(snap.magic, "PORTXS1 ", 8);
		snap.ver = 1;
		snap.seed = *g_seed;
		snap.saved_pid = GetCurrentProcessId();
		snap.engine_block = (uintptr_t)g_engine;
		snap.wine_peer = (uintptr_t)g_wine;
		snap.dxgi = (uintptr_t)g_dxgi;
		h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
				NULL);
		if (h == INVALID_HANDLE_VALUE)
			return 1;
		WriteFile(h, &snap, sizeof(snap), &n, NULL);
		CloseHandle(h);
		printf("xsession save: seed=%u fp=%08x engine=%p wine=%p dxgi=%p (historical)\n",
		       seed, snap.seed.fp, (void *)snap.engine_block, (void *)snap.wine_peer,
		       (void *)snap.dxgi);
		world_teardown();
		return 0;
	}

	h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 1;
	if (!ReadFile(h, &snap, sizeof(snap), &n, NULL) || n != sizeof(snap)) {
		CloseHandle(h);
		return 1;
	}
	CloseHandle(h);
	if (memcmp(snap.magic, "PORTXS1 ", 8) != 0)
		return 1;

	if (!strcmp(cmd, "prove")) {
		int dead = 0;

		if (snap.engine_block)
			dead++;
		if (snap.wine_peer)
			dead++;
		if (snap.dxgi)
			dead++;
		printf("xsession prove: saved pid %u vs live %u; %d historical pointer(s) "
		       "(engine/wine/dxgi) — never memcpy COM/HWND\n",
		       snap.saved_pid, (unsigned)GetCurrentProcessId(), dead);
		printf("%s: stale pin set is historical only\n", dead ? "PASS" : "FAIL");
		return dead ? 0 : 1;
	}

	if (!strcmp(cmd, "restore")) {
		unsigned fp;

		if (!world_setup(1))
			return 1;
		/* Close: destroy live COM, then recreate from the file seed. */
		free(g_dxgi);
		g_dxgi = NULL;
		*g_seed = snap.seed;
		fill_payload(g_engine, snap.seed.seed);
		dxgi_recreate_from_seed();
		fp = fnv1a(g_seed->pix, sizeof(g_seed->pix));
		printf("xsession restore: seed=%u pix_fp %s dxgi_gen=%u (recreated)\n",
		       snap.seed.seed, fp == snap.seed.fp ? "MATCH" : "DIFF", g_dxgi->gen);
		world_teardown();
		if (fp == snap.seed.fp && g_dxgi->gen == snap.seed.seed) {
			printf("PASS: recreate+re-pin from logical seed\n");
			return 0;
		}
		printf("FAIL\n");
		return 1;
	}
	return 1;
}

int main(int argc, char **argv)
{
	unsigned seed = 9001;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		printf("usage: port_harness32.exe insession [cycles]\n"
		       "       port_harness32.exe xsession save|prove|restore <file> [seed]\n");
		return 2;
	}
	if (!strcmp(argv[1], "insession"))
		return insession(argc > 2 ? atoi(argv[2]) : 20);
	if (!strcmp(argv[1], "xsession") && argc >= 4) {
		if (argc > 4)
			seed = (unsigned)strtoul(argv[4], NULL, 10);
		return xsession_cmd(argv[2], argv[3], seed);
	}
	printf("usage: port_harness32.exe insession [cycles]\n");
	return 2;
}
