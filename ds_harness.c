/* Does DirectSound streaming survive a rewind, with no game in the way?
 *
 * The game dies at rabiribi.exe+6E9F8 on a block copy with a negative length,
 * and the magnitude grows with every restore: -128819, then -147107, then
 * -200339. Two things that were meant to explain it do not. Winding the clock
 * back makes no difference, and neither does putting every DirectSound play
 * cursor back where the game left it - both were tried, and the copy still came
 * out negative in the same instruction.
 *
 * What is left is a question about shape rather than about this game, so this
 * asks it without the game. A ring buffer, a play cursor owned by hardware we
 * deliberately never rewind, and a write cursor of our own in ordinary memory
 * that the rewind does take back. That is the entire arrangement the streaming
 * code depends on, reduced to something whose every value we can print.
 *
 * The point is elimination. If the raw subtraction goes negative here, the
 * arrangement is unsound and no amount of care inside the wrapper fixes it -
 * anything that streams audio across a rewind will do this. If it stays
 * positive here, the arrangement is fine and something specific to how DxLib
 * uses it is at fault, which is a much smaller thing to go looking for.
 *
 * Deliberately NOT a reimplementation of DxLib. It reproduces the one
 * calculation that fails, and prints the operands rather than the result.
 *
 *   ds_harness32.exe [cycles] [restores]
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int savestate_save(int slot);
int savestate_load(int slot);
void savestate_guard(void);
void savestate_exclude(void *p, size_t bytes);
int savestate_last_was_restore(void);

/* The engine stops the rasteriser's workers before it suspends the process.
 * There is no rasteriser here and none are running, so this is honest rather
 * than merely convenient - the same stub ss_harness carries. */
void swrast_pool_shutdown(void)
{
}

/* Declared by hand rather than by including dsound.h, for the same reason the
 * hook in the wrapper does: no import library, no GUID library, and only four
 * methods are ever called. */
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

#define RATE 44100
#define CHANS 2
#define BYTES_PER_FRAME (CHANS * 2)
#define RING_BYTES (RATE * BYTES_PER_FRAME) /* one second */

/* The state under test.
 *
 * g_write is the whole experiment. It is an ordinary global, so the rewind
 * takes it back with everything else, while the hardware cursor it gets
 * compared against carries on regardless - exactly the split the game has, and
 * exactly the one that cannot be removed, because a sound card cannot be
 * suspended and resumed with the process. */
static DWORD g_write;
static double g_phase;
static long long g_generated;
/* Set for exactly the first step after a restore, because that is the only step
 * whose cursor relationship is in question. */
static int g_check_gap;

/* Held in the present on purpose: a COM pointer into dsound is a handle to an
 * object we do not rewind, and winding the pointer back while the object moves
 * on is a different bug that would mask this one. */
typedef struct {
	void *dev, *buf;
	int negatives, ticks;
	/* Counted here rather than on the stack: a local would be wound back with
	 * everything else and the run would restore forever. */
	int done_restores;
	long long worst;
} Held;
static Held *g_held;

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
		g_generated += BYTES_PER_FRAME;
	}
}

/* One streaming step, written the way streaming code is normally written.
 *
 * The subtraction on the next line is the entire subject of this harness. Real
 * code writes it this way because in normal running the play cursor is always
 * behind the write cursor, so the difference is always positive and the wrap is
 * handled by the modulo below. Across a rewind that stops being true. */
static int step(void)
{
	struct BufVtbl *v = *(struct BufVtbl **)g_held->buf;
	DWORD play = 0, safe = 0;
	long raw;
	DWORD want;
	void *p1 = NULL, *p2 = NULL;
	DWORD n1 = 0, n2 = 0;

	if (FAILED(v->GetCurrentPosition(g_held->buf, &play, &safe)))
		return 0;
	g_held->ticks++;

	raw = (long)play - (long)g_write;
	/* A raw subtraction goes negative every time the play cursor laps the ring,
	 * which happens constantly and which every streaming loop already handles.
	 * Counting those would drown the signal - the first run of this harness did
	 * exactly that, reporting two "negatives" before a restore had even
	 * happened. What matters is the wrapped distance, and specifically whether
	 * it is still a sane one after the rewind. */
	if (raw < 0)
		raw += RING_BYTES;
	/* In normal running we wrote up to the play cursor a few milliseconds ago,
	 * so the gap is small. A gap approaching the whole ring means the cursor
	 * lapped while we were stopped and the relationship carries no information
	 * any more - the write position is not behind the play position by a little,
	 * it is somewhere arbitrary. That is the state a streaming loop cannot
	 * reason its way out of. */
	if (g_check_gap && raw > (long)(RING_BYTES / 2)) {
		g_held->negatives++;
		if (-raw < g_held->worst)
			g_held->worst = -raw;
		printf("  LOST: play=%lu write=%lu gap=%ld of %d bytes (%.0f ms) - the "
		       "cursor lapped while we were stopped, so this distance is "
		       "arbitrary\n",
		       (unsigned long)play, (unsigned long)g_write, raw, RING_BYTES,
		       1000.0 * raw / (RATE * BYTES_PER_FRAME));
	}
	g_check_gap = 0;
	want = (DWORD)raw;
	if (want > RING_BYTES / 4)
		want = RING_BYTES / 4;
	if (!want)
		return 1;
	if (FAILED(v->Lock(g_held->buf, g_write, want, &p1, &n1, &p2, &n2, 0)))
		return 0;
	fill((unsigned char *)p1, n1);
	if (p2)
		fill((unsigned char *)p2, n2);
	((HRESULT(WINAPI *)(void *, void *, DWORD, void *, DWORD))(
		 ((void **)v)[19]))(g_held->buf, p1, n1, p2, n2); /* Unlock */
	g_write = (g_write + n1 + n2) % RING_BYTES;
	return 1;
}

int main(int argc, char **argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 600;
	int restores = argc > 2 ? atoi(argv[2]) : 3;
	int i;
	HMODULE ds;
	PFN_CREATE8 create8;
	struct DSVtbl *dv;
	struct BufVtbl *bv;
	DSBUFDESC desc;
	WAVEFORMATEX wf;
	HWND wnd;

	setvbuf(stdout, NULL, _IONBF, 0);
	g_held = (Held *)VirtualAlloc(NULL, sizeof(Held), MEM_COMMIT | MEM_RESERVE,
				      PAGE_READWRITE);
	if (!g_held)
		return 1;
	savestate_exclude(g_held, sizeof(Held));
	g_held->worst = 0;

	ds = LoadLibraryA("dsound.dll");
	create8 = ds ? (PFN_CREATE8)(void *)GetProcAddress(ds, "DirectSoundCreate8") : NULL;
	if (!create8) {
		printf("dsound.dll has no DirectSoundCreate8 - nothing to test\n");
		return 1;
	}
	if (FAILED(create8(NULL, &g_held->dev, NULL)) || !g_held->dev) {
		printf("no audio device available - nothing to test\n");
		return 1;
	}
	dv = *(struct DSVtbl **)g_held->dev;
	/* A cooperative level needs a window, and a console has none of its own. */
	wnd = CreateWindowExA(0, "STATIC", "ds_harness", 0, 0, 0, 1, 1, NULL, NULL, NULL,
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
	if (FAILED(dv->CreateSoundBuffer(g_held->dev, &desc, &g_held->buf, NULL))) {
		printf("could not create a streaming buffer\n");
		return 1;
	}
	bv = *(struct BufVtbl **)g_held->buf;

	printf("ds harness: %d cycle(s), %d restore(s), %d byte ring at %d Hz\n", cycles,
	       restores, RING_BYTES, RATE);
	printf("the play cursor belongs to the sound card and is never rewound;\n"
	       "g_write is an ordinary global and is. that is the whole test.\n\n");

	g_write = 0;
	step();
	bv->Play(g_held->buf, 0, 0, DSBPLAY_LOOPING);

	for (i = 0; i < cycles; i++) {
		savestate_guard();
		if (!step())
			break;
		Sleep(8);

		if (i && i % (cycles / (restores + 1)) == 0 &&
		    g_held->done_restores < restores) {
			printf("\n[%d] saving, write=%lu\n", i, (unsigned long)g_write);
			if (!savestate_save(0)) {
				printf("  save failed\n");
				continue;
			}
			/* savestate_save returns twice. The second return is the
			 * restore arriving: the thread reappears here rather than
			 * inside savestate_load, so "save, then load, then carry on"
			 * is control flow that does not exist. Written as a straight
			 * line the first time, this loop restored fourteen times from
			 * one save and then took a fastfail. */
			if (savestate_last_was_restore()) {
				g_held->done_restores++;
				g_check_gap = 1;
				printf("[%d] back from restore, write=%lu - wound back "
				       "from wherever it had reached\n",
				       i, (unsigned long)g_write);
				continue;
			}
			/* Let the hardware run on while the snapshot sits still, which
			 * is what a player does between a save and a load. Longer than
			 * the ring on purpose: if a lap is what breaks the arithmetic
			 * then this is the case that shows it. */
			Sleep(1200);
			printf("[%d] restoring after 1.2 s of playback (ring holds "
			       "1000 ms)\n",
			       i);
			if (!savestate_load(0)) {
				printf("  restore failed\n");
				g_held->done_restores++;
			}
		}
	}

	printf("\n================ result ================\n");
	printf("  ticks              %d\n", g_held->ticks);
	printf("  negative deltas    %d\n", g_held->negatives);
	printf("  worst              %lld bytes\n", g_held->worst);
	if (g_held->negatives)
		printf("\n  The arrangement itself is unsound. A write cursor that gets\n"
		       "  wound back and a play cursor that does not will produce a\n"
		       "  negative length in any streaming loop, and the game's crash\n"
		       "  needs no explanation beyond this.\n");
	else
		printf("\n  The arrangement survives. Rewinding a write cursor against a\n"
		       "  live play cursor did NOT produce a negative delta here, so\n"
		       "  something specific to how the game streams is at fault, not\n"
		       "  the shape of the problem.\n");
	return 0;
}
