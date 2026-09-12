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
 * the writer we are trying to prove is gone. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <stdarg.h>
#include <stddef.h>

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
	int head, n;
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

static void cb_ctx(XA2Callback *c, int slot, void *ctx)
{
	if (!c || !c->vtbl)
		return;
	((PFN_CB_CTX)c->vtbl[slot])(c, ctx);
}

static void cb_void(XA2Callback *c, int slot)
{
	if (!c || !c->vtbl)
		return;
	((PFN_CB_VOID)c->vtbl[slot])(c);
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

/* ---------------------------------------------------------------- voice */

static const void *g_src_vt[29];
static const void *g_mst_vt[20];
static const void *g_sub_vt[19];

/* IXAudio2Voice is not IUnknown-derived: the first slot is GetVoiceDetails, and
 * a voice is released with DestroyVoice rather than Release. */
static void WINAPI V_GetVoiceDetails(SwVoice *v, XA2_VOICE_DETAILS *d)
{
	g_calls[M_DETAILS]++;
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
	EnterCriticalSection(&g_cs);
	if (!v->started) {
		v->anchor = now_qpc();
		v->started = 1;
	}
	LeaveCriticalSection(&g_cs);
	return S_OK;
}

static HRESULT WINAPI S_Stop(SwVoice *v, UINT32 flags, UINT32 op)
{
	(void)flags;
	(void)op;
	g_calls[M_STOP]++;
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

static const void *g_eng_vt[12];

static void vt_init(void)
{
	g_eng_vt[0] = (const void *)E_QueryInterface;
	g_eng_vt[1] = (const void *)E_AddRef;
	g_eng_vt[2] = (const void *)E_Release;
	g_eng_vt[3] = (const void *)E_RegisterForCallbacks;
	g_eng_vt[4] = (const void *)E_CreateSourceVoice;
	g_eng_vt[5] = (const void *)E_CreateSubmixVoice;
	g_eng_vt[6] = (const void *)E_CreateMasteringVoice;
	g_eng_vt[7] = (const void *)E_StartEngine;
	g_eng_vt[8] = (const void *)E_StopEngine;
	g_eng_vt[9] = (const void *)E_CommitChanges;
	g_eng_vt[10] = (const void *)E_GetPerformanceData;
	g_eng_vt[11] = (const void *)E_SetDebugConfiguration;

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
	return S_OK;
}
