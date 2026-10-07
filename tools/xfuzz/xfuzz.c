/* xfuzz - a stand-in game for the cross-launch restore.
 *
 * Every cross-launch crash so far, in Rabi-Ribi, Haydee and DDPR, was a
 * reference that crossed the line between memory the restore wrote back and
 * memory it left in the present: a pointer, a handle, a function address, a lock.
 * This program plants many such references of every kind it can think of, on a
 * layout disturbed by a seed, lets d3d9.dll save in one launch and restore in
 * the next, and then asks each reference whether it still means what it meant.
 *
 *   xfuzz.exe <seed> <shift>
 *
 * seed  - drives the junk allocations, extra DLLs and creation order, so the
 *         present side lands somewhere else in each launch
 * shift - 1 forces xfuzz_mod.dll to a different base than the OS would pick
 *
 * Save and load are driven by D3D9SW_SAVE_AT / D3D9SW_LOAD_AT; after a load
 * from another process (or a rewind in the same one) the verdicts go to
 * xfuzz_report.txt and xfuzz_detail.txt and the process ends. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <string.h>
#include <winternl.h>

enum {
	K_PROCHEAP,  /* block in the process heap */
	K_PRIVHEAP,  /* block in a HeapCreate heap */
	K_VA_OS,     /* VirtualAlloc placed by the OS */
	K_VA_FIXED,  /* VirtualAlloc at a fixed address - the control */
	K_FN_SYS,    /* function in a system DLL */
	K_FN_MOVED,  /* function in a DLL that can load elsewhere */
	K_DATA_MOVED,/* data in a DLL that can load elsewhere */
	K_EVENT,     /* kernel event handle */
	K_FILE,      /* file handle */
	K_THREAD,    /* thread handle and id */
	K_CS,        /* critical section contended before the save */
	K_TLS,       /* TLS slot value on the main thread */
	K_STACK,     /* pointer into a worker thread's stack */
	K_HWND,      /* window handle */
	K_BACKREF,   /* present-side holder pointing into restored memory */
	K_MAX
};

static const char *const g_kname[K_MAX] = {
	"proc-heap", "priv-heap", "va-os",   "va-fixed", "fn-sys",
	"fn-moved",  "data-moved", "event",  "file",     "thread",
	"lock",      "tls",       "stack",   "hwnd",     "backref",
};

enum { V_OK, V_GONE, V_CHANGED, V_STALE, V_OTHER, V_LOST, V_MAX };
static const char *const g_vname[V_MAX] = {"ok", "gone", "changed", "stale", "other", "lost"};

#define MAGIC 0x315A4658u /* "XFZ1" */
#define PER_KIND 4
#define NEDGE (K_MAX * PER_KIND)
#define FIXED_BASE 0x70000000u

typedef struct {
	DWORD magic, tag;
} Tagged;

typedef struct {
	DWORD magic;
	int kind;
	int home; /* 0 exe data, 1 OS-placed block */
	UINT_PTR v, aux;
	DWORD tag;
	char name[64];
} Edge;

typedef struct {
	DWORD magic;
	Tagged *target;
	DWORD tag;
} Holder;

/* Restored side: the exe's data, and one OS-placed block found through it. */
static Edge g_bss[NEDGE];
static Edge *g_blk;
static int g_nbss, g_nblk;
static DWORD g_owner_pid;
static DWORD g_seed_built;
static volatile long g_frame;
static HWND g_wnd, g_rwnd;
static CRITICAL_SECTION g_cs_store[PER_KIND];

static DWORD g_rng;
static DWORD rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static Edge *edge_new(int kind)
{
	Edge *e;

	if ((rnd() & 1) && g_blk && g_nblk < NEDGE)
		e = &g_blk[g_nblk++], e->home = 1;
	else
		e = &g_bss[g_nbss++], e->home = 0;
	e->magic = MAGIC;
	e->kind = kind;
	e->tag = rnd() | 1;
	return e;
}

static Tagged *tagged_at(void *p, DWORD tag)
{
	Tagged *t = (Tagged *)p;

	t->magic = MAGIC;
	t->tag = tag;
	return t;
}

static int readable(const void *p, SIZE_T n)
{
	MEMORY_BASIC_INFORMATION mbi;
	UINT_PTR a = (UINT_PTR)p;

	if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
		return 0;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
		return 0;
	return a + n <= (UINT_PTR)mbi.BaseAddress + mbi.RegionSize;
}

/* ---------------------------------------------------------------- objects */

typedef LONG(NTAPI *PFN_NtQueryObject)(HANDLE, int, PVOID, ULONG, PULONG);
static PFN_NtQueryObject p_nqo;

/* 1 if the handle names a live object of that type, -1 if it names something
 * else, 0 if it names nothing. */
static int handle_is(HANDLE h, const WCHAR *type)
{
	BYTE buf[512];
	UNICODE_STRING *u = (UNICODE_STRING *)buf;
	DWORD fl;

	if (!h || !GetHandleInformation(h, &fl))
		return 0;
	if (!p_nqo || p_nqo(h, 2, buf, sizeof(buf), NULL) < 0)
		return -1;
	if (u->Length / 2 == lstrlenW(type) && !memcmp(u->Buffer, type, u->Length))
		return 1;
	return -1;
}

static int handle_named(HANDLE h, const char *want)
{
	BYTE buf[1024];
	UNICODE_STRING *u = (UNICODE_STRING *)buf;
	char name[512];
	int n;

	if (!p_nqo || p_nqo(h, 1, buf, sizeof(buf), NULL) < 0 || !u->Buffer)
		return 0;
	n = WideCharToMultiByte(CP_ACP, 0, u->Buffer, u->Length / 2, name, sizeof(name) - 1, 0, 0);
	name[n < 0 ? 0 : n] = 0;
	return strstr(name, want) != NULL;
}

static DWORD WINAPI idle_main(LPVOID p)
{
	volatile Tagged local;

	tagged_at((void *)&local, (DWORD)(UINT_PTR)p);
	*(volatile Tagged **)p = &local; /* hand the address back */
	for (;;)
		Sleep(1000);
	return 0;
}

static DWORD WINAPI cs_hold_main(LPVOID p)
{
	EnterCriticalSection((CRITICAL_SECTION *)p);
	Sleep(30);
	LeaveCriticalSection((CRITICAL_SECTION *)p);
	return 0;
}

/* ---------------------------------------------------------------- build */

static const char *const g_extra_dlls[] = {
	"dbghelp.dll", "winhttp.dll", "crypt32.dll", "wininet.dll", "shell32.dll",
	"ole32.dll",   "ws2_32.dll",  "setupapi.dll", "imm32.dll",  "version.dll",
};

static HMODULE g_mod;
static int (*g_mod_fn)(int);
static volatile DWORD *g_mod_data;

static void perturb(void)
{
	int i, n = 8 + (int)(rnd() % 64);
	HANDLE ph = GetProcessHeap();

	for (i = 0; i < n; i++) {
		switch (rnd() % 4) {
		case 0: HeapAlloc(ph, 0, 16 + rnd() % 8192); break;
		case 1: VirtualAlloc(NULL, 4096 * (1 + rnd() % 64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); break;
		case 2: CreateEventA(NULL, FALSE, FALSE, NULL); break;
		case 3: LoadLibraryA(g_extra_dlls[rnd() % (sizeof(g_extra_dlls) / sizeof(g_extra_dlls[0]))]); break;
		}
	}
}

static void mod_load(int shift)
{
	g_mod = LoadLibraryA("xfuzz_mod.dll");
	if (g_mod && shift) {
		/* Wherever the OS put it, take that spot and load it again. */
		void *base = g_mod;

		FreeLibrary(g_mod);
		VirtualAlloc(base, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
		g_mod = LoadLibraryA("xfuzz_mod.dll");
	}
	if (g_mod) {
		g_mod_fn = (int (*)(int))GetProcAddress(g_mod, "xfm_fn");
		g_mod_data = (volatile DWORD *)GetProcAddress(g_mod, "xfm_data");
	}
}

static void fn_edge(Edge *e, HMODULE m, const char *mname, void *fn)
{
	e->v = (UINT_PTR)fn;
	e->aux = (UINT_PTR)fn - (UINT_PTR)m;
	lstrcpynA(e->name, mname, sizeof(e->name));
}

static void build_kind(int k, HANDLE priv, char *tmpdir)
{
	int i;

	for (i = 0; i < PER_KIND; i++) {
		Edge *e = edge_new(k);
		void *p = NULL;

		switch (k) {
		case K_PROCHEAP: p = HeapAlloc(GetProcessHeap(), 0, 64 + rnd() % 4096); break;
		case K_PRIVHEAP: p = HeapAlloc(priv, 0, 64 + rnd() % 4096); break;
		case K_VA_OS: p = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); break;
		case K_VA_FIXED:
			p = VirtualAlloc((void *)(UINT_PTR)(FIXED_BASE + i * 0x10000), 0x10000,
					 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			break;
		case K_FN_SYS: {
			static const char *const mods[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "ntdll.dll"};
			static const char *const fns[] = {"Sleep", "IsWindow", "GetStockObject", "RtlZeroMemory"};
			HMODULE m = GetModuleHandleA(mods[i]);

			fn_edge(e, m, mods[i], (void *)GetProcAddress(m, fns[i]));
			continue;
		}
		case K_FN_MOVED:
			fn_edge(e, g_mod, "xfuzz_mod.dll", (void *)g_mod_fn);
			continue;
		case K_DATA_MOVED:
			if (g_mod_data) {
				g_mod_data[i * 2] = MAGIC;
				g_mod_data[i * 2 + 1] = e->tag;
				p = (void *)&g_mod_data[i * 2];
			}
			e->v = (UINT_PTR)p;
			continue;
		case K_EVENT:
			wsprintfA(e->name, "xfuzz_ev_%lu_%d", GetCurrentProcessId(), i);
			e->v = (UINT_PTR)CreateEventA(NULL, TRUE, TRUE, e->name);
			continue;
		case K_FILE:
			wsprintfA(e->name, "xfuzz_%lu_%d.tmp", GetCurrentProcessId(), i);
			{
				char path[MAX_PATH];

				wsprintfA(path, "%s%s", tmpdir, e->name);
				e->v = (UINT_PTR)CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
							     CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
			}
			continue;
		case K_THREAD: {
			volatile Tagged *where = NULL;
			HANDLE t = CreateThread(NULL, 0, idle_main, (LPVOID)&where, 0, NULL);

			while (!where)
				Sleep(1);
			e->v = (UINT_PTR)t;
			e->aux = GetThreadId(t);
			continue;
		}
		case K_CS: {
			CRITICAL_SECTION *cs = &g_cs_store[i];
			HANDLE t;

			InitializeCriticalSection(cs);
			t = CreateThread(NULL, 0, cs_hold_main, cs, 0, NULL);
			Sleep(5);
			EnterCriticalSection(cs); /* waits on the holder: LockSemaphore gets made */
			LeaveCriticalSection(cs);
			WaitForSingleObject(t, INFINITE);
			CloseHandle(t);
			e->v = (UINT_PTR)cs;
			e->aux = (UINT_PTR)cs->LockSemaphore;
			continue;
		}
		case K_TLS:
			e->aux = TlsAlloc();
			TlsSetValue((DWORD)e->aux, (LPVOID)(UINT_PTR)e->tag);
			continue;
		case K_STACK: {
			volatile Tagged *where = NULL;

			CloseHandle(CreateThread(NULL, 0, idle_main, (LPVOID)&where, 0, NULL));
			while (!where)
				Sleep(1);
			e->v = (UINT_PTR)where;
			e->tag = where->tag;
			continue;
		}
		case K_HWND:
			e->v = (UINT_PTR)g_wnd;
			continue;
		case K_BACKREF: {
			/* Restored side: the target, in the private heap. Present side:
			 * a holder in the process heap pointing at it. */
			Tagged *t = tagged_at(HeapAlloc(priv, 0, 64), e->tag);
			Holder *h = (Holder *)HeapAlloc(GetProcessHeap(), 0, sizeof(Holder));

			h->magic = MAGIC;
			h->target = t;
			h->tag = e->tag;
			e->v = (UINT_PTR)h;
			e->aux = (UINT_PTR)t;
			continue;
		}
		}
		if (p)
			tagged_at(p, e->tag);
		e->v = (UINT_PTR)p;
	}
}

static void build(DWORD seed, int shift)
{
	int order[K_MAX], i;
	HANDLE priv;
	char tmpdir[MAX_PATH];

	g_rng = seed * 2654435761u + 1;
	perturb();
	mod_load(shift);
	perturb();
	priv = HeapCreate(0, 0, 0);
	g_blk = (Edge *)VirtualAlloc(NULL, sizeof(Edge) * NEDGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	GetTempPathA(sizeof(tmpdir), tmpdir);
	for (i = 0; i < K_MAX; i++)
		order[i] = i;
	for (i = K_MAX - 1; i > 0; i--) {
		int j = (int)(rnd() % (DWORD)(i + 1)), t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (i = 0; i < K_MAX; i++) {
		build_kind(order[i], priv, tmpdir);
		if (rnd() & 1)
			perturb();
	}
	g_owner_pid = GetCurrentProcessId();
	g_seed_built = seed;
}

/* ---------------------------------------------------------------- verify */

static int verify_one(Edge *e)
{
	switch (e->kind) {
	case K_PROCHEAP: case K_PRIVHEAP: case K_VA_OS: case K_VA_FIXED: case K_DATA_MOVED: case K_STACK: {
		Tagged *t = (Tagged *)e->v;

		if (!readable(t, sizeof(*t)))
			return V_GONE;
		return t->magic == MAGIC && t->tag == e->tag ? V_OK : V_CHANGED;
	}
	case K_FN_SYS: case K_FN_MOVED: {
		HMODULE m = NULL;
		char path[MAX_PATH], *b;

		if (!readable((void *)e->v, 1))
			return V_GONE;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCSTR)e->v, &m) || !m)
			return V_CHANGED;
		GetModuleFileNameA(m, path, sizeof(path));
		b = strrchr(path, '\\');
		b = b ? b + 1 : path;
		if (lstrcmpiA(b, e->name))
			return V_OTHER;
		return e->v - (UINT_PTR)m == e->aux ? V_OK : V_CHANGED;
	}
	case K_EVENT: {
		int r = handle_is((HANDLE)e->v, L"Event");

		if (!r)
			return V_STALE;
		return r > 0 && handle_named((HANDLE)e->v, e->name) ? V_OK : V_OTHER;
	}
	case K_FILE: {
		int r = handle_is((HANDLE)e->v, L"File");

		if (!r)
			return V_STALE;
		return r > 0 && handle_named((HANDLE)e->v, e->name) ? V_OK : V_OTHER;
	}
	case K_THREAD: {
		int r = handle_is((HANDLE)e->v, L"Thread");

		if (!r)
			return V_STALE;
		return r > 0 && GetThreadId((HANDLE)e->v) == (DWORD)e->aux ? V_OK : V_OTHER;
	}
	case K_CS: {
		CRITICAL_SECTION *cs = (CRITICAL_SECTION *)e->v;
		int r;

		if (!readable(cs, sizeof(*cs)))
			return V_GONE;
		/* NULL: ntdll makes a fresh one. -1: waits go to the keyed event,
		 * which is what current Windows does for every contended section. */
		if (!cs->LockSemaphore || cs->LockSemaphore == INVALID_HANDLE_VALUE)
			return V_OK;
		r = handle_is(cs->LockSemaphore, L"Event");
		if (!r)
			return V_STALE;
		return r > 0 ? V_OK : V_OTHER;
	}
	case K_TLS:
		return TlsGetValue((DWORD)e->aux) == (LPVOID)(UINT_PTR)e->tag ? V_OK : V_CHANGED;
	case K_HWND: {
		DWORD pid = 0;

		if (!IsWindow((HWND)e->v))
			return V_STALE;
		GetWindowThreadProcessId((HWND)e->v, &pid);
		return pid == GetCurrentProcessId() ? V_OK : V_OTHER;
	}
	}
	return V_OK;
}

/* The holder is present-side, so it is this launch's; its target was in
 * restored memory and now holds whatever the save had there. */
static int verify_backref(Holder *h)
{
	if (!readable(h, sizeof(*h)) || h->magic != MAGIC)
		return V_LOST;
	if (!readable(h->target, sizeof(Tagged)))
		return V_GONE;
	return h->target->magic == MAGIC && h->target->tag == h->tag ? V_OK : V_CHANGED;
}

static Holder *g_live_holders[PER_KIND];

/* The report runs straight after a restore, so it stays off the C runtime: its
 * stream locks are exactly the kind of thing a restore can leave broken. */
static HANDLE out_open(const char *path)
{
	HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);

	return h == INVALID_HANDLE_VALUE ? NULL : h;
}

static void outf(HANDLE h, const char *fmt, ...)
{
	char b[1024];
	va_list a;
	DWORD w;
	int n;

	va_start(a, fmt);
	n = wvsprintfA(b, fmt, a);
	va_end(a);
	if (h && n > 0)
		WriteFile(h, b, (DWORD)n, &w, NULL);
}

static const char *parse_num(const char *c, DWORD *v)
{
	while (*c == ' ')
		c++;
	*v = 0;
	while (*c >= '0' && *c <= '9')
		*v = *v * 10 + (DWORD)(*c++ - '0');
	return c;
}

static void report(const char *why, DWORD seed, int shift)
{
	int tally[K_MAX][V_MAX];
	HANDLE rf = out_open("xfuzz_report.txt"), df = out_open("xfuzz_detail.txt");
	int i, k, v, lost = 0;
	int blk_ok = readable(g_blk, sizeof(Edge) * NEDGE);

	memset(tally, 0, sizeof(tally));
	if (df)
		outf(df, "== %s: built by pid %lu seed %lu, checked in pid %lu seed %lu shift %d\n", why,
			g_owner_pid, g_seed_built, GetCurrentProcessId(), seed, shift);
	for (i = 0; i < g_nbss + g_nblk; i++) {
		Edge *e = i < g_nbss ? &g_bss[i] : (blk_ok ? &g_blk[i - g_nbss] : NULL);
		int r;

		if (!e || e->magic != MAGIC) {
			lost++;
			if (df)
				outf(df, "  edge %d in the OS block: record itself lost\n", i);
			continue;
		}
		if (e->kind == K_BACKREF)
			r = V_OK; /* the restored copy of a holder pointer says nothing */
		else
			r = verify_one(e);
		if (e->kind != K_BACKREF)
			tally[e->kind][r]++;
		if (r != V_OK && df)
			outf(df, "  %-10s home %s  value %08lX aux %08lX -> %s\n", g_kname[e->kind],
				e->home ? "os-block" : "exe-data", (unsigned long)e->v, (unsigned long)e->aux,
				g_vname[r]);
	}
	for (i = 0; i < PER_KIND; i++) {
		int r = g_live_holders[i] ? verify_backref(g_live_holders[i]) : V_LOST;

		tally[K_BACKREF][r]++;
		if (r != V_OK && df)
			outf(df, "  %-10s holder %08lX -> %s\n", g_kname[K_BACKREF],
				(unsigned long)(UINT_PTR)g_live_holders[i], g_vname[r]);
	}
	if (rf) {
		outf(rf, "%s seed %lu->%lu shift %d os-block %s, %d record(s) lost |", why,
			g_seed_built, seed, shift, blk_ok ? "kept" : "LOST", lost);
		for (k = 0; k < K_MAX; k++) {
			int bad = 0;

			for (v = 1; v < V_MAX; v++)
				bad += tally[k][v];
			if (!bad) {
				outf(rf, " %s:ok", g_kname[k]);
				continue;
			}
			outf(rf, " %s:", g_kname[k]);
			for (v = 1; v < V_MAX; v++)
				if (tally[k][v])
					outf(rf, "%s%d", g_vname[v], tally[k][v]);
		}
		outf(rf, "\n");
		CloseHandle(rf);
	}
	if (df)
		CloseHandle(df);
}

/* ---------------------------------------------------------------- main */

/* This launch's window. g_wnd is restored with everything else, so after a load
 * from another launch it names the old one. */
static HWND present_wnd(void)
{
	HWND w = NULL;
	DWORD pid;

	while ((w = FindWindowExA(NULL, w, "xfuzz", NULL)) != NULL) {
		GetWindowThreadProcessId(w, &pid);
		if (pid == GetCurrentProcessId())
			return w;
	}
	return NULL;
}

/* This launch's render window: the device draws into a window of its own, as
 * DDPR's does - a child of the main one or a separate popup, by seed. */
static HWND present_rwnd(void)
{
	HWND m = present_wnd(), w = NULL;
	DWORD pid;

	if (m && (w = FindWindowExA(m, NULL, "xfuzz_render", NULL)) != NULL)
		return w;
	while ((w = FindWindowExA(NULL, w, "xfuzz_render", NULL)) != NULL) {
		GetWindowThreadProcessId(w, &pid);
		if (pid == GetCurrentProcessId())
			return w;
	}
	return NULL;
}

#define PROBE_RGB 0xFF00FFu

/* Did the frame just presented reach the window this launch shows? The
 * renderer blits with GDI, so the window's own DC holds the last frame. */
static void render_check(void)
{
	HWND w = present_rwnd();
	HANDLE f = out_open("xfuzz_render.txt");
	RECT rc;
	COLORREF c = CLR_INVALID;
	HDC dc;

	if (w && GetClientRect(w, &rc) && (dc = GetDC(w)) != NULL) {
		c = GetPixel(dc, rc.right / 2, rc.bottom / 2);
		ReleaseDC(w, dc);
	}
	/* COLORREF is 0x00BBGGRR; the probe is symmetric in R and B. */
	if (c == PROBE_RGB)
		outf(f, "render: ok\r\n");
	else
		outf(f, "render: STALE - live render window %p shows %06lX, not the probe\r\n",
		     (void *)w, (unsigned long)c);
	if (f)
		CloseHandle(f);
}

static void present_args(DWORD *seed, int *shift)
{
	const char *c = GetCommandLineA();

	if (*c == '"')
		c = strchr(c + 1, '"') ? strchr(c + 1, '"') + 1 : c;
	else
		while (*c && *c != ' ')
			c++;
	{
		DWORD s;

		c = parse_num(c, seed);
		parse_num(c, &s);
		*shift = (int)s;
	}
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	if (m == WM_DESTROY)
		PostQuitMessage(0);
	return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	WNDCLASSA wc;
	IDirect3D9 *d3d;
	IDirect3DDevice9 *dev = NULL;
	D3DPRESENT_PARAMETERS pp;
	DWORD seed = 1;
	int shift = 0, i;
	MSG msg;

	(void)prev, (void)show;
	(void)cmd;
	/* Unattended: a crash ends the process instead of waiting on a dialog. */
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	present_args(&seed, &shift);
	p_nqo = (PFN_NtQueryObject)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryObject");

	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.lpszClassName = "xfuzz";
	RegisterClassA(&wc);
	wc.lpszClassName = "xfuzz_render";
	RegisterClassA(&wc);
	g_wnd = CreateWindowA("xfuzz", "xfuzz", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 336, 278, 0, 0,
			      inst, 0);
	if (seed & 1)
		g_rwnd = CreateWindowA("xfuzz_render", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 320, 240,
				       g_wnd, 0, inst, 0);
	else
		g_rwnd = CreateWindowA("xfuzz_render", "xfuzz render", WS_POPUP | WS_VISIBLE, 400, 40,
				       320, 240, g_wnd, 0, inst, 0);

	d3d = Direct3DCreate9(D3D_SDK_VERSION);
	memset(&pp, 0, sizeof(pp));
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferWidth = 320;
	pp.BackBufferHeight = 240;
	pp.hDeviceWindow = g_rwnd;
	if (!d3d || d3d->lpVtbl->CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd,
					      D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev) < 0)
		return 2;

	build(seed, shift);
	/* This launch's present-side holders, kept where a restore cannot reach:
	 * the restored copy of g_live_holders would be the saving launch's. */
	{
		Holder **keep = (Holder **)HeapAlloc(GetProcessHeap(), 0, sizeof(g_live_holders));
		int n = 0;

		for (i = 0; i < g_nbss + g_nblk; i++) {
			Edge *e = i < g_nbss ? &g_bss[i] : &g_blk[i - g_nbss];

			if (e->kind == K_BACKREF && n < PER_KIND)
				keep[n++] = (Holder *)e->v;
		}
		SetPropA(g_wnd, "xfuzz_keep", keep);
		SetPropA(g_wnd, "xfuzz_frame", (HANDLE)0);
	}

	for (;;) {
		/* Keeps running after the report so a save taken after the restore
		 * (D3D9SW_SAVE_AFTER_LOAD) gets its turn; QUIT_AT ends it. */
		static int reported, probed;

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				return 0;
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET,
				   reported ? (D3DCOLOR)(0xFF000000u | PROBE_RGB)
					    : D3DCOLOR_XRGB((g_frame * 3) & 255, 40, 80),
				   1.0f, 0);
		dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
		g_frame++;
		if (reported && ++probed == 3)
			render_check();

		/* The window's properties live in win32k, which no restore touches,
		 * so they are this launch's clock and this launch's holders. */
		{
			HWND w = present_wnd();
			long seen = (long)(LONG_PTR)GetPropA(w, "xfuzz_frame");
			int other = g_owner_pid != GetCurrentProcessId();
			int rewound = seen > g_frame + 1;

			if ((other || rewound) && !reported) {
				Holder **keep = (Holder **)GetPropA(w, "xfuzz_keep");

				reported = 1;
				for (i = 0; i < PER_KIND; i++)
					g_live_holders[i] = keep ? keep[i] : NULL;
				present_args(&seed, &shift);
				report(other ? "CROSS" : "SAME ", seed, shift);
				/* LOAD_AT's own counter rewinds in a same-launch restore. */
				if (!other)
					TerminateProcess(GetCurrentProcess(), 0);
			}
			SetPropA(w, "xfuzz_frame", (HANDLE)(LONG_PTR)g_frame);
		}
	}
}
