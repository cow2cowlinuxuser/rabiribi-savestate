#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include "windows.h"
#include "host.h"

static DWORD s_last_error;
static HWND s_hwnd = (HWND)1;

static void *heap_alloc(DWORD bytes)
{
	void *p = memalign(32, bytes ? bytes : 4);
	if (p)
		memset(p, 0, bytes);
	return p;
}

HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
		     int x, int y, int w, int h, HWND parent, HMENU menu,
		     HINSTANCE inst, LPVOID param)
{
	(void)ex; (void)cls; (void)style; (void)x; (void)y; (void)w; (void)h;
	(void)parent; (void)menu; (void)inst; (void)param;
	host_log("CreateWindowExA %s", title ? title : "");
	return s_hwnd;
}

LRESULT DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l)
{
	(void)h; (void)m; (void)w; (void)l;
	return 0;
}

BOOL DestroyWindow(HWND h)
{
	(void)h;
	host_log("DestroyWindow");
	return TRUE;
}

BOOL PeekMessageA(LPMSG msg, HWND h, UINT min, UINT max, UINT remove)
{
	(void)h; (void)min; (void)max; (void)remove;
	if (msg)
		memset(msg, 0, sizeof(*msg));
	return FALSE;
}

BOOL TranslateMessage(const MSG *msg) { (void)msg; return TRUE; }
LRESULT DispatchMessageA(const MSG *msg) { (void)msg; return 0; }
void PostQuitMessage(int code) { (void)code; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type)
{
	(void)h;
	(void)type;
	host_log("MSG %s: %s", cap ? cap : "", text ? text : "");
	host_trap(text ? text : "MessageBoxA");
	return 1;
}

BOOL GetClientRect(HWND h, LPRECT r)
{
	(void)h;
	if (!r)
		return FALSE;
	r->left = 0;
	r->top = 0;
	r->right = 640;
	r->bottom = 480;
	return TRUE;
}

BOOL GetWindowRect(HWND h, LPRECT r)
{
	return GetClientRect(h, r);
}

HWND GetDesktopWindow(void) { return s_hwnd; }

HWND GetActiveWindow(void) { return s_hwnd; }

int GetSystemMetrics(int idx)
{
	if (idx == SM_CXSCREEN)
		return 640;
	if (idx == SM_CYSCREEN)
		return 480;
	return 0;
}

BOOL SetRect(LPRECT r, int l, int t, int ri, int b)
{
	if (!r)
		return FALSE;
	r->left = l;
	r->top = t;
	r->right = ri;
	r->bottom = b;
	return TRUE;
}

BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu)
{
	(void)style;
	(void)menu;
	(void)r;
	return TRUE;
}

LONG SetWindowLongA(HWND h, int idx, LONG val)
{
	(void)h;
	(void)idx;
	return val;
}

BOOL MoveWindow(HWND h, int x, int y, int w, int ht, BOOL repaint)
{
	(void)h; (void)x; (void)y; (void)w; (void)ht; (void)repaint;
	return TRUE;
}

int ShowCursor(BOOL show)
{
	static int count;

	if (show)
		count++;
	else
		count--;
	return count;
}

BOOL ShowWindow(HWND h, int cmd)
{
	(void)h;
	(void)cmd;
	return TRUE;
}

HDC GetDC(HWND h)
{
	(void)h;
	return (HDC)2;
}

int ReleaseDC(HWND h, HDC dc)
{
	(void)h;
	(void)dc;
	return 1;
}

HICON LoadIconA(HINSTANCE i, LPCSTR name)
{
	(void)i;
	(void)name;
	return (HICON)3;
}

LRESULT SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l)
{
	(void)h;
	(void)msg;
	(void)w;
	(void)l;
	return 0;
}

HIMC ImmGetContext(HWND h)
{
	(void)h;
	return NULL;
}

BOOL ImmSetOpenStatus(HIMC imc, BOOL open)
{
	(void)imc;
	(void)open;
	return TRUE;
}

HIMC ImmAssociateContext(HWND h, HIMC imc)
{
	(void)h;
	return imc;
}

HWND ImmGetDefaultIMEWnd(HWND h)
{
	(void)h;
	return NULL;
}

BOOL ImmReleaseContext(HWND h, HIMC imc)
{
	(void)h;
	(void)imc;
	return TRUE;
}

ATOM RegisterClassA(const void *wc)
{
	(void)wc;
	return 1;
}

int wsprintfA(LPSTR buf, LPCSTR fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, 512, fmt, ap);
	va_end(ap);
	return n;
}

#define HOST_NFILES 16
#define HOST_FH_BASE 0x100u

static FILE *s_files[HOST_NFILES];
static const char *s_vol = "sdc";

void host_set_sd_vol(const char *vol)
{
	if (vol && vol[0])
		s_vol = vol;
}

const char *host_sd_vol(void)
{
	return s_vol;
}

static void host_norm_pc_path(char *out, size_t n, const char *name)
{
	const char *s = name ? name : "";
	char tmp[300];
	size_t i = 0;

	if (s[0] == '.' && (s[1] == '\\' || s[1] == '/'))
		s += 2;
	while (*s && i + 1 < sizeof(tmp)) {
		tmp[i++] = (*s == '\\') ? '/' : *s;
		s++;
	}
	tmp[i] = 0;
	if (!strncmp(tmp, "_LocalConfig", 12))
		snprintf(out, n, "RBO/LocalConfig%s", tmp + 12);
	else if (!strncmp(tmp, "Data/", 5) || !strncmp(tmp, "data/", 5))
		snprintf(out, n, "RBO/DATA/%s", tmp + 5);
	else if (!strncmp(tmp, "RBO/", 4) || !strncmp(tmp, "rbo/", 4))
		snprintf(out, n, "%s", tmp);
	else
		snprintf(out, n, "RBO/%s", tmp);
}

static FILE *host_fopen_pc(const char *name, const char *mode)
{
	char rel[300];
	char full[320];
	FILE *fp;

	host_norm_pc_path(rel, sizeof(rel), name);
	host_log("FILE %s", rel);
	/* Do not present here. After D3D Flip is live, overlay_pump wedges
	 * GX. WinMain wm_step and IMG progress still Flip before that. */
	host_pump_escape();
	snprintf(full, sizeof(full), "%s:/%s", s_vol, rel);
	fp = fopen(full, mode);
	return fp;
}

static FILE *host_fp(HANDLE h)
{
	unsigned i;

	i = (unsigned)(uintptr_t)h - HOST_FH_BASE;
	if (i >= HOST_NFILES)
		return NULL;
	return s_files[i];
}

HANDLE CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPVOID sa,
		   DWORD disp, DWORD flags, HANDLE tmpl)
{
	FILE *fp;
	const char *mode;
	unsigned i;

	(void)share;
	(void)sa;
	(void)flags;
	(void)tmpl;
	mode = (acc & 0x40000000u) ? "wb+" : "rb";
	if (disp == 3)
		mode = "rb";
	fp = host_fopen_pc(name, mode);
	if (!fp) {
		s_last_error = ERROR_FILE_NOT_FOUND;
		return INVALID_HANDLE_VALUE;
	}
	for (i = 0; i < HOST_NFILES; i++) {
		if (!s_files[i]) {
			s_files[i] = fp;
			return (HANDLE)(HOST_FH_BASE + i);
		}
	}
	fclose(fp);
	s_last_error = ERROR_FILE_NOT_FOUND;
	return INVALID_HANDLE_VALUE;
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *out, LPOVERLAPPED ov)
{
	FILE *fp = host_fp(h);
	size_t got = 0;

	(void)ov;
	if (!fp || !buf) {
		if (out)
			*out = 0;
		return FALSE;
	}
	got = fread(buf, 1, n, fp);
	if (out)
		*out = (DWORD)got;
	return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *out, LPOVERLAPPED ov)
{
	FILE *fp = host_fp(h);
	size_t got = 0;

	(void)ov;
	if (!fp || !buf) {
		if (out)
			*out = 0;
		return FALSE;
	}
	got = fwrite(buf, 1, n, fp);
	if (out)
		*out = (DWORD)got;
	return TRUE;
}

BOOL CloseHandle(HANDLE h)
{
	unsigned i;

	i = (unsigned)(uintptr_t)h - HOST_FH_BASE;
	if (i >= HOST_NFILES || !s_files[i])
		return TRUE;
	fclose(s_files[i]);
	s_files[i] = NULL;
	return TRUE;
}

DWORD GetFileSize(HANDLE h, DWORD *high)
{
	FILE *fp = host_fp(h);
	long pos, end;

	if (high)
		*high = 0;
	if (!fp)
		return 0;
	pos = ftell(fp);
	fseek(fp, 0, SEEK_END);
	end = ftell(fp);
	fseek(fp, pos, SEEK_SET);
	return end > 0 ? (DWORD)end : 0;
}

DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method)
{
	FILE *fp = host_fp(h);
	int whence;
	long pos;

	if (high)
		*high = 0;
	if (!fp)
		return 0xffffffffu;
	if (method == 0)
		whence = SEEK_SET;
	else if (method == 1)
		whence = SEEK_CUR;
	else if (method == 2)
		whence = SEEK_END;
	else
		return 0xffffffffu;
	if (fseek(fp, dist, whence) != 0)
		return 0xffffffffu;
	pos = ftell(fp);
	if (pos < 0)
		return 0xffffffffu;
	return (DWORD)pos;
}

HANDLE GetProcessHeap(void) { return (HANDLE)4; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, DWORD bytes)
{
	(void)heap;
	(void)flags;
	return heap_alloc(bytes);
}

LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, DWORD bytes)
{
	LPVOID n;

	(void)heap;
	(void)flags;
	n = heap_alloc(bytes);
	if (n && p)
		memcpy(n, p, bytes);
	free(p);
	return n;
}

BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID p)
{
	(void)heap;
	(void)flags;
	free(p);
	return TRUE;
}

DWORD HeapSize(HANDLE heap, DWORD flags, LPCVOID p)
{
	(void)heap;
	(void)flags;
	(void)p;
	return 0;
}

HANDLE HeapCreate(DWORD f, DWORD init, DWORD max)
{
	(void)f;
	(void)init;
	(void)max;
	return GetProcessHeap();
}

BOOL HeapDestroy(HANDLE h)
{
	(void)h;
	return TRUE;
}

LPVOID VirtualAlloc(LPVOID a, DWORD s, DWORD t, DWORD p)
{
	(void)a;
	(void)t;
	(void)p;
	return heap_alloc(s);
}

BOOL VirtualFree(LPVOID a, DWORD s, DWORD t)
{
	(void)s;
	(void)t;
	free(a);
	return TRUE;
}

HGLOBAL GlobalAlloc(UINT flags, DWORD bytes)
{
	(void)flags;
	return (HGLOBAL)heap_alloc(bytes);
}

HGLOBAL GlobalFree(HGLOBAL h)
{
	free(h);
	return NULL;
}

DWORD GetTickCount(void) { return host_tick_ms(); }

void Sleep(DWORD ms)
{
	if (ms)
		usleep(ms * 1000);
}

void GetLocalTime(LPSYSTEMTIME st)
{
	memset(st, 0, sizeof(*st));
	st->wYear = 2006;
}

void GetSystemTime(LPSYSTEMTIME st) { GetLocalTime(st); }

void GetSystemTimeAsFileTime(LPFILETIME ft)
{
	memset(ft, 0, sizeof(*ft));
}

BOOL QueryPerformanceCounter(void *out)
{
	if (out)
		*(u64 *)out = ticks_to_microsecs(gettime());
	return TRUE;
}

BOOL QueryPerformanceFrequency(void *out)
{
	if (out)
		*(u64 *)out = 1000000ull;
	return TRUE;
}

void EnterCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }
void LeaveCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }
void InitializeCriticalSection(LPCRITICAL_SECTION cs) { memset(cs, 0, sizeof(*cs)); }
void DeleteCriticalSection(LPCRITICAL_SECTION cs) { (void)cs; }

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, DWORD stack,
		    LPTHREAD_START_ROUTINE start, LPVOID arg,
		    DWORD flags, DWORD *id)
{
	(void)sa; (void)stack; (void)start; (void)arg; (void)flags; (void)id;
	host_trap("CreateThread");
	return NULL;
}

DWORD GetLastError(void) { return s_last_error; }
void SetLastError(DWORD e) { s_last_error = e; }
DWORD GetCurrentProcess(void) { return 1; }
BOOL TerminateProcess(HANDLE h, UINT code)
{
	(void)h;
	(void)code;
	return TRUE;
}
void ExitProcess(UINT code)
{
	(void)code;
	host_trap("ExitProcess");
}

DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n)
{
	(void)m;
	snprintf(buf, n, "sdc:/RBO/RBO_EX3.DOL");
	return (DWORD)strlen(buf);
}

HMODULE GetModuleHandleA(LPCSTR n)
{
	(void)n;
	return (HMODULE)5;
}

FARPROC GetProcAddress(HMODULE m, LPCSTR n)
{
	(void)m;
	host_log("GetProcAddress %s", n ? n : "");
	host_trap(n ? n : "GetProcAddress");
	return NULL;
}

HMODULE LoadLibraryA(LPCSTR n)
{
	host_log("LoadLibraryA %s", n ? n : "");
	return (HMODULE)6;
}

BOOL PathFileExistsA(LPCSTR path)
{
	FILE *fp = host_fopen_pc(path, "rb");

	if (!fp)
		return FALSE;
	fclose(fp);
	return TRUE;
}

LPSTR PathCombineA(LPSTR out, LPCSTR a, LPCSTR b)
{
	if (!out)
		return NULL;
	snprintf(out, MAX_PATH, "%s/%s", a ? a : "", b ? b : "");
	return out;
}

BOOL PathRenameExtensionA(LPSTR path, LPCSTR ext)
{
	char *dot;

	if (!path)
		return FALSE;
	dot = strrchr(path, '.');
	if (dot)
		*dot = 0;
	if (ext)
		strcat(path, ext);
	return TRUE;
}

DWORD timeGetTime(void) { return host_tick_ms(); }
UINT timeBeginPeriod(UINT p) { (void)p; return 0; }

HRESULT CoInitialize(LPVOID reserved)
{
	(void)reserved;
	host_log("CoInitialize");
	return S_OK;
}

void CoUninitialize(void) { host_log("CoUninitialize"); }

HRESULT CoCreateInstance(REFCLSID clsid, LPVOID unk, DWORD ctx, REFIID iid,
			 LPVOID *ppv)
{
	(void)clsid; (void)unk; (void)ctx; (void)iid;
	if (ppv)
		*ppv = NULL;
	host_trap("CoCreateInstance");
	return E_NOINTERFACE;
}

void CoTaskMemFree(LPVOID p) { free(p); }

LONG RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD opt, DWORD sam, HKEY *out)
{
	(void)k; (void)opt; (void)sam;
	host_log("RegOpenKeyExA %s", sub ? sub : "");
	if (out)
		*out = NULL;
	return ERROR_FILE_NOT_FOUND;
}

LONG RegQueryValueExA(HKEY k, LPCSTR name, DWORD *res, DWORD *ty,
		      BYTE *data, DWORD *cb)
{
	(void)k; (void)name; (void)res; (void)ty; (void)data; (void)cb;
	return ERROR_FILE_NOT_FOUND;
}

LONG RegCloseKey(HKEY k)
{
	(void)k;
	return ERROR_SUCCESS;
}

int BitBlt(HDC a, int b, int c, int d, int e, HDC f, int g, int h, DWORD i)
{
	(void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
	host_trap("BitBlt");
	return 0;
}

void *GetStockObject(int i)
{
	(void)i;
	return (void *)7;
}

UINT GetSystemPaletteEntries(HDC d, UINT s, UINT n, LPVOID e)
{
	(void)d; (void)s; (void)n; (void)e;
	return 0;
}

int PatBlt(HDC d, int x, int y, int w, int h, DWORD rop)
{
	(void)d; (void)x; (void)y; (void)w; (void)h; (void)rop;
	return 1;
}

BOOL TextOutA(HDC d, int x, int y, LPCSTR s, int n)
{
	(void)d; (void)x; (void)y; (void)s; (void)n;
	return TRUE;
}

LPVOID SHBrowseForFolderA(LPVOID bi)
{
	(void)bi;
	host_trap("SHBrowseForFolderA");
	return NULL;
}

BOOL SHGetPathFromIDListA(LPVOID id, LPSTR buf)
{
	(void)id;
	if (buf)
		buf[0] = 0;
	return FALSE;
}
