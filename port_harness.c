/*
 * Software engine test for the harness-positive save/copy/restore rules.
 *
 * Drives savestate.c, unmodified, the way ss_harness does. No game, no Proton
 * Rabi-Ribi, no GPU iterate. Present/CB idle-copy, DXGI recreate from a logical
 * seed, heap handle-page exclusion, atlas/release live_mask, depth/CB, XA2
 * voices, GDI+DXGI on one HWND (recreate, never rewind USER32).
 *
 *   wine port_harness32.exe insession [cycles]
 *   wine port_harness32.exe xsession save|prove|restore <file> [seed]
 *   wine port_harness32.exe audio
 *   wine port_harness32.exe audio-silent
 *
 * audio opens waveOut the way xa2_sw does, then one save/load. Under Wine that
 * device is mmdevapi's audio_client_main; quiesce parks our mix thread and
 * does not waveOutPause, which is the game path. audio-silent is the same
 * save/load with no device — the logical XA2 the rest of this harness already
 * passes. One shot each, not a loop.
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
#define ATLAS_SLOTS 8
#define PCM_N 256

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

typedef struct AtlasSlot {
	unsigned live;
	unsigned gen;
	unsigned fp;
	void *com;
} AtlasSlot;

typedef struct AtlasWorld {
	AtlasSlot slot[ATLAS_SLOTS];
	unsigned frame_cb;
	unsigned depth_fp;
	unsigned color2_fp;
	unsigned voice_gen;
	unsigned has_voice;
	unsigned char pcm[PCM_N];
	unsigned hwnd_id; /* USER32 stand-in; never restore the saved id */
	unsigned gdi_fp;
	unsigned dxgi_hwnd_fp;
} AtlasWorld;

typedef struct LogicalSeed {
	unsigned magic;
	unsigned seed;
	unsigned fp;
	unsigned width, height;
	unsigned char pix[PIX * PIX * 4];
	unsigned live_mask;
	unsigned gen[ATLAS_SLOTS];
	unsigned frame_cb;
	unsigned depth_fp;
	unsigned color2_fp;
	unsigned voice_gen;
	unsigned pcm_fp;
	unsigned char pcm[PCM_N];
	unsigned gdi_fp;
	unsigned dxgi_fp;
} LogicalSeed;

typedef struct XSnap {
	char magic[8]; /* "PORTXS2 " */
	unsigned ver;
	LogicalSeed seed;
	unsigned saved_pid;
	uintptr_t engine_block;
	uintptr_t wine_peer;
	uintptr_t dxgi;
	uintptr_t atlas_com[ATLAS_SLOTS];
	uintptr_t xa2_ptr;
	uintptr_t voice_ptr;
	uintptr_t hwnd;
} XSnap;

typedef struct Held {
	int cycle;
	int ok;
	int heap_held;
	int poisoned;
	int recreated;
	int atlas_ok;
	int bad_revive;
	int depth_ok;
	int xa2_ok;
	int gdi_ok;
	int hwnd_rewound;
	int saves;
	int restores;
	unsigned wine_before;
	unsigned want_seed;
	int rw_wine;
	LogicalSeed saved;
	unsigned hwnd_at_save;
} Held;

static HANDLE g_wine_heap;
static EngineBlock *g_engine;
static WinePeer *g_wine;
static DxgiFake *g_dxgi;
static AtlasWorld *g_atlas;
static LogicalSeed *g_seed; /* excluded from snapshot */
static Held *g_held;
static volatile LONG g_stop;
static volatile LONG g_cb_phase;
static volatile LONG g_present_serial;
static volatile LONG g_pause;
static HANDLE g_presenter;
static unsigned g_hwnd_serial = 1;
static void *g_xa2_ptr;
static void *g_voice_ptr;

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

static unsigned slot_fp(unsigned seed, unsigned i, unsigned gen)
{
	unsigned v[3];

	v[0] = seed;
	v[1] = i;
	v[2] = gen;
	return fnv1a(v, sizeof(v));
}

static void fill_pcm(unsigned char *pcm, unsigned seed)
{
	unsigned i;

	for (i = 0; i < PCM_N; i++)
		pcm[i] = (unsigned char)((seed * 1103515245u + i * 12345u) >> 16);
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
	s->live_mask = 0xffu;
	s->frame_cb = seed & 0xffffu;
	s->depth_fp = seed ^ 0xD3D11D32u;
	s->color2_fp = seed ^ 0xC0102u;
	s->voice_gen = 1 + (seed % 7u);
	fill_pcm(s->pcm, seed);
	s->pcm_fp = fnv1a(s->pcm, PCM_N);
	s->gdi_fp = seed ^ 0x6D11DC0u;
	s->dxgi_fp = seed ^ 0xD3D11C0u;
	for (i = 0; i < ATLAS_SLOTS; i++)
		s->gen[i] = 1u + ((seed + i * 17u) % 4u);
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

static void atlas_release_slot(unsigned i)
{
	if (g_atlas->slot[i].com) {
		free(g_atlas->slot[i].com);
		g_atlas->slot[i].com = NULL;
	}
	g_atlas->slot[i].live = 0;
}

static void atlas_create_slot(unsigned i, unsigned gen, unsigned seed)
{
	atlas_release_slot(i);
	g_atlas->slot[i].com = calloc(1, 16);
	g_atlas->slot[i].live = 1;
	g_atlas->slot[i].gen = gen;
	g_atlas->slot[i].fp = slot_fp(seed, i, gen);
}

static unsigned new_hwnd(void)
{
	/* Process-local serial plus pid so a cross-session restore cannot look
	 * like USER32 rewind just because both processes counted 1,2,3. */
	g_hwnd_serial++;
	return ((unsigned)GetCurrentProcessId() << 8) ^ g_hwnd_serial;
}

static void xa2_recreate(unsigned gen)
{
	/* Recreate engine/voice. PCM is logical. Do not memcpy COM. No mixer. */
	g_xa2_ptr = (void *)(uintptr_t)(0xA200000u + gen);
	g_voice_ptr = (void *)(uintptr_t)(0xB01CE000u + gen);
	g_atlas->voice_gen = gen;
	g_atlas->has_voice = 1;
}

static int apply_logical(const LogicalSeed *l)
{
	unsigned i;
	int bad = 0;

	for (i = 0; i < ATLAS_SLOTS; i++) {
		if (l->live_mask & (1u << i))
			atlas_create_slot(i, l->gen[i], l->seed);
		else
			atlas_release_slot(i);
	}
	g_atlas->frame_cb = l->frame_cb;
	g_atlas->depth_fp = l->depth_fp;
	g_atlas->color2_fp = l->color2_fp;
	memcpy(g_atlas->pcm, l->pcm, PCM_N);
	xa2_recreate(l->voice_gen);
	/* Recreate HWND + re-pin GDI DC + recreate DXGI backbuffer from seed.
	 * Saved hwnd_id is historical only. */
	g_atlas->hwnd_id = new_hwnd();
	g_atlas->gdi_fp = l->gdi_fp;
	g_atlas->dxgi_hwnd_fp = l->dxgi_fp;

	for (i = 0; i < ATLAS_SLOTS; i++) {
		int want = (l->live_mask & (1u << i)) != 0;
		if (want) {
			if (!g_atlas->slot[i].live || g_atlas->slot[i].gen != l->gen[i] ||
			    g_atlas->slot[i].fp != slot_fp(l->seed, i, l->gen[i]))
				bad++;
		} else if (g_atlas->slot[i].live || g_atlas->slot[i].com)
			bad++;
	}
	return bad == 0;
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
	g_wine_heap = HeapCreate(HEAP_GROWABLE, 0, 0);
	if (!g_wine_heap)
		return 0;
	/* Engine payload is private VirtualAlloc so the snapshot captures it.
	 * A HeapCreate heap is unnamed under Wine and is held (handle-page
	 * exclusion) — that is the wine peer, not the payload. */
	g_engine = (EngineBlock *)VirtualAlloc(NULL, sizeof(*g_engine), MEM_COMMIT | MEM_RESERVE,
					       PAGE_READWRITE);
	g_wine = (WinePeer *)HeapAlloc(g_wine_heap, HEAP_ZERO_MEMORY, sizeof(*g_wine));
	g_seed = (LogicalSeed *)VirtualAlloc(NULL, sizeof(*g_seed), MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
	g_atlas = (AtlasWorld *)VirtualAlloc(NULL, sizeof(*g_atlas), MEM_COMMIT | MEM_RESERVE,
					     PAGE_READWRITE);
	if (!g_engine || !g_wine || !g_seed || !g_atlas)
		return 0;
	savestate_exclude(g_seed, sizeof(*g_seed));
	fill_payload(g_engine, seed);
	fill_seed(g_seed, seed);
	wine_touch(1);
	dxgi_create(seed);
	memset(g_atlas, 0, sizeof(*g_atlas));
	apply_logical(g_seed);
	return 1;
}

static void world_teardown(void)
{
	unsigned i;

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
	if (g_atlas) {
		for (i = 0; i < ATLAS_SLOTS; i++)
			atlas_release_slot(i);
		VirtualFree(g_atlas, 0, MEM_RELEASE);
		g_atlas = NULL;
	}
	if (g_engine) {
		VirtualFree(g_engine, 0, MEM_RELEASE);
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
	if (g_seed) {
		VirtualFree(g_seed, 0, MEM_RELEASE);
		g_seed = NULL;
	}
}

static int engine_ok(void)
{
	return g_engine && g_engine->magic == 0x454E474Eu &&
	       g_engine->fp == fnv1a(g_engine->payload, PAYLOAD);
}

static int check_surfaces(const LogicalSeed *l)
{
	int atlas, depth, xa2, gdi;

	atlas = apply_logical(l);
	if (atlas)
		g_held->atlas_ok++;
	else
		g_held->bad_revive++;

	depth = (g_atlas->depth_fp == l->depth_fp && g_atlas->color2_fp == l->color2_fp &&
		 g_atlas->frame_cb == l->frame_cb);
	if (depth)
		g_held->depth_ok++;

	xa2 = (fnv1a(g_atlas->pcm, PCM_N) == l->pcm_fp && g_atlas->voice_gen == l->voice_gen &&
	       g_atlas->has_voice);
	if (xa2)
		g_held->xa2_ok++;

	gdi = (g_atlas->gdi_fp == l->gdi_fp && g_atlas->dxgi_hwnd_fp == l->dxgi_fp);
	if (gdi)
		g_held->gdi_ok++;
	if (g_atlas->hwnd_id == g_held->hwnd_at_save)
		g_held->hwnd_rewound++;

	return atlas && depth && xa2 && gdi && g_atlas->hwnd_id != g_held->hwnd_at_save;
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
	printf("  atlas/release live_mask, depth/CB/2nd RT, XA2 PCM, GDI+DXGI one HWND\n");

	while (g_held->cycle < cycles) {
		unsigned seed = 4242u + (unsigned)g_held->cycle * 17u;
		unsigned i;

		fill_payload(g_engine, seed);
		fill_seed(g_seed, seed);
		/* Saved logical has slots 1,3,5 already released. */
		g_seed->live_mask &= ~((1u << 1) | (1u << 3) | (1u << 5));
		if (g_dxgi)
			g_dxgi->gen = seed;
		apply_logical(g_seed);
		g_held->want_seed = seed;
		g_held->saved = *g_seed;
		g_held->hwnd_at_save = g_atlas->hwnd_id;

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
			/* Behavior: treat restored DXGI/atlas/XA2/HWND as historical. */
			dxgi_recreate_from_seed();
			g_held->recreated++;
			check_surfaces(&g_held->saved);
			InterlockedExchange(&g_pause, 0);
			if ((g_held->cycle % 10) == 0)
				printf("  c=%d ok=%d atlas=%d depth=%d xa2=%d gdi=%d revive=%d hwnd_rw=%d\n",
				       g_held->cycle, g_held->ok, g_held->atlas_ok, g_held->depth_ok,
				       g_held->xa2_ok, g_held->gdi_ok, g_held->bad_revive,
				       g_held->hwnd_rewound);
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
		/* Present diverges: revive candidates (new gens) + new HWND + dirty PCM. */
		for (i = 0; i < ATLAS_SLOTS; i++)
			atlas_create_slot(i, 99u, seed ^ 0xF00u);
		g_atlas->frame_cb++;
		g_atlas->depth_fp ^= 0xffffffffu;
		g_atlas->color2_fp ^= 0xffffffffu;
		fill_pcm(g_atlas->pcm, seed ^ 0xBEEFu);
		g_atlas->hwnd_id = new_hwnd();
		g_atlas->gdi_fp = 0;
		g_atlas->dxgi_hwnd_fp = 0;
		xa2_recreate(99);

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
	       "atlas=%d/%d bad_revive=%d depth=%d/%d xa2=%d/%d gdi=%d/%d hwnd_rewound=%d "
	       "saves=%d restores=%d present_serial=%ld\n",
	       g_held->ok, cycles, g_held->heap_held, g_held->recreated, g_held->poisoned,
	       g_held->atlas_ok, cycles, g_held->bad_revive, g_held->depth_ok, cycles,
	       g_held->xa2_ok, cycles, g_held->gdi_ok, cycles, g_held->hwnd_rewound,
	       g_held->saves, g_held->restores,
	       (long)InterlockedCompareExchange(&g_present_serial, 0, 0));
	world_teardown();
	if (g_held->ok == cycles && g_held->poisoned == 0 && g_held->recreated == cycles &&
	    g_held->atlas_ok == cycles && g_held->bad_revive == 0 && g_held->depth_ok == cycles &&
	    g_held->xa2_ok == cycles && g_held->gdi_ok == cycles && g_held->hwnd_rewound == 0) {
		printf("PASS: idle-copy, DXGI/atlas/depth/XA2/GDI recreate from seed, no USER32 rewind\n");
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
	unsigned i;

	if (!strcmp(cmd, "save")) {
		if (!world_setup(seed))
			return 1;
		g_seed->live_mask &= ~((1u << 1) | (1u << 3) | (1u << 5));
		apply_logical(g_seed);
		savestate_hooks_install();
		memset(&snap, 0, sizeof(snap));
		memcpy(snap.magic, "PORTXS2 ", 8);
		snap.ver = 2;
		snap.seed = *g_seed;
		snap.saved_pid = GetCurrentProcessId();
		snap.engine_block = (uintptr_t)g_engine;
		snap.wine_peer = (uintptr_t)g_wine;
		snap.dxgi = (uintptr_t)g_dxgi;
		for (i = 0; i < ATLAS_SLOTS; i++)
			snap.atlas_com[i] = (uintptr_t)g_atlas->slot[i].com;
		snap.xa2_ptr = (uintptr_t)g_xa2_ptr;
		snap.voice_ptr = (uintptr_t)g_voice_ptr;
		snap.hwnd = (uintptr_t)g_atlas->hwnd_id;
		h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
				NULL);
		if (h == INVALID_HANDLE_VALUE)
			return 1;
		WriteFile(h, &snap, sizeof(snap), &n, NULL);
		CloseHandle(h);
		printf("xsession save: seed=%u fp=%08x mask=0x%x depth=%08x pcm=%08x hwnd=%u (historical)\n",
		       seed, snap.seed.fp, snap.seed.live_mask, snap.seed.depth_fp, snap.seed.pcm_fp,
		       g_atlas->hwnd_id);
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
	if (memcmp(snap.magic, "PORTXS2 ", 8) != 0)
		return 1;

	if (!strcmp(cmd, "prove")) {
		int dead = 0;

		if (snap.engine_block)
			dead++;
		if (snap.wine_peer)
			dead++;
		if (snap.dxgi)
			dead++;
		if (snap.xa2_ptr)
			dead++;
		if (snap.voice_ptr)
			dead++;
		if (snap.hwnd)
			dead++;
		for (i = 0; i < ATLAS_SLOTS; i++) {
			if (snap.atlas_com[i])
				dead++;
		}
		printf("xsession prove: saved pid %u vs live %u; %d historical pointer(s) "
		       "(engine/wine/dxgi/atlas/xa2/hwnd) — never memcpy COM/HWND\n",
		       snap.saved_pid, (unsigned)GetCurrentProcessId(), dead);
		printf("%s: stale pin set is historical only\n", dead ? "PASS" : "FAIL");
		return dead ? 0 : 1;
	}

	if (!strcmp(cmd, "restore")) {
		unsigned fp, pcm;
		int atlas, hwnd_new, pass;

		if (!world_setup(1))
			return 1;
		free(g_dxgi);
		g_dxgi = NULL;
		*g_seed = snap.seed;
		fill_payload(g_engine, snap.seed.seed);
		dxgi_recreate_from_seed();
		atlas = apply_logical(g_seed);
		fp = fnv1a(g_seed->pix, sizeof(g_seed->pix));
		pcm = fnv1a(g_atlas->pcm, PCM_N);
		hwnd_new = g_atlas->hwnd_id != (unsigned)snap.hwnd;
		printf("xsession restore: seed=%u pix %s mask=0x%x atlas=%s depth=%s pcm %s "
		       "hwnd_new=%s dxgi_gen=%u\n",
		       snap.seed.seed, fp == snap.seed.fp ? "MATCH" : "DIFF", g_seed->live_mask,
		       atlas ? "ok" : "REVIVE",
		       (g_atlas->depth_fp == snap.seed.depth_fp &&
			g_atlas->color2_fp == snap.seed.color2_fp)
			   ? "MATCH"
			   : "DIFF",
		       pcm == snap.seed.pcm_fp ? "MATCH" : "DIFF", hwnd_new ? "yes" : "REWOUND",
		       g_dxgi->gen);
		pass = (fp == snap.seed.fp && g_dxgi->gen == snap.seed.seed && atlas &&
			g_atlas->depth_fp == snap.seed.depth_fp && pcm == snap.seed.pcm_fp &&
			hwnd_new && g_atlas->gdi_fp == snap.seed.gdi_fp);
		world_teardown();
		if (pass) {
			printf("PASS: recreate+re-pin atlas/depth/XA2/GDI/DXGI from logical seed\n");
			return 0;
		}
		printf("FAIL\n");
		return 1;
	}
	return 1;
}

HRESULT WINAPI xa2_sw_create(void **out, UINT32 flags, UINT32 processor);

/* One save/load. with_device calls the software XAudio2, which opens waveOut
 * and leaves it playing across the copy (no waveOutPause under Wine). */
static int audio_cmd(int with_device)
{
	void *eng = NULL;
	HRESULT hr;

	printf("audio: device=%s\n", with_device ? "waveOut via xa2_sw" : "none");
	if (!world_setup(4242)) {
		printf("FAIL: world_setup\n");
		return 1;
	}
	savestate_hooks_install();
	if (with_device) {
		hr = xa2_sw_create(&eng, 0, 0xFFFFFFFFu);
		printf("audio: xa2_sw_create hr=%08lx eng=%p\n", (unsigned long)hr, eng);
		if (hr != 0 || !eng) {
			printf("FAIL: software XAudio2 did not come up\n");
			world_teardown();
			return 1;
		}
		/* A few quanta so the driver thread is inside its unix main loop
		 * before we freeze, the way the title is when KEY_2 arrives. */
		Sleep(400);
	}
	printf("audio: save\n");
	if (!savestate_save(0)) {
		printf("FAIL: save refused\n");
		world_teardown();
		return 1;
	}
	if (savestate_last_was_restore()) {
		printf("PASS: restore resumed inside save; process lived (device=%s)\n",
		       with_device ? "waveOut" : "none");
		world_teardown();
		return 0;
	}
	printf("audio: load (quiesce does not waveOutPause)\n");
	if (!savestate_load(0)) {
		printf("FAIL: load refused\n");
		world_teardown();
		return 1;
	}
	printf("audio: load returned without resuming inside save\n");
	world_teardown();
	return 1;
}

int main(int argc, char **argv)
{
	unsigned seed = 9001;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2) {
		printf("usage: port_harness32.exe insession [cycles]\n"
		       "       port_harness32.exe xsession save|prove|restore <file> [seed]\n"
		       "       port_harness32.exe audio | audio-silent\n");
		return 2;
	}
	if (!strcmp(argv[1], "audio"))
		return audio_cmd(1);
	if (!strcmp(argv[1], "audio-silent"))
		return audio_cmd(0);
	if (!strcmp(argv[1], "insession"))
		return insession(argc > 2 ? atoi(argv[2]) : 20);
	if (!strcmp(argv[1], "xsession") && argc >= 4) {
		if (argc > 4)
			seed = (unsigned)strtoul(argv[4], NULL, 10);
		return xsession_cmd(argv[2], argv[3], seed);
	}
	printf("usage: port_harness32.exe insession [cycles]\n"
	       "       port_harness32.exe audio | audio-silent\n");
	return 2;
}
