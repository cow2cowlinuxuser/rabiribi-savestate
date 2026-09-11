/* Keeping DirectSound's play cursor honest across a restore.
 *
 * The game streams audio by asking where the hardware is playing, comparing
 * that against where it last wrote, and copying the difference. That works
 * because time only moves forward. A restore breaks the assumption: dsound and
 * the audio stack are excluded from the rewind and have to be - they talk to
 * the sound card, and the card keeps playing while the game is stopped - so
 * afterwards the hardware's cursor is in the present and the game's write
 * position is several seconds in the past. The subtraction comes out negative.
 *
 * As an unsigned byte count that is 4.29 billion, and the game hands it to a
 * block copy. Caught at rabiribi.exe+36913A with ecx = FFEAAD9C, and again at
 * +6E9F8 with -128819. The second one wrote decoded audio over the allocator's
 * structures before it reached unmapped memory and faulted: ntdll was later
 * found dereferencing FFD6FFE3, which is not a pointer but two quiet 16-bit
 * samples, -42 and -29.
 *
 * A fault handler cannot fix this. rep movs faults only when it reaches memory
 * that is not there, having faithfully copied everything up to that point, so
 * by the time we see the exception the damage is done and clamping the count
 * only spares the remainder. The copy has to not be issued.
 *
 * So the cursor has to agree with the game's state, which means owning it.
 *
 * There is no proxy DLL here and no wrapper objects. A COM vtable is shared by
 * every instance of an implementation, so creating one throwaway device of our
 * own is enough to learn the address of the table the game's device will also
 * use. Patch the handful of methods that matter, release the throwaway, and
 * every buffer the game creates afterwards comes through us - regardless of
 * whether it got there via DirectSoundCreate8, the older entry point, or
 * CoCreateInstance, because all three land on the same implementation. */

#include "savestate.h"
#include <windows.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#ifndef DSBSTATUS_PLAYING
#define DSBSTATUS_PLAYING 0x00000001
#endif
#ifndef DSBPLAY_LOOPING
#define DSBPLAY_LOOPING 0x00000001
#endif

/* Vtable slots, counted from IUnknown. Fixed by the interface contract, so they
 * are as stable as the interface itself. */
#define DS_CREATE_BUFFER 3
#define DS_DUPLICATE 5

#define DSB_RELEASE 2
#define DSB_LOCK 11
#define DSB_GET_POSITION 4
#define DSB_GET_STATUS 9
#define DSB_PLAY 12
#define DSB_SET_POSITION 13
#define DSB_STOP 18

typedef HRESULT(WINAPI *PFN_DSCREATE8)(const GUID *, void **, void *);
typedef HRESULT(WINAPI *PFN_CREATEBUF)(void *, const void *, void **, void *);
typedef HRESULT(WINAPI *PFN_DUP)(void *, void *, void **);
typedef ULONG(WINAPI *PFN_RELEASE)(void *);
typedef HRESULT(WINAPI *PFN_LOCK)(void *, DWORD, DWORD, void **, DWORD *, void **, DWORD *,
				   DWORD);
typedef HRESULT(WINAPI *PFN_GETPOS)(void *, DWORD *, DWORD *);
typedef HRESULT(WINAPI *PFN_GETSTATUS)(void *, DWORD *);
typedef HRESULT(WINAPI *PFN_PLAY)(void *, DWORD, DWORD, DWORD);
typedef HRESULT(WINAPI *PFN_SETPOS)(void *, DWORD);
typedef HRESULT(WINAPI *PFN_STOP)(void *);

/* DxLib makes a buffer per sound effect, and 256 was a guess that the game
 * silently walked past: a run reported "256 of 256 buffer(s)" recorded, which
 * is the ceiling reporting itself as a result. Anything untracked keeps a play
 * cursor in the present while its write position is wound back, which is the
 * whole bug this file exists to remove. */
#define DSH_MAX 4096

typedef struct Buf {
	void *p;
	DWORD play, write; /* as of the last snapshot */
	DWORD status;
	int held; /* the snapshot has something to say about this one */
	void *owner; /* the return address of whoever asked for it */
} Buf;

static Buf g_buf[DSH_MAX];
static int g_nbuf;
static CRITICAL_SECTION g_cs;
static int g_ready, g_armed, g_capped;

static PFN_CREATEBUF g_real_create;
static PFN_DUP g_real_dup;
static PFN_RELEASE g_real_release;
static PFN_GETPOS g_real_getpos;
static PFN_LOCK g_real_lock;
static void **g_buf_vtbl;

void savestate_log_line(const char *s); /* the engine's log, shared deliberately */

/* wsprintfA rather than the CRT: no floats are printed here and it drags in
 * nothing. It does not understand %p, though, and the two messages that used one
 * were dropped silently for the whole life of this file - which is why the
 * install was invisible in all 38 logged sessions while the save, printing only
 * %d, was not. Addresses are formatted as %08lX here for that reason. Findings go to the savestate log so they sit beside the restore they
 * explain. */
static void ss_log(const char *fmt, ...)
{
	char b[512];
	va_list ap;

	va_start(ap, fmt);
	wvsprintfA(b, fmt, ap);
	va_end(ap);
	savestate_log_line(b);
}

/* Name an address as module plus offset.
 *
 * The allocation base of any address inside a loaded image is that image's base,
 * so one VirtualQuery answers both which module and how far into it, without a
 * module list and without the loader lock. */
static void where(void *a, char *out, int n)
{
	MEMORY_BASIC_INFORMATION mbi;
	char path[MAX_PATH];
	const char *leaf;

	if (!a || !VirtualQuery(a, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
		wsprintfA(out, "%08lX (no module)", (unsigned long)(ULONG_PTR)a);
		return;
	}
	path[0] = 0;
	GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path));
	leaf = path[0] ? strrchr(path, '\\') : NULL;
	wsprintfA(out, "%08lX (%s+0x%lX)", (unsigned long)(ULONG_PTR)a,
		  leaf ? leaf + 1 : (path[0] ? path : "?"),
		  (unsigned long)((char *)a - (char *)mbi.AllocationBase));
	(void)n;
}

/* Who asks for sound buffers, reported once per distinct call site.
 *
 * The question this answers is which code owns the audio, and it has resisted
 * being answered from the crash: the stack walk out of a faulting copy produced
 * five frames and all five were inside dsound.dll, so the owner was never named.
 * Creation is the better place to ask. Our hook sits in the device's vtable
 * slot, so the address it returns to is the caller of CreateSoundBuffer itself -
 * not a frame guessed from a stack scan, but the actual owner, recorded at the
 * one moment it is unambiguous. */
/* Is this return address inside our own DLL?
 *
 * dsh_save reads every buffer's cursor through the same vtable slot the game
 * uses, so without this the loudest caller of GetCurrentPosition would be us,
 * and the census would report the observer instead of the subject. */
static HMODULE g_self_mod;

static HMODULE self_mod(void)
{
	if (!g_self_mod)
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				   (LPCWSTR)(void *)self_mod, &g_self_mod);
	return g_self_mod;
}

/* One frame further out than our immediate caller, or nothing.
 *
 * frame_up((void **)__builtin_frame_address(0)) does this in a line and the compiler warns that
 * it is unsafe, correctly: it dereferences a frame pointer that a frameless or
 * optimised caller may not have set, and a wrong guess reads whatever happens
 * to be at that address. The walk is the same either way, so the difference
 * that matters is the checking, and that can be made exact rather than hopeful.
 *
 * A frame pointer must lie inside this thread's stack, must point higher than
 * the frame below it, and must leave room for the saved pointer and the return
 * address. The TEB knows the stack's bounds precisely, so all three are cheap
 * to test and nothing here can fault. A chain that fails any of them yields
 * null and the log says "no module" instead of naming an address we invented. */
static void *frame_up(void **fp)
{
	NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
	char *lo = (char *)tib->StackLimit;
	char *hi = (char *)tib->StackBase;
	void **caller;

	if (!fp || (char *)fp < lo || (char *)fp + 2 * sizeof(void *) > hi)
		return NULL;
	caller = (void **)fp[0];
	if ((char *)caller <= (char *)fp ||
	    (char *)caller + 2 * sizeof(void *) > hi)
		return NULL;
	return caller[1];
}

static int from_us(void *ra)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!ra || !VirtualQuery(ra, &mbi, sizeof(mbi)))
		return 0;
	return (HMODULE)mbi.AllocationBase == self_mod();
}

#define SITE_CREATE 0
#define SITE_CURSOR 1

#define DSH_OWNERS 24
typedef struct Site {
	void *ra;
	void *up; /* one frame further out, where the interesting code usually is */
	int kind;
	int ours; /* remembered so our own frames are queried once, not per call */
	int told;
	unsigned long hits;
} Site;

static Site g_site[DSH_OWNERS];
static int g_nowner;

/* Filled by the Lock watch further down, reported from here.
 *
 * The ring copy at rabiribi.exe+0x36340 clamps with cmova, an unsigned minimum,
 * so it cannot manufacture a negative count - it can only pass through one that
 * was already sitting in the ring's available-bytes field. That field is filled
 * by Lock. Restoring the play cursor did not stop the negative copies, so the
 * assumption that the count is computed from a cursor difference at the moment
 * of the copy is wrong somewhere, and Lock is the link in the chain that has
 * never been watched. */
static unsigned long g_lock_calls, g_lock_bad;
static DWORD g_lock_worst_req, g_lock_worst_r1, g_lock_worst_r2, g_lock_worst_off;
static void *g_lock_worst_ra;

/* Filled by the cursor probe further down, reported from here. */
static void *g_probe_obj;
static DWORD g_probe_sw, g_probe_hw;
static unsigned long g_probe_calls, g_probe_sw_moves, g_probe_hw_moves;

/* Recorded now, reported later.
 *
 * Every buffer in this game is made during startup, and the savestate log is not
 * open yet - which is why the install message was invisible for the whole life
 * of this file and was blamed on a format specifier. Writing at the moment of
 * discovery loses the discovery. So the address is kept and printed from
 * dsh_save, which demonstrably reaches the log. */
static void note_owner(void *ra, void *up, int kind)
{
	int i;

	if (!ra)
		return;
	/* The scan comes first because GetCurrentPosition is on the game's audio
	 * path and is called constantly. A known address costs a few compares;
	 * only a genuinely new one pays for a VirtualQuery.
	 *
	 * Keyed on both frames. The immediate one turned out to be a forwarding
	 * accessor that every caller shares, so keying on it alone collapsed
	 * every distinct caller into a single entry. */
	for (i = 0; i < g_nowner; i++)
		if (g_site[i].ra == ra && g_site[i].up == up && g_site[i].kind == kind) {
			g_site[i].hits++;
			return;
		}
	if (g_nowner >= DSH_OWNERS)
		return;
	g_site[g_nowner].ra = ra;
	g_site[g_nowner].up = up;
	g_site[g_nowner].kind = kind;
	g_site[g_nowner].ours = from_us(ra);
	g_site[g_nowner].told = 0;
	g_site[g_nowner].hits = 1;
	g_nowner++;
}

/* The install happens at startup too, so it is deferred for the same reason. */
static void **g_pending_dev_vtbl;
static int g_told_install;
static int g_told_flags;

static void report_owners(void)
{
	char buf[MAX_PATH + 64];
	int i;

	if (g_pending_dev_vtbl && !g_told_install) {
		g_told_install = 1;
		ss_log("dsound: device vtable at %08lX, CreateSoundBuffer and "
		       "DuplicateSoundBuffer watched; buffer vtable at %08lX, release "
		       "watched. %d buffer(s) tracked so far\n",
		       (unsigned long)(ULONG_PTR)g_pending_dev_vtbl,
		       (unsigned long)(ULONG_PTR)g_buf_vtbl, g_nbuf);
	}
	/* The two flags that choose the cursor's source, read from the image
	 * rather than hard-coded as absolute addresses, so a different base does
	 * not silently report someone else's memory. */
	{
		char *base = (char *)GetModuleHandleA(NULL);

		/* Rebasing was the only thing this guarded against, and the comment
		 * above says so. It is not enough: both offsets are five megabytes
		 * into an image, and read out of anything smaller they land past the
		 * end of it. A 363 KB harness faulted here on the first save, reading
		 * base+0x50E18C exactly. The flags are DxLib's, so the check is
		 * whether this is the game, not whether the pointer happens to be
		 * mapped - a lookalike that reads cleanly would be worse. */
		if (base && !savestate_host_is("rabiribi.exe")) {
			if (!g_told_flags) {
				g_told_flags = 1;
				ss_log("dsound: not reading the cursor source flags - they "
				       "are rabiribi.exe offsets and this is a different "
				       "executable\n");
			}
			base = NULL;
		}
		if (base) {
			DWORD hi = *(DWORD *)(base + 0x50E18C);
			DWORD lo = *(DWORD *)(base + 0x52EF18);

			ss_log("dsound: cursor source flags - +0x50E18C = %lu (nonzero "
			       "means skip DirectSound), +0x52EF18 = %lu (nonzero makes "
			       "the low-level wrapper decline). The cursor is %s\n",
			       hi, lo,
			       (hi || lo) ? "DxLib's own counter, which rewinds with the game"
					  : "the hardware's, which does not rewind");
		}
	}
	if (g_lock_calls) {
		char who[MAX_PATH + 64];

		where(g_lock_worst_ra, who, sizeof(who));
		ss_log("dsound: %lu Lock call(s), %lu of them asking for a byte count with "
		       "the top bit set. Worst: offset %lu, requested %ld, granted %lu + "
		       "%lu, from %s\n",
		       g_lock_calls, g_lock_bad, g_lock_worst_off, (long)g_lock_worst_req,
		       g_lock_worst_r1, g_lock_worst_r2, who);
		if (!g_lock_bad)
			ss_log("dsound: no Lock ever asked for a negative size, so the ring's "
			       "available count is not coming from the request - it is "
			       "being computed after the Lock returns\n");
	}
	if (g_probe_calls)
		ss_log("dsound: over %lu read(s) of one stream, DxLib's own counter "
		       "changed %lu time(s) and the hardware cursor changed %lu time(s). "
		       "If the first is zero the software source is dead in this mode "
		       "and the flag cannot simply be flipped; if both move, the counter "
		       "is maintained and rewinds with the game\n",
		       g_probe_calls, g_probe_sw_moves, g_probe_hw_moves);
	for (i = 0; i < g_nowner; i++) {
		if (g_site[i].told || g_site[i].ours)
			continue;
		g_site[i].told = 1;
		where(g_site[i].ra, buf, sizeof(buf));
		if (g_site[i].kind == SITE_CREATE)
			ss_log("dsound: sound buffers are created by %s - this is the "
			       "owner, the code whose idea of the play cursor has to "
			       "survive a restore\n",
			       buf);
		else {
			char up[MAX_PATH + 64];

			where(g_site[i].up, up, sizeof(up));
			ss_log("dsound: the play cursor is read at %s, called from %s, %lu "
			       "time(s) so far - the second address is the one that holds "
			       "the other operand of the subtraction\n",
			       buf, up, g_site[i].hits);
		}
	}
}

/* Patch one slot of a vtable that lives in read-only data. */
static void *slot_swap(void **vtbl, int i, void *fn)
{
	DWORD old;
	void *prev;

	if (!VirtualProtect(&vtbl[i], sizeof(void *), PAGE_READWRITE, &old))
		return NULL;
	prev = vtbl[i];
	vtbl[i] = fn;
	VirtualProtect(&vtbl[i], sizeof(void *), old, &old);
	return prev;
}

static void track(void *p, void *owner)
{
	int i;

	if (!p)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++)
		if (g_buf[i].p == p)
			goto out;
	if (g_nbuf < DSH_MAX) {
		g_buf[g_nbuf].p = p;
		g_buf[g_nbuf].held = 0;
		g_buf[g_nbuf].owner = owner;
		g_nbuf++;
	} else if (!g_capped) {
		g_capped = 1;
		ss_log("dsound: hit the %d buffer ceiling - every buffer past this one "
		       "keeps a play cursor we cannot wind back, so restores will still "
		       "see negative lengths\n",
		       DSH_MAX);
	}
out:
	LeaveCriticalSection(&g_cs);
}

static void untrack(void *p)
{
	int i;

	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++)
		if (g_buf[i].p == p) {
			g_buf[i] = g_buf[--g_nbuf];
			break;
		}
	LeaveCriticalSection(&g_cs);
}

/* Buffers are learned from the one call that makes them, so the list is exact
 * rather than discovered by scanning. Release is watched for the same reason:
 * a freed buffer is a pointer we must never call again, and asking a dead COM
 * object for its status is its own crash. */
static ULONG WINAPI hook_release(void *self)
{
	ULONG n = g_real_release(self);

	if (!n)
		untrack(self);
	return n;
}

/* Does DxLib's own cursor move while DirectSound is the one being asked?
 *
 * Both callers of the accessor have a second way to answer the question: if a
 * global is set, or if the low-level wrapper reports "not applicable", they skip
 * the hardware and compute the position as [obj+0x54] * [obj+0x3C] - a sample
 * counter and a block align, both living in the game's own object and therefore
 * both rewinding with it. A cursor sourced from there could not produce a
 * negative difference, because both operands would travel together.
 *
 * Whether that is usable turns on one fact we cannot get by reading code: does
 * [obj+0x54] advance while the hardware path is the one in use? If it does, the
 * counter is maintained regardless and the software source is live. If it sits
 * at whatever it was initialised to, the mode is chosen once at startup and
 * flipping the flag mid-run would freeze every stream instead of fixing it.
 *
 * So watch it. The object is not passed to us - we are handed the COM buffer -
 * but the forwarder's frame holds it as its first argument, and the object can
 * be confirmed rather than assumed: obj+0xC8 is where it keeps the very buffer
 * pointer we were called on. If that matches, we have the right object. */
#define DXSND_BUFPTR 0xC8
#define DXSND_SAMPLEPOS 0x54
#define DXSND_BLOCKALIGN 0x3C

static int obj_readable(void *o)
{
	MEMORY_BASIC_INFORMATION mbi;

	if (!o || !VirtualQuery(o, &mbi, sizeof(mbi)))
		return 0;
	if (mbi.State != MEM_COMMIT)
		return 0;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return 0;
	/* The whole object, not just its first byte. */
	return (char *)o + DXSND_BUFPTR + sizeof(void *) <=
	       (char *)mbi.BaseAddress + mbi.RegionSize;
}

static void probe_cursor(void **fp, void *self, DWORD play)
{
	void *obj;
	DWORD sw;

	if (!fp)
		return;
	obj = fp[2]; /* the forwarder's first argument */
	if (!obj_readable(obj))
		return;
	if (*(void **)((char *)obj + DXSND_BUFPTR) != self)
		return; /* not the object we thought; say nothing rather than guess */
	sw = *(DWORD *)((char *)obj + DXSND_SAMPLEPOS) *
	     (DWORD) * (WORD *)((char *)obj + DXSND_BLOCKALIGN);
	if (!g_probe_obj)
		g_probe_obj = obj;
	if (obj != g_probe_obj)
		return; /* one object, so the counts describe one stream */
	g_probe_calls++;
	if (sw != g_probe_sw) {
		g_probe_sw_moves++;
		g_probe_sw = sw;
	}
	if (play != g_probe_hw) {
		g_probe_hw_moves++;
		g_probe_hw = play;
	}
}

/* Who asks where the hardware is.
 *
 * The negative length is a subtraction: somebody reads the play cursor, compares
 * it against a position of their own, and copies the difference. The copier is
 * known - it is the CRT's memcpy, which tells us nothing about who computed the
 * count. This names the code doing the comparing, which is the code holding the
 * other operand, and that operand is the thing whose rewind produces a negative
 * result. Recorded per distinct call site rather than per call. */
static HRESULT WINAPI hook_getpos(void *self, DWORD *play, DWORD *write)
{
	/* Two frames, because the first one is a forwarder.
	 *
	 * rabiribi.exe+0x4E010 takes the object, reads the buffer pointer from
	 * +0xC8, calls this, and returns zero. Every caller in the game arrives
	 * through it, so the immediate return address is the same constant every
	 * time and says nothing about who wanted the cursor. The frame above it
	 * is the one doing arithmetic with the answer.
	 *
	 * Asking for frame 1 makes the compiler keep a frame pointer here, and
	 * the forwarder has an ordinary push ebp / mov ebp,esp prologue, so the
	 * chain is walkable. If it ever is not, this yields null and the log
	 * simply says no module rather than inventing an address. */
	void **fp = (void **)__builtin_frame_address(0);
	HRESULT hr;

	note_owner(__builtin_return_address(0), frame_up(fp), SITE_CURSOR);
	hr = g_real_getpos(self, play, write);
	if (SUCCEEDED(hr) && play && fp[0])
		probe_cursor((void **)fp[0], self, *play);
	return hr;
}

/* What the game asks Lock for, and what it gets.
 *
 * Three numbers matter and none of them have ever been seen: the size asked
 * for, and the two region sizes handed back. If the request already has its top
 * bit set then the negative is born upstream, in the cursor arithmetic at
 * +0x33CE4, and the ring is only carrying it. If the request is sane and the
 * ring still ends up negative, then something between Lock returning and the
 * copy running is corrupting the count - which would point at the restore
 * rather than at the audio. Those two answers need different fixes, and this
 * tells them apart. */
static HRESULT WINAPI hook_lock(void *self, DWORD off, DWORD bytes, void **p1, DWORD *b1,
				 void **p2, DWORD *b2, DWORD flags)
{
	HRESULT hr = g_real_lock(self, off, bytes, p1, b1, p2, b2, flags);

	g_lock_calls++;
	/* Worth recording the largest request as well as any negative one: a
	 * request of two billion is the same bug wearing a different sign. */
	if ((bytes & 0x80000000u) || bytes > g_lock_worst_req) {
		if (bytes & 0x80000000u)
			g_lock_bad++;
		g_lock_worst_req = bytes;
		g_lock_worst_off = off;
		g_lock_worst_r1 = (SUCCEEDED(hr) && b1) ? *b1 : 0;
		g_lock_worst_r2 = (SUCCEEDED(hr) && b2) ? *b2 : 0;
		g_lock_worst_ra = frame_up((void **)__builtin_frame_address(0));
	}
	return hr;
}

static void learn_buffer_vtbl(void *b)
{
	if (g_buf_vtbl || !b)
		return;
	g_buf_vtbl = *(void ***)b;
	g_real_release = (PFN_RELEASE)slot_swap(g_buf_vtbl, DSB_RELEASE, (void *)hook_release);
	g_real_getpos =
		(PFN_GETPOS)slot_swap(g_buf_vtbl, DSB_GET_POSITION, (void *)hook_getpos);
	g_real_lock = (PFN_LOCK)slot_swap(g_buf_vtbl, DSB_LOCK, (void *)hook_lock);
	ss_hook_note("vtable", "IDirectSoundBuffer::Release", g_buf_vtbl[DSB_RELEASE], 1);
	ss_hook_note("vtable", "IDirectSoundBuffer::GetCurrentPosition",
		     g_buf_vtbl[DSB_GET_POSITION], 1);
	ss_hook_note("vtable", "IDirectSoundBuffer::Lock", g_buf_vtbl[DSB_LOCK], 1);
}

static HRESULT WINAPI hook_create(void *self, const void *desc, void **out, void *unk)
{
	void *ra = __builtin_return_address(0);
	HRESULT hr = g_real_create(self, desc, out, unk);

	if (SUCCEEDED(hr) && out && *out) {
		learn_buffer_vtbl(*out);
		track(*out, ra);
		note_owner(ra, frame_up((void **)__builtin_frame_address(0)), SITE_CREATE);
	}
	return hr;
}

static HRESULT WINAPI hook_dup(void *self, void *src, void **out)
{
	void *ra = __builtin_return_address(0);
	HRESULT hr = g_real_dup(self, src, out);

	if (SUCCEEDED(hr) && out && *out) {
		learn_buffer_vtbl(*out);
		track(*out, ra);
		note_owner(ra, frame_up((void **)__builtin_frame_address(0)), SITE_CREATE);
	}
	return hr;
}

/* Learn the device vtable from a device of our own.
 *
 * Nothing about the throwaway is used except its type. It is created with the
 * default device and no cooperative level, which is enough to allocate the
 * object and therefore enough to read the pointer, and it is released
 * immediately. The game's device - created before or after this, by whatever
 * route - shares the table. */
void dsh_install(void)
{
	HMODULE ds;
	PFN_DSCREATE8 create8;
	void *dev = NULL;
	void **vtbl;

	if (g_armed)
		return;
	ds = GetModuleHandleA("dsound.dll");
	if (!ds)
		return; /* not loaded yet; called again next frame */
	create8 = (PFN_DSCREATE8)(void *)GetProcAddress(ds, "DirectSoundCreate8");
	if (!create8) {
		g_armed = 1; /* nothing to hook, and no point retrying every frame */
		return;
	}
	if (FAILED(create8(NULL, &dev, NULL)) || !dev) {
		g_armed = 1;
		ss_log("dsound: could not create a probe device, so the play cursor stays "
		       "the sound card's. Expect negative-length copies after a restore\n");
		return;
	}
	InitializeCriticalSection(&g_cs);
	vtbl = *(void ***)dev;
	g_real_create = (PFN_CREATEBUF)slot_swap(vtbl, DS_CREATE_BUFFER, (void *)hook_create);
	g_real_dup = (PFN_DUP)slot_swap(vtbl, DS_DUPLICATE, (void *)hook_dup);
	ss_hook_note("vtable", "IDirectSound::CreateSoundBuffer", vtbl[DS_CREATE_BUFFER], 1);
	ss_hook_note("vtable", "IDirectSound::DuplicateSoundBuffer", vtbl[DS_DUPLICATE], 1);
	((PFN_RELEASE)(*(void ***)dev)[2])(dev);
	g_armed = 1;
	g_ready = g_real_create != NULL;
	g_pending_dev_vtbl = vtbl;
}

/* Where every buffer is, at the instant the rest of the state is captured. */
void dsh_save(void)
{
	int i, n = 0;

	if (!g_ready)
		return;
	report_owners();
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v = *(void ***)g_buf[i].p;
		DWORD st = 0;

		g_buf[i].held = 0;
		/* The real method, not the slot: the slot is our own hook now, and
		 * routing the snapshot through it would file us as a caller. */
		if (FAILED((g_real_getpos ? g_real_getpos
					  : (PFN_GETPOS)v[DSB_GET_POSITION])(
			    g_buf[i].p, &g_buf[i].play, &g_buf[i].write)) ||
		    FAILED(((PFN_GETSTATUS)v[DSB_GET_STATUS])(g_buf[i].p, &st))) {
			/* A buffer that will not say where it is cannot be wound
			 * back, so it keeps a cursor in the present while its
			 * owner's write position goes back - exactly the state
			 * that produces a negative length. Name the owner: these
			 * are the buffers that matter. */
			char who[MAX_PATH + 64];

			where(g_buf[i].owner, who, sizeof(who));
			ss_log("dsound: buffer %08lX would not report its position, so it "
			       "cannot be wound back. Created by %s\n",
			       (unsigned long)(ULONG_PTR)g_buf[i].p, who);
			continue;
		}
		g_buf[i].status = st;
		g_buf[i].held = 1;
		n++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d of %d buffer(s) had their play position recorded\n", n, g_nbuf);
}

/* Silence for the duration of the copy.
 *
 * A save reported "14F3E088 CHANGED while we copied it", and in the same breath
 * that every thread in the process was suspended. So the writer was not a
 * thread. Suspending user-mode code does not stop a sound card: the mixer feeds
 * from a mapped buffer on its own schedule, and a driver completing a transfer
 * writes whether or not anything in this process is runnable. Deciding which
 * memory belongs to whom has no answer to that - the memory is the game's by any
 * definition and it still changes underneath us.
 *
 * What can be done is to remove the writer. A stopped buffer is not being read
 * by the card and not being filled by the driver, so for as long as everything
 * is stopped the copy has no competition. That is a smaller claim than owning
 * the audio stack and it is one we can actually make good on.
 *
 * The cost is honest and small: audio stops for the length of the snapshot. It
 * was going to glitch there anyway. */
void dsh_quiet(void)
{
	int i, n = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v = *(void ***)g_buf[i].p;

		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		n++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) stopped, so the card is not reading or filling "
	       "anything while we copy memory\n",
	       n);
}

/* Start again from where each buffer was when it was stopped.
 *
 * Only the ones that were playing at the snapshot: starting a buffer that was
 * idle would produce sound the game never asked for. */
void dsh_play(void)
{
	int i, n = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;

		if (!g_buf[i].held || !(g_buf[i].status & DSBSTATUS_PLAYING))
			continue;
		v = *(void ***)g_buf[i].p;
		((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play);
		((PFN_PLAY)v[DSB_PLAY])(g_buf[i].p, 0, 0, DSBPLAY_LOOPING);
		n++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) playing again from where they were stopped\n", n);
}

/* Move every cursor back, without starting anything.
 *
 * Split out of dsh_restore because the two halves want opposite sides of
 * resume_all and lumping them together got both wrong. Doing the whole thing
 * after the threads restart means stopping, seeking and starting 700 buffers
 * while the game is already reading those same cursors - a race we caused.
 * Doing the whole thing before means calling Play into dsound with every thread
 * suspended, which risks waiting on a lock a suspended thread holds.
 *
 * Seeking has neither problem. The buffers are already stopped, because
 * dsh_quiet stopped them at the top of the restore, so there is no mixer to
 * race and no work for dsound to do beyond writing a field. And once it is
 * done, a buffer that is asked where it is - by a thread that has only just
 * resumed - answers with the position the game expects rather than one from
 * several seconds in its future. That is the whole point, and it has to be
 * true from the first instruction the game executes, not from whenever we get
 * around to it. */
void dsh_seek(void)
{
	int i, moved = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;

		if (!g_buf[i].held)
			continue;
		v = *(void ***)g_buf[i].p;
		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		if (SUCCEEDED(((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play)))
			moved++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) wound back to their saved play position before the "
	       "threads restart, so the first cursor the game reads is the one it "
	       "expects\n",
	       moved);
}

/* Put the hardware back where the game believes it is.
 *
 * Stop before moving: SetCurrentPosition on a playing buffer is documented to
 * be honoured but races the mixer, and the whole point here is to remove a race
 * rather than to relocate it. A buffer that was playing is started again with
 * the looping flag it almost certainly had, because a streaming buffer that
 * stops looping goes silent at the end of its first pass and never recovers. */
void dsh_restore(void)
{
	int i, moved = 0;

	if (!g_ready)
		return;
	EnterCriticalSection(&g_cs);
	for (i = 0; i < g_nbuf; i++) {
		void **v;

		if (!g_buf[i].held)
			continue;
		v = *(void ***)g_buf[i].p;
		((PFN_STOP)v[DSB_STOP])(g_buf[i].p);
		if (FAILED(((PFN_SETPOS)v[DSB_SET_POSITION])(g_buf[i].p, g_buf[i].play)))
			continue;
		if (g_buf[i].status & DSBSTATUS_PLAYING)
			((PFN_PLAY)v[DSB_PLAY])(g_buf[i].p, 0, 0, DSBPLAY_LOOPING);
		moved++;
	}
	LeaveCriticalSection(&g_cs);
	ss_log("dsound: %d buffer(s) wound back to their saved play position, so the "
	       "game's write cursor is ahead of the hardware's again\n",
	       moved);
}



