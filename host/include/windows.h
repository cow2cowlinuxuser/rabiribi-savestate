#ifndef HOST_WINDOWS_H
#define HOST_WINDOWS_H

#include "host_win.h"
#include "ddraw.h"
#include "d3d.h"
#include "dsound.h"
#include "dinput.h"
#include "objbase.h"

HWND CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style,
		     int x, int y, int w, int h, HWND parent, HMENU menu,
		     HINSTANCE inst, LPVOID param);
LRESULT DefWindowProcA(HWND h, UINT m, WPARAM w, LPARAM l);
BOOL DestroyWindow(HWND h);
BOOL PeekMessageA(LPMSG msg, HWND h, UINT min, UINT max, UINT remove);
BOOL TranslateMessage(const MSG *msg);
LRESULT DispatchMessageA(const MSG *msg);
void PostQuitMessage(int code);
int MessageBoxA(HWND h, LPCSTR text, LPCSTR cap, UINT type);
BOOL GetClientRect(HWND h, LPRECT r);
BOOL GetWindowRect(HWND h, LPRECT r);
HWND GetDesktopWindow(void);
HWND GetActiveWindow(void);
int GetSystemMetrics(int idx);
BOOL SetRect(LPRECT r, int l, int t, int ri, int b);
BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu);
LONG SetWindowLongA(HWND h, int idx, LONG val);
BOOL MoveWindow(HWND wnd, int x, int y, int w, int ht, BOOL repaint);
int ShowCursor(BOOL show);
BOOL ShowWindow(HWND h, int cmd);
HDC GetDC(HWND h);
int ReleaseDC(HWND h, HDC dc);
HICON LoadIconA(HINSTANCE i, LPCSTR name);
ATOM RegisterClassA(const void *wc);
int wsprintfA(LPSTR buf, LPCSTR fmt, ...);
LRESULT SendMessageA(HWND h, UINT msg, WPARAM w, LPARAM l);

typedef HANDLE HIMC;
HIMC ImmGetContext(HWND h);
BOOL ImmSetOpenStatus(HIMC imc, BOOL open);
HIMC ImmAssociateContext(HWND h, HIMC imc);
HWND ImmGetDefaultIMEWnd(HWND h);
BOOL ImmReleaseContext(HWND h, HIMC imc);

HANDLE CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPVOID sa,
		   DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *out, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *out, LPOVERLAPPED ov);
BOOL CloseHandle(HANDLE h);
DWORD GetFileSize(HANDLE h, DWORD *high);
DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method);
HANDLE GetProcessHeap(void);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, DWORD bytes);
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, DWORD bytes);
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID p);
DWORD HeapSize(HANDLE heap, DWORD flags, LPCVOID p);
HANDLE HeapCreate(DWORD f, DWORD init, DWORD max);
BOOL HeapDestroy(HANDLE h);
LPVOID VirtualAlloc(LPVOID a, DWORD s, DWORD t, DWORD p);
BOOL VirtualFree(LPVOID a, DWORD s, DWORD t);
HGLOBAL GlobalAlloc(UINT flags, DWORD bytes);
HGLOBAL GlobalFree(HGLOBAL h);
DWORD GetTickCount(void);
void Sleep(DWORD ms);
void GetLocalTime(LPSYSTEMTIME st);
void GetSystemTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL QueryPerformanceCounter(void *out);
BOOL QueryPerformanceFrequency(void *out);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, DWORD stack,
		    LPTHREAD_START_ROUTINE start, LPVOID arg,
		    DWORD flags, DWORD *id);
DWORD GetLastError(void);
void SetLastError(DWORD e);
DWORD GetCurrentProcess(void);
BOOL TerminateProcess(HANDLE h, UINT code);
void ExitProcess(UINT code);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);
HMODULE GetModuleHandleA(LPCSTR n);
FARPROC GetProcAddress(HMODULE m, LPCSTR n);
HMODULE LoadLibraryA(LPCSTR n);
BOOL PathFileExistsA(LPCSTR path);
LPSTR PathCombineA(LPSTR out, LPCSTR a, LPCSTR b);
BOOL PathRenameExtensionA(LPSTR path, LPCSTR ext);

DWORD timeGetTime(void);
UINT timeBeginPeriod(UINT p);

LONG RegOpenKeyExA(HKEY k, LPCSTR sub, DWORD opt, DWORD sam, HKEY *out);
LONG RegQueryValueExA(HKEY k, LPCSTR name, DWORD *res, DWORD *ty,
		      BYTE *data, DWORD *cb);
LONG RegCloseKey(HKEY k);

#endif
