/* A software XAudio2.
 *
 * The DirectSound route is closed and we know why. Steam has
 * C:\WINDOWS\System32\dsound.dll in the process before our DLL attaches, so a
 * same-folder stub is never asked for the name; and patching
 * DirectSoundCreate8 in the loaded module changed nothing either, because a
 * completed save and load afterwards produced no ds_sw report at all, which
 * means DxLib never called the export. CLSID_DirectSound is in the executable,
 * and a CoCreateInstance goes through the registry to an absolute path and
 * touches neither the name nor the export. Both of our levers miss it by
 * construction.
 *
 * xaudio2_9.dll has neither problem. Steam does not preload it, so the game
 * folder wins the search - measured, not assumed: the load probe attached as
 * ...\Rabi-Ribi\xaudio2_9.DLL and DxLib called straight into it with
 * XAudio2Create(flags=0, processor=0xFFFFFFFF).
 *
 * It is also the easier thing to stand in for, and for a reason that goes to
 * the heart of the bug. DirectSound is a ring with a hardware play cursor, and
 * every failure this project has chased came from that cursor and the game's
 * write position being two clocks that rewind differently. XAudio2 pushes:
 * the game hands us whole buffers and asks how many samples have played. There
 * is no ring to wrap, no write cursor to keep ahead of a play cursor, and no
 * unsigned subtraction that can come out negative. The state is a queue we own
 * and a sample count we advance.
 *
 * There is no audio thread here, deliberately. Real XAudio2 fires
 * OnBufferEnd from its own mixer thread, which is exactly the kind of second
 * party we are removing - something that runs while every game thread is
 * suspended and holds pointers into memory we are about to rewind. Instead the
 * clock advances lazily, inside whatever call the game itself makes, so every
 * callback runs on the game's thread. Nothing touches audio state except the
 * game, which is the process shape -nosound has.
 *
 * Phase one is silent. Nothing is mixed and nothing reaches the speakers; the
 * PCM the game submits is never even read. The question this answers is
 * whether the game survives a restore with its audio entirely inside memory we
 * own, and putting a mixer thread in before that is answered would reintroduce
 * the writer we are trying to prove is gone.
 *
 * Execute-at-0 after CreateMasteringVoice is not a missing vtable slot here.
 * DxLib delay-loads X3DAudioInitialize from the same xaudio2_9.dll that
 * answered XAudio2Create; those extra names are exported from xa2_fwd.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stddef.h>
#include "savestate.h"

void savestate_log_line(const char *s);

static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

#define XA2_LOOP_INFINITE 255
#define XA2_END_OF_STREAM 0x0040
#define XA2_MAX_QUEUED 64
#define XA2_MAX_VOICES 1024

typedef struct {
	UINT32 Flags;
	UINT32 AudioBytes;
	const BYTE *pAudioData;
	UINT32 PlayBegin;
	UINT32 PlayLength;
	UINT32 LoopBegin;
	UINT32 LoopLength;
	UINT32 LoopCount;
	void *pContext;
} XA2_BUFFER;

typedef struct {
	void *pCurrentBufferContext;
	UINT32 BuffersQueued;
	UINT64 SamplesPlayed;
} XA2_VOICE_STATE;

typedef struct {
	UINT32 CreationFlags;
	UINT32 ActiveFlags;
	UINT32 InputChannels;
	UINT32 InputSampleRate;
} XA2_VOICE_DETAILS;

/* IXAudio2VoiceCallback: seven stdcall methods, no IUnknown at the front. */
typedef struct XA2Callback XA2Callback;
struct XA2Callback {
	void(WINAPI *const *vtbl)(void);
};
enum {
	CB_PASS_START = 0,
	CB_PASS_END,
	CB_STREAM_END,
	CB_BUFFER_START,
	CB_BUFFER_END,
	CB_LOOP_END,
	CB_VOICE_ERROR
};

typedef void(WINAPI *PFN_CB_VOID)(void *);
typedef void(WINAPI *PFN_CB_CTX)(void *, void *);

/* ---------------------------------------------------------------- state */

typedef struct SwVoice SwVoice;

struct SwVoice {
	const void **vtbl; /* first: this is a COM-shaped object */
	int kind;	   /* 0 source, 1 mastering, 2 submix */
	int alive;
	XA2Callback *cb;
	UINT32 channels, rate, bits, block;
	float freq_ratio, volume;
	double rate_eff; /* samples per second after the frequency ratio */
	int started;
	LONGLONG anchor; /* rewound QPC at which `played` and `pos` were current */
	UINT64 played;	 /* samples, the number the game asks for */
	UINT32 pos;	 /* samples into the buffer at the head of the queue */
	UINT32 frac;	 /* 16.16 remainder of pos, for rate conversion */
	int head, n;
	/* Panning arrives as an output matrix rather than a pan value, so it is
	 * kept in the shape it was given and folded in at mix time. */
	int mtx_src, mtx_dst;
	float mtx[8];
	XA2_BUFFER q[XA2_MAX_QUEUED];
};

typedef struct {
	const void **vtbl;
	LONG ref;
} SwEngine;

enum {
	M_CREATE_SOURCE = 0,
	M_CREATE_MASTER,
	M_CREATE_SUBMIX,
	M_START,
	M_STOP,
	M_SUBMIT,
	M_FLUSH,
	M_GETSTATE,
	M_FREQRATIO,
	M_VOLUME,
	M_CHANVOL,
	M_OUTMATRIX,
	M_DESTROY,
	M_DISCONT,
	M_EXITLOOP,
	M_DETAILS,
	M_MAX
};
static const char *const g_mname[M_MAX] = {
	"CreateSourceVoice", "CreateMasteringVoice", "CreateSubmixVoice",
	"Start",	     "Stop",		    "SubmitSourceBuffer",
	"FlushSourceBuffers", "GetState",	    "SetFrequencyRatio",
	"SetVolume",	     "SetChannelVolumes",   "SetOutputMatrix",
	"DestroyVoice",	     "Discontinuity",	    "ExitLoop",
	"GetVoiceDetails"
};
static unsigned long g_calls[M_MAX];

/* The last few calls, in memory, written out only if something faults.
 *
 * The savestate log is no help here: the engine that owns it had not booted
 * when the game died at one second in, so every ss_log from this file went
 * nowhere. A file write per call would answer that but costs a syscall on a
 * path taken thousands of times a second. A ring costs a memcpy and is only
 * read when there is a fault to explain, which is the right trade for a hook
 * that is supposed to be invisible when it works. */
#define XA2_RING 24
static char g_ring[XA2_RING][112];
static unsigned g_ring_n;

static void tr(const char *fmt, ...)
{
	va_list ap;
	unsigned slot = g_ring_n++ % XA2_RING;

	va_start(ap, fmt);
	wvsprintfA(g_ring[slot], fmt, ap);
	va_end(ap);
}

void xa2_sw_trace_dump(void (*emit)(const char *))
{
	unsigned i, first;

	if (!emit || !g_ring_n)
		return;
	emit("xa2_sw: the calls leading up to this, oldest first:");
	first = g_ring_n > XA2_RING ? g_ring_n - XA2_RING : 0;
	for (i = first; i < g_ring_n; i++) {
		char line[160];

		wsprintfA(line, "  %4u %s", i, g_ring[i % XA2_RING]);
		emit(line);
	}
}

/* A roster the mixer can walk. Static, so it lives in this DLL's data and is
 * held in the present - which is right: who exists is present tense, while what
 * each voice contains is state and stays captured in the arena. A voice created
 * after a save rewinds to zeroed arena memory, so `alive` reads false and the
 * mixer steps over it rather than playing something that no longer happened. */
static SwVoice *g_vtab[XA2_MAX_VOICES];
static int g_vn;

/* Set once waveOut is running. Declared here because `advance` has to know to
 * stand aside well before the mixer that sets it is defined. */
static int g_out_live;

static CRITICAL_SECTION g_cs;
static int g_ready;
static LONGLONG g_qpf;
static unsigned char *g_chunk;
static SIZE_T g_chunk_left;
static unsigned long g_voices, g_submits, g_starved, g_overflow;

/* Same arena rule as ds_sw: ordinary private read-write memory, captured by the
 * snapshot, not on the wrapper's CRT heap and not excluded. The queue, the
 * sample count and the timestamp it was anchored to have to come back from one
 * moment or we have rebuilt the split inside our own code. The submitted PCM is
 * not copied here - it stays where the game put it, in the game's memory, and
 * rewinds with the game, which is the correct owner for it. */
#define XA2_CHUNK (1u * 1024u * 1024u)

static void *arena_alloc(SIZE_T n)
{
	void *p;

	n = (n + 15) & ~(SIZE_T)15;
	if (n > g_chunk_left) {
		SIZE_T want = n > XA2_CHUNK ? n : XA2_CHUNK;

		p = VirtualAlloc(NULL, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!p)
			return NULL;
		if (n > XA2_CHUNK)
			return p;
		g_chunk = (unsigned char *)p;
		g_chunk_left = want;
	}
	p = g_chunk;
	g_chunk += n;
	g_chunk_left -= n;
	return p;
}

static LONGLONG now_qpc(void)
{
	LARGE_INTEGER t;

	/* The hooked counter, on purpose: it rewinds with the game, which is what
	 * makes the sample count and the game's own idea of time agree after a
	 * restore. */
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

/* ------------------------------------------------------------- callbacks */

/* Deferred, and that is the point.
 *
 * Once there is a mixer thread, it is the thread that discovers a buffer has
 * finished - and calling OnBufferEnd from there would put a foreign thread
 * inside DxLib's code, allocating and submitting, possibly in the middle of a
 * save. That is the exact hazard this project exists to remove, rebuilt by our
 * own hand.
 *
 * So the mixer only records what happened. The game's next call into us drains
 * the queue on the game's own thread, which is a frame of latency at worst and
 * keeps the invariant that nothing but the game ever calls the game. The queue
 * is static, so it lives in this DLL's data, which is held in the present -
 * correct, because a pending notification is about the present and not part of
 * the state being rewound. */
#define XA2_PEND 256

typedef struct {
	XA2Callback *cb;
	int slot;
	void *ctx;
} Pending;

static Pending g_pend[XA2_PEND];
static int g_pend_head, g_pend_n;
static unsigned long g_pend_lost;

static void cb_post(XA2Callback *c, int slot, void *ctx)
{
	if (!c || !c->vtbl)
		return;
	if (g_pend_n >= XA2_PEND) {
		g_pend_lost++;
		return;
	}
	g_pend[(g_pend_head + g_pend_n) % XA2_PEND].cb = c;
	g_pend[(g_pend_head + g_pend_n) % XA2_PEND].slot = slot;
	g_pend[(g_pend_head + g_pend_n) % XA2_PEND].ctx = ctx;
	g_pend_n++;
}

/* Called on the game's thread, never holding the lock: DxLib submits more audio
 * from inside OnBufferEnd, and that path comes straight back in here. */
static void cb_flush(void)
{
	static LONG busy;

	/* DxLib submits more audio from inside OnBufferEnd, and that path comes
	 * straight back here. Draining only at the outermost level keeps the
	 * recursion from nesting once per notification. */
	if (InterlockedCompareExchange(&busy, 1, 0))
		return;
	for (;;) {
		Pending p;

		EnterCriticalSection(&g_cs);
		if (!g_pend_n) {
			LeaveCriticalSection(&g_cs);
			InterlockedExchange(&busy, 0);
			return;
		}
		p = g_pend[g_pend_head];
		g_pend_head = (g_pend_head + 1) % XA2_PEND;
		g_pend_n--;
		LeaveCriticalSection(&g_cs);
		if (p.slot == CB_STREAM_END)
			((PFN_CB_VOID)p.cb->vtbl[p.slot])(p.cb);
		else
			((PFN_CB_CTX)p.cb->vtbl[p.slot])(p.cb, p.ctx);
	}
}

static void cb_ctx(XA2Callback *c, int slot, void *ctx)
{
	cb_post(c, slot, ctx);
}

static void cb_void(XA2Callback *c, int slot)
{
	cb_post(c, slot, NULL);
}

/* ---------------------------------------------------------------- clock */

static UINT32 buf_samples(const SwVoice *v, const XA2_BUFFER *b)
{
	if (b->PlayLength)
		return b->PlayLength;
	return v->block ? b->AudioBytes / v->block : 0;
}

/* Move the voice forward to now, retiring buffers and firing the callbacks the
 * game is waiting on. Called from inside the game's own calls, so every
 * callback runs on the game's thread and nothing here needs a thread of its
 * own. */
static void advance(SwVoice *v)
{
	LONGLONG now, d;
	unsigned long long s;

	if (!v->started || v->kind != 0 || v->rate_eff <= 0.0)
		return;
	if (g_out_live)
		return; /* the mixer moves it, by audio actually produced */
	now = now_qpc();
	d = now - v->anchor;
	if (d <= 0)
		return;
	s = (unsigned long long)((double)d * v->rate_eff / (double)g_qpf);
	if (!s)
		return;
	/* Keep the sub-sample remainder rather than dropping it, or a voice polled
	 * often enough would never advance at all. */
	v->anchor += (LONGLONG)((double)s * (double)g_qpf / v->rate_eff);

	while (s) {
		XA2_BUFFER *b;
		UINT32 total, end, take, avail;

		if (!v->n) {
			/* Starved. Real XAudio2 does not advance SamplesPlayed through
			 * silence it was never given, and neither do we - the game uses
			 * that number to decide when to submit more. */
			v->anchor = now;
			g_starved++;
			break;
		}
		b = &v->q[v->head];
		total = buf_samples(v, b);
		if (!total) {
			cb_ctx(v->cb, CB_BUFFER_END, b->pContext);
			v->head = (v->head + 1) % XA2_MAX_QUEUED;
			v->n--;
			v->pos = 0;
			continue;
		}
		if (v->pos == 0)
			cb_ctx(v->cb, CB_BUFFER_START, b->pContext);

		/* A looping buffer ends at the loop point, not at the buffer end. BGM
		 * lives on this path, and getting it wrong would retire the music
		 * after one pass. */
		end = total;
		if (b->LoopCount && b->LoopLength)
			end = b->LoopBegin + b->LoopLength;
		if (end > total)
			end = total;
		avail = end > v->pos ? end - v->pos : 0;
		take = (UINT32)(s < avail ? s : avail);
		v->pos += take;
		v->played += take;
		s -= take;
		if (v->pos < end)
			break;
		if (b->LoopCount == XA2_LOOP_INFINITE) {
			cb_ctx(v->cb, CB_LOOP_END, b->pContext);
			v->pos = b->LoopBegin;
			continue;
		}
		if (b->LoopCount) {
			b->LoopCount--;
			cb_ctx(v->cb, CB_LOOP_END, b->pContext);
			v->pos = b->LoopBegin;
			continue;
		}
		cb_ctx(v->cb, CB_BUFFER_END, b->pContext);
		if (b->Flags & XA2_END_OF_STREAM)
			cb_void(v->cb, CB_STREAM_END);
		v->head = (v->head + 1) % XA2_MAX_QUEUED;
		v->n--;
		v->pos = 0;
	}
}

/* ---------------------------------------------------------------- mixer */

/* Speakers, at last, and on our terms.
 *
 * The rule that made the silent build safe still holds: nothing outside the
 * game may touch the game's memory at a moment of our choosing. A mixer thread
 * plainly does touch it - it reads the PCM DxLib submitted, which lives in the
 * game's heap and rewinds with the game. The difference from every audio stack
 * we have fought is that this thread is ours, so stopping it is real. XAudio2's
 * own mixer could be asked to stop and would keep reading; this one is parked
 * before the first byte of a snapshot is copied and does not run again until
 * the restore is finished.
 *
 * Sample position is driven by audio actually produced rather than by the
 * clock. That is strictly better than the QPC scheme it replaces: the count
 * only moves when we do work, so parking the mixer freezes it exactly, and
 * there is no wall-clock term left to disagree with a rewind.
 *
 * Output is fixed at 44100/16/stereo because every format in the survey
 * resamples into it cleanly and a fixed sink is one less thing that can change
 * underneath a restore. waveOut rather than anything newer: winmm is already in
 * the process, it needs no COM, no device enumeration and no session
 * management, and it brings none of AUDIOSES or MMDevApi with it. */
#define OUT_RATE 44100
#define OUT_CH 2
#define OUT_FRAMES 1024 /* about 23 ms; four of these is a comfortable buffer */
#define OUT_BLOCKS 4

typedef struct {
	WAVEHDR hdr[OUT_BLOCKS];
	short pcm[OUT_BLOCKS][OUT_FRAMES * OUT_CH];
	int acc[OUT_FRAMES * OUT_CH];
} MixOut;

static HWAVEOUT g_wo;
static MixOut *g_out;
static HANDLE g_mix_thr, g_mix_wake;
static volatile LONG g_mix_quit, g_mix_park, g_mix_idle;
static unsigned long g_blocks_out;

static int voice_playing(const SwVoice *v)
{
	return v->alive && v->started && v->kind == 0 && v->n;
}

/* One voice into the accumulator. Returns having advanced that voice's position
 * by exactly the audio it contributed, which is what makes SamplesPlayed a
 * report of work done rather than of time passed. */
static void mix_voice(SwVoice *v, int *acc, unsigned frames)
{
	unsigned step, f;
	float gl, gr;

	if (v->rate_eff <= 0.0)
		return;
	step = (unsigned)((v->rate_eff * 65536.0) / (double)OUT_RATE + 0.5);
	if (!step)
		step = 1;

	gl = gr = v->volume;
	if (v->mtx_dst == 2 && v->mtx_src == 1) {
		gl *= v->mtx[0];
		gr *= v->mtx[1];
	}

	for (f = 0; f < frames; f++) {
		XA2_BUFFER *b;
		UINT32 total, end;
		const short *s16;
		int l, r;
		UINT32 idx;

		if (!v->n) {
			g_starved++;
			return;
		}
		b = &v->q[v->head];
		total = buf_samples(v, b);
		end = total;
		if (b->LoopCount && b->LoopLength)
			end = b->LoopBegin + b->LoopLength;
		if (end > total)
			end = total;

		if (!total || v->pos >= end) {
			/* Retire or loop, then take this output frame again from
			 * whatever is next. */
			if (total && b->LoopCount) {
				if (b->LoopCount != XA2_LOOP_INFINITE)
					b->LoopCount--;
				cb_ctx(v->cb, CB_LOOP_END, b->pContext);
				v->pos = b->LoopBegin;
			} else {
				cb_ctx(v->cb, CB_BUFFER_END, b->pContext);
				if (b->Flags & XA2_END_OF_STREAM)
					cb_void(v->cb, CB_STREAM_END);
				v->head = (v->head + 1) % XA2_MAX_QUEUED;
				v->n--;
				v->pos = 0;
				v->frac = 0;
			}
			f--;
			continue;
		}
		if (v->pos == 0 && v->frac == 0)
			cb_ctx(v->cb, CB_BUFFER_START, b->pContext);

		idx = b->PlayBegin + v->pos;
		s16 = (const short *)b->pAudioData;
		if (!s16 || v->bits != 16 || (idx + 1) * v->block > b->AudioBytes) {
			/* Anything we cannot read confidently contributes silence. The
			 * buffer still advances, so a format we do not handle costs the
			 * sound and not the timing. */
			l = r = 0;
		} else if (v->channels >= 2) {
			l = s16[idx * v->channels];
			r = s16[idx * v->channels + 1];
		} else {
			l = r = s16[idx];
		}
		acc[f * OUT_CH] += (int)(l * gl);
		acc[f * OUT_CH + 1] += (int)(r * gr);

		v->frac += step;
		v->pos += v->frac >> 16;
		v->played += v->frac >> 16;
		v->frac &= 0xFFFF;
	}
}

static void mix_block(short *out, int *acc, unsigned frames)
{
	unsigned i;
	SwVoice *v;

	ZeroMemory(acc, frames * OUT_CH * sizeof(int));
	EnterCriticalSection(&g_cs);
	for (i = 0; i < (unsigned)g_vn; i++) {
		v = g_vtab[i];
		if (v && voice_playing(v))
			mix_voice(v, acc, frames);
	}
	LeaveCriticalSection(&g_cs);
	for (i = 0; i < frames * OUT_CH; i++) {
		int s = acc[i];

		/* Clamp rather than wrap. With hundreds of voices a sum can exceed
		 * the range, and wrapping turns a loud moment into a bang. */
		if (s > 32767)
			s = 32767;
		else if (s < -32768)
			s = -32768;
		out[i] = (short)s;
	}
}

static DWORD WINAPI mix_main(LPVOID p)
{
	(void)p;
	for (;;) {
		int i, sent = 0;

		if (g_mix_quit)
			break;
		if (g_mix_park) {
			InterlockedExchange(&g_mix_idle, 1);
			WaitForSingleObject(g_mix_wake, 10);
			continue;
		}
		InterlockedExchange(&g_mix_idle, 0);
		for (i = 0; i < OUT_BLOCKS; i++) {
			WAVEHDR *h = &g_out->hdr[i];

			if (!(h->dwFlags & WHDR_DONE))
				continue;
			h->dwFlags &= ~WHDR_DONE;
			mix_block(g_out->pcm[i], g_out->acc, OUT_FRAMES);
			h->dwBufferLength = OUT_FRAMES * OUT_CH * sizeof(short);
			if (waveOutWrite(g_wo, h, sizeof(*h)) == MMSYSERR_NOERROR) {
				g_blocks_out++;
				sent = 1;
			} else
				h->dwFlags |= WHDR_DONE;
		}
		if (!sent)
			WaitForSingleObject(g_mix_wake, 5);
	}
	return 0;
}

/* Excluded from the snapshot on purpose. These are the present tense: sixteen
 * kilobytes of samples on their way to the speakers, and a waveOut header the
 * driver owns a pointer into. Rewinding either would hand the driver a block it
 * has already returned, which is the very mistake the rest of this file is
 * built to avoid. The voices, which are state, stay captured. */
static void out_start(void)
{
	WAVEFORMATEX wf;
	int i;

	if (g_out_live)
		return;
	g_out = (MixOut *)VirtualAlloc(NULL, sizeof(MixOut), MEM_COMMIT | MEM_RESERVE,
				       PAGE_READWRITE);
	if (!g_out)
		return;
	savestate_exclude(g_out, sizeof(MixOut));

	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = OUT_CH;
	wf.nSamplesPerSec = OUT_RATE;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = OUT_CH * 2;
	wf.nAvgBytesPerSec = OUT_RATE * wf.nBlockAlign;
	wf.cbSize = 0;

	g_mix_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_mix_wake, 0, CALLBACK_EVENT) !=
	    MMSYSERR_NOERROR) {
		g_wo = NULL;
		ss_log("xa2_sw: waveOut would not open, so the engine stays silent - "
		       "everything else about the restore is unaffected\n");
		return;
	}
	for (i = 0; i < OUT_BLOCKS; i++) {
		WAVEHDR *h = &g_out->hdr[i];

		ZeroMemory(h, sizeof(*h));
		h->lpData = (LPSTR)g_out->pcm[i];
		h->dwBufferLength = OUT_FRAMES * OUT_CH * sizeof(short);
		waveOutPrepareHeader(g_wo, h, sizeof(*h));
		h->dwFlags |= WHDR_DONE;
	}
	g_out_live = 1;
	g_mix_thr = CreateThread(NULL, 0, mix_main, NULL, 0, NULL);
	if (g_mix_thr)
		SetThreadPriority(g_mix_thr, THREAD_PRIORITY_ABOVE_NORMAL);
	ss_log("xa2_sw: mixing to waveOut at %d Hz, %d channel(s), %d x %d frame(s). "
	       "The mixer is ours, so it stops for a save - which is the one thing "
	       "no sound card has ever agreed to do\n",
	       OUT_RATE, OUT_CH, OUT_BLOCKS, OUT_FRAMES);
}

/* Called before a snapshot is copied and before a restore writes anything. The
 * wait is bounded because a mixer that will not stop must not be allowed to
 * hold up a save; if it ever times out the log says so rather than continuing
 * on an assumption. */
void xa2_sw_park(void)
{
	int spins;

	if (!g_out_live)
		return;
	InterlockedExchange(&g_mix_park, 1);
	SetEvent(g_mix_wake);
	for (spins = 0; spins < 200 && !g_mix_idle; spins++)
		Sleep(1);
	if (!g_mix_idle)
		ss_log("xa2_sw: the mixer did not park within 200 ms - the copy is "
		       "going ahead anyway, so treat any audio corruption in this "
		       "restore as explained\n");
	waveOutPause(g_wo);
}

void xa2_sw_resume(void)
{
	if (!g_out_live)
		return;
	waveOutRestart(g_wo);
	InterlockedExchange(&g_mix_park, 0);
	SetEvent(g_mix_wake);
}

/* ---------------------------------------------------------------- voice */

static const void *g_src_vt[29];
static const void *g_mst_vt[20];
static const void *g_sub_vt[19];

/* IXAudio2Voice is not IUnknown-derived: the first slot is GetVoiceDetails, and
 * a voice is released with DestroyVoice rather than Release. */
static void WINAPI V_GetVoiceDetails(SwVoice *v, XA2_VOICE_DETAILS *d)
{
	g_calls[M_DETAILS]++;
	tr("GetVoiceDetails v=%08lX kind=%d", (unsigned long)(UINT_PTR)v, v->kind);
	if (!d)
		return;
	d->CreationFlags = 0;
	d->ActiveFlags = 0;
	d->InputChannels = v->channels;
	d->InputSampleRate = v->rate;
}

static HRESULT WINAPI V_SetOutputVoices(SwVoice *v, const void *sends)
{
	(void)v;
	(void)sends;
	return S_OK;
}

static HRESULT WINAPI V_SetEffectChain(SwVoice *v, const void *chain)
{
	(void)v;
	(void)chain;
	return S_OK;
}

static HRESULT WINAPI V_EnableEffect(SwVoice *v, UINT32 i, UINT32 op)
{
	(void)v;
	(void)i;
	(void)op;
	return S_OK;
}

static HRESULT WINAPI V_DisableEffect(SwVoice *v, UINT32 i, UINT32 op)
{
	(void)v;
	(void)i;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetEffectState(SwVoice *v, UINT32 i, BOOL *on)
{
	(void)v;
	(void)i;
	if (on)
		*on = FALSE;
}

static HRESULT WINAPI V_SetEffectParameters(SwVoice *v, UINT32 i, const void *p, UINT32 n,
					    UINT32 op)
{
	(void)v;
	(void)i;
	(void)p;
	(void)n;
	(void)op;
	return S_OK;
}

static HRESULT WINAPI V_GetEffectParameters(SwVoice *v, UINT32 i, void *p, UINT32 n)
{
	(void)v;
	(void)i;
	(void)p;
	(void)n;
	return S_OK;
}

static HRESULT WINAPI V_SetFilterParameters(SwVoice *v, const void *p, UINT32 op)
{
	(void)v;
	(void)p;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetFilterParameters(SwVoice *v, void *p)
{
	(void)v;
	(void)p;
}

static HRESULT WINAPI V_SetOutputFilterParameters(SwVoice *v, void *dst, const void *p, UINT32 op)
{
	(void)v;
	(void)dst;
	(void)p;
	(void)op;
	return S_OK;
}

static void WINAPI V_GetOutputFilterParameters(SwVoice *v, void *dst, void *p)
{
	(void)v;
	(void)dst;
	(void)p;
}

static HRESULT WINAPI V_SetVolume(SwVoice *v, float vol, UINT32 op)
{
	(void)op;
	g_calls[M_VOLUME]++;
	v->volume = vol;
	return S_OK;
}

static void WINAPI V_GetVolume(SwVoice *v, float *vol)
{
	if (vol)
		*vol = v->volume;
}

static HRESULT WINAPI V_SetChannelVolumes(SwVoice *v, UINT32 n, const float *vols, UINT32 op)
{
	(void)v;
	(void)n;
	(void)vols;
	(void)op;
	g_calls[M_CHANVOL]++;
	return S_OK;
}

static void WINAPI V_GetChannelVolumes(SwVoice *v, UINT32 n, float *vols)
{
	UINT32 i;

	(void)v;
	for (i = 0; vols && i < n; i++)
		vols[i] = 1.0f;
}

static HRESULT WINAPI V_SetOutputMatrix(SwVoice *v, void *dst, UINT32 src_ch, UINT32 dst_ch,
					const float *m, UINT32 op)
{
	(void)v;
	(void)dst;
	(void)src_ch;
	(void)dst_ch;
	(void)m;
	(void)op;
	/* Panning arrives here rather than as a pan value. Stored nowhere while we
	 * are silent, but counted, because it is the second half of the question
	 * about how this game positions its sounds. */
	tr("SetOutputMatrix src=%u dst=%u", src_ch, dst_ch);
	g_calls[M_OUTMATRIX]++;
	return S_OK;
}

static void WINAPI V_GetOutputMatrix(SwVoice *v, void *dst, UINT32 src_ch, UINT32 dst_ch, float *m)
{
	UINT32 i;

	(void)v;
	(void)dst;
	for (i = 0; m && i < src_ch * dst_ch; i++)
		m[i] = 1.0f;
}

static void WINAPI V_DestroyVoice(SwVoice *v)
{
	g_calls[M_DESTROY]++;
	tr("DestroyVoice v=%08lX", (unsigned long)(UINT_PTR)v);
	EnterCriticalSection(&g_cs);
	/* Marked dead, never reused. An address that meant one sound at save time
	 * and a different one at restore time is the entity-pool problem in
	 * miniature, and the arena is cheap enough that we simply never take that
	 * risk. */
	v->alive = 0;
	v->started = 0;
	v->n = 0;
	LeaveCriticalSection(&g_cs);
}

/* --------------------------------------------------- source voice extras */

static HRESULT WINAPI S_Start(SwVoice *v, UINT32 flags, UINT32 op)
{
	(void)flags;
	(void)op;
	g_calls[M_START]++;
	tr("Start v=%08lX", (unsigned long)(UINT_PTR)v);
	EnterCriticalSection(&g_cs);
	if (!v->started) {
		v->anchor = now_qpc();
		v->started = 1;
	}
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static HRESULT WINAPI S_Stop(SwVoice *v, UINT32 flags, UINT32 op)
{
	(void)flags;
	(void)op;
	g_calls[M_STOP]++;
	tr("Stop v=%08lX", (unsigned long)(UINT_PTR)v);
	EnterCriticalSection(&g_cs);
	advance(v);
	v->started = 0;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI S_SubmitSourceBuffer(SwVoice *v, const XA2_BUFFER *b, const void *wmadata)
{
	(void)wmadata;
	g_calls[M_SUBMIT]++;
	tr("SubmitSourceBuffer v=%08lX bytes=%u loop=%u", (unsigned long)(UINT_PTR)v,
	   b ? b->AudioBytes : 0, b ? b->LoopCount : 0);
	if (!b)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	advance(v);
	if (v->n >= XA2_MAX_QUEUED) {
		/* Real XAudio2 allows 64 queued buffers and returns an error past
		 * that; DxLib streams with two or three, so reaching this means
		 * something is wrong rather than busy. Counted so it cannot happen
		 * quietly. */
		g_overflow++;
		LeaveCriticalSection(&g_cs);
		return E_FAIL;
	}
	v->q[(v->head + v->n) % XA2_MAX_QUEUED] = *b;
	v->n++;
	g_submits++;
	LeaveCriticalSection(&g_cs);
	cb_flush();
	return S_OK;
}

static HRESULT WINAPI S_FlushSourceBuffers(SwVoice *v)
{
	g_calls[M_FLUSH]++;
	EnterCriticalSection(&g_cs);
	v->n = 0;
	v->pos = 0;
	v->anchor = now_qpc();
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI S_Discontinuity(SwVoice *v)
{
	(void)v;
	g_calls[M_DISCONT]++;
	return S_OK;
}

static HRESULT WINAPI S_ExitLoop(SwVoice *v, UINT32 op)
{
	(void)op;
	g_calls[M_EXITLOOP]++;
	EnterCriticalSection(&g_cs);
	if (v->n)
		v->q[v->head].LoopCount = 0;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

/* Two arguments, as in the XAudio2 2.8 and later headers. This matters more
 * than it looks: these are stdcall, so the callee pops the arguments, and a
 * one-argument definition called with two would walk the caller's stack. The
 * game asks for xaudio2_9 by name, so it is compiled against the header that
 * has the Flags parameter. */
static void WINAPI S_GetState(SwVoice *v, XA2_VOICE_STATE *st, UINT32 flags)
{
	(void)flags;
	g_calls[M_GETSTATE]++;
	if (!st)
		return;
	EnterCriticalSection(&g_cs);
	advance(v);
	st->pCurrentBufferContext = v->n ? v->q[v->head].pContext : NULL;
	st->BuffersQueued = (UINT32)v->n;
	st->SamplesPlayed = v->played;
	LeaveCriticalSection(&g_cs);
	cb_flush();
}

static HRESULT WINAPI S_SetFrequencyRatio(SwVoice *v, float ratio, UINT32 op)
{
	(void)op;
	g_calls[M_FREQRATIO]++;
	EnterCriticalSection(&g_cs);
	/* Settle first: samples already played were played at the old rate, and
	 * changing the rate without folding them in would move the count
	 * retroactively. */
	advance(v);
	v->freq_ratio = ratio > 0.0f ? ratio : 1.0f;
	v->rate_eff = (double)v->rate * (double)v->freq_ratio;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static void WINAPI S_GetFrequencyRatio(SwVoice *v, float *ratio)
{
	if (ratio)
		*ratio = v->freq_ratio;
}

static HRESULT WINAPI S_SetSourceSampleRate(SwVoice *v, UINT32 rate)
{
	EnterCriticalSection(&g_cs);
	advance(v);
	v->rate = rate ? rate : v->rate;
	v->rate_eff = (double)v->rate * (double)v->freq_ratio;
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI M_GetChannelMask(SwVoice *v, DWORD *mask)
{
	(void)v;
	if (mask)
		*mask = 0x3; /* front left and right */
	return S_OK;
}

/* --------------------------------------------------------------- engine */

static SwVoice *voice_new(int kind, UINT32 channels, UINT32 rate, UINT32 bits, XA2Callback *cb)
{
	SwVoice *v = (SwVoice *)arena_alloc(sizeof(*v));

	if (!v)
		return NULL;
	ZeroMemory(v, sizeof(*v));
	v->vtbl = kind == 0 ? g_src_vt : (kind == 1 ? g_mst_vt : g_sub_vt);
	v->kind = kind;
	v->alive = 1;
	v->cb = cb;
	v->channels = channels ? channels : 2;
	v->rate = rate ? rate : 44100;
	v->bits = bits ? bits : 16;
	v->block = v->channels * (v->bits / 8);
	v->freq_ratio = 1.0f;
	v->volume = 1.0f;
	v->rate_eff = (double)v->rate;
	if (g_vn < XA2_MAX_VOICES)
		g_vtab[g_vn++] = v;
	g_voices++;
	return v;
}

static HRESULT WINAPI E_QueryInterface(SwEngine *e, const GUID *iid, void **out)
{
	(void)iid;
	if (!out)
		return E_INVALIDARG;
	InterlockedIncrement(&e->ref);
	*out = e;
	return S_OK;
}

static ULONG WINAPI E_AddRef(SwEngine *e)
{
	return (ULONG)InterlockedIncrement(&e->ref);
}

static ULONG WINAPI E_Release(SwEngine *e)
{
	LONG r = InterlockedDecrement(&e->ref);

	return (ULONG)(r < 0 ? 0 : r);
}

static HRESULT WINAPI E_RegisterForCallbacks(SwEngine *e, void *cb)
{
	(void)e;
	(void)cb;
	return S_OK;
}

/* Leaving this out cost a run. It sits between RegisterForCallbacks and
 * CreateSourceVoice, so omitting it shifted every entry after it up by one and
 * DxLib's CreateMasteringVoice landed on StartEngine - which returns S_OK
 * without ever writing the out-pointer, so the game read through the NULL it
 * was left holding and died at rabiribi.exe+0x4d6d4 before the first present.
 * A vtable is a contract counted in slots, and a missing void method is as
 * damaging as a wrong one. */
static void WINAPI E_UnregisterForCallbacks(SwEngine *e, void *cb)
{
	(void)e;
	(void)cb;
}

static HRESULT WINAPI E_CreateSourceVoice(SwEngine *e, void **out, const WAVEFORMATEX *fmt,
					  UINT32 flags, float max_ratio, XA2Callback *cb,
					  const void *sends, const void *chain)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)max_ratio;
	(void)sends;
	(void)chain;
	g_calls[M_CREATE_SOURCE]++;
	tr("CreateSourceVoice ch=%u rate=%u bits=%u cb=%08lX",
	   fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0,
	   fmt ? fmt->wBitsPerSample : 0, (unsigned long)(UINT_PTR)cb);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(0, fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0,
		      fmt ? fmt->wBitsPerSample : 0, cb);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

static HRESULT WINAPI E_CreateSubmixVoice(SwEngine *e, void **out, UINT32 channels, UINT32 rate,
					  UINT32 flags, UINT32 stage, const void *sends,
					  const void *chain)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)stage;
	(void)sends;
	(void)chain;
	g_calls[M_CREATE_SUBMIX]++;
	tr("CreateSubmixVoice ch=%u rate=%u", channels, rate);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(2, channels, rate, 16, NULL);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

/* Seven parameters, the XAudio2 2.8 and later shape. The 2.7 version took a
 * device index where this takes a device id string and has no stream category,
 * and mixing the two up on stdcall would unbalance the stack. */
static HRESULT WINAPI E_CreateMasteringVoice(SwEngine *e, void **out, UINT32 channels, UINT32 rate,
					     UINT32 flags, const wchar_t *device,
					     const void *chain, int category)
{
	SwVoice *v;

	(void)e;
	(void)flags;
	(void)device;
	(void)chain;
	(void)category;
	g_calls[M_CREATE_MASTER]++;
	tr("CreateMasteringVoice ch=%u rate=%u", channels, rate);
	if (!out)
		return E_INVALIDARG;
	EnterCriticalSection(&g_cs);
	v = voice_new(1, channels ? channels : 2, rate ? rate : 44100, 16, NULL);
	LeaveCriticalSection(&g_cs);
	if (!v)
		return E_OUTOFMEMORY;
	*out = v;
	return S_OK;
}

static HRESULT WINAPI E_StartEngine(SwEngine *e)
{
	(void)e;
	return S_OK;
}

static void WINAPI E_StopEngine(SwEngine *e)
{
	(void)e;
}

static HRESULT WINAPI E_CommitChanges(SwEngine *e, UINT32 op)
{
	(void)e;
	(void)op;
	return S_OK;
}

static void WINAPI E_GetPerformanceData(SwEngine *e, void *data)
{
	(void)e;
	/* The struct is large and entirely statistics; zeroing it is honest and is
	 * what a silent engine has to report. */
	if (data)
		ZeroMemory(data, 64);
}

static HRESULT WINAPI E_SetDebugConfiguration(SwEngine *e, const void *cfg, void *reserved)
{
	(void)e;
	(void)cfg;
	(void)reserved;
	return S_OK;
}

static const void *g_eng_vt[13];

static void vt_init(void)
{
	g_eng_vt[0] = (const void *)E_QueryInterface;
	g_eng_vt[1] = (const void *)E_AddRef;
	g_eng_vt[2] = (const void *)E_Release;
	g_eng_vt[3] = (const void *)E_RegisterForCallbacks;
	g_eng_vt[4] = (const void *)E_UnregisterForCallbacks;
	g_eng_vt[5] = (const void *)E_CreateSourceVoice;
	g_eng_vt[6] = (const void *)E_CreateSubmixVoice;
	g_eng_vt[7] = (const void *)E_CreateMasteringVoice;
	g_eng_vt[8] = (const void *)E_StartEngine;
	g_eng_vt[9] = (const void *)E_StopEngine;
	g_eng_vt[10] = (const void *)E_CommitChanges;
	g_eng_vt[11] = (const void *)E_GetPerformanceData;
	g_eng_vt[12] = (const void *)E_SetDebugConfiguration;

	/* IXAudio2Voice, shared prefix of all three voice kinds. */
	g_sub_vt[0] = (const void *)V_GetVoiceDetails;
	g_sub_vt[1] = (const void *)V_SetOutputVoices;
	g_sub_vt[2] = (const void *)V_SetEffectChain;
	g_sub_vt[3] = (const void *)V_EnableEffect;
	g_sub_vt[4] = (const void *)V_DisableEffect;
	g_sub_vt[5] = (const void *)V_GetEffectState;
	g_sub_vt[6] = (const void *)V_SetEffectParameters;
	g_sub_vt[7] = (const void *)V_GetEffectParameters;
	g_sub_vt[8] = (const void *)V_SetFilterParameters;
	g_sub_vt[9] = (const void *)V_GetFilterParameters;
	g_sub_vt[10] = (const void *)V_SetOutputFilterParameters;
	g_sub_vt[11] = (const void *)V_GetOutputFilterParameters;
	g_sub_vt[12] = (const void *)V_SetVolume;
	g_sub_vt[13] = (const void *)V_GetVolume;
	g_sub_vt[14] = (const void *)V_SetChannelVolumes;
	g_sub_vt[15] = (const void *)V_GetChannelVolumes;
	g_sub_vt[16] = (const void *)V_SetOutputMatrix;
	g_sub_vt[17] = (const void *)V_GetOutputMatrix;
	g_sub_vt[18] = (const void *)V_DestroyVoice;

	CopyMemory(g_mst_vt, g_sub_vt, 19 * sizeof(void *));
	g_mst_vt[19] = (const void *)M_GetChannelMask;

	CopyMemory(g_src_vt, g_sub_vt, 19 * sizeof(void *));
	g_src_vt[19] = (const void *)S_Start;
	g_src_vt[20] = (const void *)S_Stop;
	g_src_vt[21] = (const void *)S_SubmitSourceBuffer;
	g_src_vt[22] = (const void *)S_FlushSourceBuffers;
	g_src_vt[23] = (const void *)S_Discontinuity;
	g_src_vt[24] = (const void *)S_ExitLoop;
	g_src_vt[25] = (const void *)S_GetState;
	g_src_vt[26] = (const void *)S_SetFrequencyRatio;
	g_src_vt[27] = (const void *)S_GetFrequencyRatio;
	g_src_vt[28] = (const void *)S_SetSourceSampleRate;
}

/* ---------------------------------------------------------------- entry */

int xa2_sw_armed(void)
{
	return g_ready;
}

/* Printed at every save, next to the restore it explains. The call census is
 * the same instrument the DirectSound survey was, and for the same reason:
 * nobody knows which part of an audio API a game actually uses until it is
 * counted, and the answer decides what a stand-in has to be correct about. */
void xa2_sw_report(void)
{
	int i;

	if (!g_ready)
		return;
	ss_log("xa2_sw: %lu voice(s), %lu buffer(s) submitted, %lu starve(s), %lu "
	       "overflow(s). No XAudio2 threads, no mixer, no cursor moving while "
	       "we copy - every callback ran on the game's own thread\n",
	       g_voices, g_submits, g_starved, g_overflow);
	for (i = 0; i < M_MAX; i++)
		if (g_calls[i])
			ss_log("  %-22s %lu call(s)\n", g_mname[i], g_calls[i]);
	for (i = 0; i < M_MAX; i++)
		if (!g_calls[i])
			ss_log("  %-22s never called\n", g_mname[i]);
}

__declspec(dllexport) HRESULT WINAPI xa2_sw_create(void **out, UINT32 flags, UINT32 processor)
{
	SwEngine *e;
	LARGE_INTEGER f;

	(void)flags;
	(void)processor;
	if (!out)
		return E_INVALIDARG;
	*out = NULL;
	tr("XAudio2Create flags=%u", flags);
	if (!g_ready) {
		InitializeCriticalSection(&g_cs);
		QueryPerformanceFrequency(&f);
		g_qpf = f.QuadPart ? f.QuadPart : 1;
		vt_init();
		g_ready = 1;
		ss_log("xa2_sw: the software XAudio2 is answering. Samples played is a "
		       "number we advance from the rewound clock, held in captured "
		       "memory, and buffers are retired on the game's own thread - so "
		       "there is no second party in this process with an opinion about "
		       "where the audio is\n");
	}
	e = (SwEngine *)arena_alloc(sizeof(*e));
	if (!e)
		return E_OUTOFMEMORY;
	ZeroMemory(e, sizeof(*e));
	e->vtbl = g_eng_vt;
	e->ref = 1;
	*out = e;
	out_start();
	return S_OK;
}
