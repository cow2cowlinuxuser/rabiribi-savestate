#ifndef HOST_WIN_H
#define HOST_WIN_H

/*
 * Minimal Win32 types for Ghidra-lifted RBO_Ex3 on PowerPC.
 * COM this-pointer is argument 0 (already how Ghidra emits calls).
 */

#include <stddef.h>
#include <stdint.h>
#include <gcbool.h>

#define WINAPI
#define CALLBACK
#define APIENTRY
#define STDMETHODCALLTYPE
#define CONST const

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef int INT;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned short WCHAR;
typedef char CHAR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *HANDLE;
typedef HANDLE HWND;
typedef HANDLE HDC;
typedef HANDLE HINSTANCE;
typedef HANDLE HMODULE;
typedef HANDLE HICON;
typedef HANDLE HCURSOR;
typedef HANDLE HBRUSH;
typedef HANDLE HMENU;
typedef HANDLE HBITMAP;
typedef HANDLE HFONT;
typedef HANDLE HPALETTE;
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef HANDLE HKEY;
typedef HANDLE HRESULT_HANDLE;
typedef int INT_PTR;
typedef unsigned int UINT_PTR;
typedef long LONG_PTR;
typedef unsigned long ULONG_PTR;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef DWORD COLORREF;
typedef WORD ATOM;

typedef int32_t HRESULT;
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define DD_OK S_OK
#define DDERR_GENERIC E_FAIL
#define DDERR_UNSUPPORTED ((HRESULT)0x80004001)
#define DS_OK S_OK
#define DI_OK S_OK
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define MAX_PATH 260

#define REG_NONE 0
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2

typedef struct _GUID {
	DWORD Data1;
	WORD Data2;
	WORD Data3;
	BYTE Data4[8];
} GUID;
typedef GUID IID;
typedef const IID *REFIID;
typedef const GUID *REFGUID;
typedef GUID CLSID;
typedef const CLSID *REFCLSID;

typedef struct tagRECT {
	LONG left;
	LONG top;
	LONG right;
	LONG bottom;
} RECT, *LPRECT, *PRECT, tagRECT;

typedef struct tagPOINT {
	LONG x;
	LONG y;
} POINT, *LPPOINT;

typedef struct _FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
	WORD wYear, wMonth, wDayOfWeek, wDay;
	WORD wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef struct _OVERLAPPED {
	ULONG_PTR Internal;
	ULONG_PTR InternalHigh;
	DWORD Offset;
	DWORD OffsetHigh;
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef int (*FARPROC)(void);

typedef struct _SECURITY_ATTRIBUTES {
	DWORD nLength;
	LPVOID lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _RTL_CRITICAL_SECTION {
	void *DebugInfo;
	LONG LockCount;
	LONG RecursionCount;
	HANDLE OwningThread;
	HANDLE LockSemaphore;
	ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

typedef struct tagMSG {
	HWND hwnd;
	UINT message;
	WPARAM wParam;
	LPARAM lParam;
	DWORD time;
	POINT pt;
} MSG, *LPMSG;

#define WM_QUIT 0x0012
#define PM_REMOVE 0x0001
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define GWL_STYLE (-16)

#ifndef HWND_DESKTOP
#define HWND_DESKTOP ((HWND)0)
#endif

#endif /* HOST_WIN_H */
