#include <malloc.h>
#include <string.h>

#include "dinput.h"
#include "host.h"
#include "rbo_input.h"

typedef struct IDirectInput8A IDirectInput8A;
typedef struct IDirectInputDevice8A IDirectInputDevice8A;

typedef struct IDirectInput8Vtbl {
	HRESULT (*QueryInterface)(IDirectInput8A *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirectInput8A *);
	ULONG (*Release)(IDirectInput8A *);
	HRESULT (*CreateDevice)(IDirectInput8A *, REFGUID, LPDIRECTINPUTDEVICE8 *, LPVOID);
	HRESULT (*EnumDevices)(IDirectInput8A *, DWORD, LPVOID, LPVOID, DWORD);
	HRESULT (*GetDeviceStatus)(IDirectInput8A *, REFGUID);
	HRESULT (*RunControlPanel)(IDirectInput8A *, HWND, DWORD);
	HRESULT (*Initialize)(IDirectInput8A *, HINSTANCE, DWORD);
} IDirectInput8Vtbl;

typedef struct IDirectInputDevice8Vtbl {
	HRESULT (*QueryInterface)(IDirectInputDevice8A *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirectInputDevice8A *);
	ULONG (*Release)(IDirectInputDevice8A *);
	HRESULT (*GetCapabilities)(IDirectInputDevice8A *, LPVOID);
	HRESULT (*EnumObjects)(IDirectInputDevice8A *, LPVOID, LPVOID, DWORD);
	HRESULT (*GetProperty)(IDirectInputDevice8A *, REFGUID, LPVOID);
	HRESULT (*SetProperty)(IDirectInputDevice8A *, REFGUID, LPVOID);
	HRESULT (*Acquire)(IDirectInputDevice8A *);
	HRESULT (*Unacquire)(IDirectInputDevice8A *);
	HRESULT (*GetDeviceState)(IDirectInputDevice8A *, DWORD, LPVOID);
	HRESULT (*GetDeviceData)(IDirectInputDevice8A *, DWORD, LPVOID, DWORD *, DWORD);
	HRESULT (*SetDataFormat)(IDirectInputDevice8A *, LPVOID);
	HRESULT (*SetEventNotification)(IDirectInputDevice8A *, HANDLE);
	HRESULT (*SetCooperativeLevel)(IDirectInputDevice8A *, HWND, DWORD);
	HRESULT (*GetObjectInfo)(IDirectInputDevice8A *, LPVOID, DWORD, DWORD);
	HRESULT (*GetDeviceInfo)(IDirectInputDevice8A *, LPVOID);
	HRESULT (*RunControlPanel)(IDirectInputDevice8A *, HWND, DWORD);
	HRESULT (*Initialize)(IDirectInputDevice8A *, HINSTANCE, DWORD, REFGUID);
} IDirectInputDevice8Vtbl;

struct IDirectInput8A {
	const IDirectInput8Vtbl *lpVtbl;
	LONG ref;
};

struct IDirectInputDevice8A {
	const IDirectInputDevice8Vtbl *lpVtbl;
	LONG ref;
	int acquired;
};

static HRESULT Dev_QI(IDirectInputDevice8A *t, REFIID i, LPVOID *p)
{
	(void)i;
	if (!p)
		return E_POINTER;
	t->ref++;
	*p = t;
	return S_OK;
}
static ULONG Dev_AddRef(IDirectInputDevice8A *t) { return (ULONG)++t->ref; }
static ULONG Dev_Release(IDirectInputDevice8A *t)
{
	if (--t->ref > 0)
		return (ULONG)t->ref;
	free(t);
	return 0;
}
static HRESULT Dev_Trap(IDirectInputDevice8A *t, const char *n)
{
	(void)t;
	host_trap(n);
	return E_NOTIMPL;
}
static HRESULT Dev_GetCaps(IDirectInputDevice8A *t, LPVOID a)
{
	(void)a;
	return Dev_Trap(t, "DI.GetCapabilities");
}
static HRESULT Dev_EnumObj(IDirectInputDevice8A *t, LPVOID a, LPVOID b, DWORD c)
{
	(void)a; (void)b; (void)c;
	return Dev_Trap(t, "DI.EnumObjects");
}
static HRESULT Dev_GetProp(IDirectInputDevice8A *t, REFGUID a, LPVOID b)
{
	(void)a; (void)b;
	return Dev_Trap(t, "DI.GetProperty");
}
static HRESULT Dev_SetProp(IDirectInputDevice8A *t, REFGUID a, LPVOID b)
{
	(void)a; (void)b;
	return DI_OK;
}
static HRESULT Dev_Acquire(IDirectInputDevice8A *t)
{
	t->acquired = 1;
	return DI_OK;
}
static HRESULT Dev_Unacquire(IDirectInputDevice8A *t)
{
	t->acquired = 0;
	return DI_OK;
}
static HRESULT Dev_GetState(IDirectInputDevice8A *t, DWORD cb, LPVOID out)
{
	const RboPad *p;

	if (!out)
		return E_POINTER;
	memset(out, 0, cb);
	/* Snapshot only. Polling here steals the title menu's rising edge. */
	p = rbo_input_pad(0);
	/*
	 * Live poll is not this boot trap. When slots are type 2 (0x200)
	 * and DAT_00bc80dc holds devices, fill DIJOYSTATE per chan:
	 *   Up/Down/Left/Right  stick Y+ / Y- / X- / X+
	 *   Weak B, Strong A, Guard L, Special R, Pause Start
	 * Type 1 (0x100) is DIK bytes; do not fake a keyboard for pads.
	 */
	/* BYTE[256] keyboard or DIJOYSTATE: stash buttons in first bytes. */
	if (cb >= 16 && p) {
		BYTE *b = (BYTE *)out;
		b[0] = (BYTE)(p->held & 0xff);
		b[1] = (BYTE)((p->held >> 8) & 0xff);
		b[2] = (BYTE)p->stick_x;
		b[3] = (BYTE)p->stick_y;
	}
	(void)t;
	return DI_OK;
}
static HRESULT Dev_GetData(IDirectInputDevice8A *t, DWORD a, LPVOID b, DWORD *c, DWORD d)
{
	(void)a; (void)b; (void)d;
	if (c)
		*c = 0;
	return Dev_Trap(t, "DI.GetDeviceData");
}
static HRESULT Dev_SetFmt(IDirectInputDevice8A *t, LPVOID f)
{
	(void)t;
	(void)f;
	host_log("DI SetDataFormat");
	return DI_OK;
}
static HRESULT Dev_SetEvent(IDirectInputDevice8A *t, HANDLE h)
{
	(void)h;
	return Dev_Trap(t, "DI.SetEventNotification");
}
static HRESULT Dev_SetCoop(IDirectInputDevice8A *t, HWND h, DWORD f)
{
	(void)t;
	(void)h;
	host_log("DI SetCooperativeLevel %x", (unsigned)f);
	return DI_OK;
}
static HRESULT Dev_ObjInfo(IDirectInputDevice8A *t, LPVOID a, DWORD b, DWORD c)
{
	(void)a; (void)b; (void)c;
	return Dev_Trap(t, "DI.GetObjectInfo");
}
static HRESULT Dev_DevInfo(IDirectInputDevice8A *t, LPVOID a)
{
	(void)a;
	return Dev_Trap(t, "DI.GetDeviceInfo");
}
static HRESULT Dev_Panel(IDirectInputDevice8A *t, HWND h, DWORD f)
{
	(void)h; (void)f;
	return Dev_Trap(t, "DI.RunControlPanel");
}
static HRESULT Dev_Init(IDirectInputDevice8A *t, HINSTANCE i, DWORD v, REFGUID g)
{
	(void)t; (void)i; (void)v; (void)g;
	return DI_OK;
}

static const IDirectInputDevice8Vtbl s_dev = {
	Dev_QI, Dev_AddRef, Dev_Release, Dev_GetCaps, Dev_EnumObj, Dev_GetProp,
	Dev_SetProp, Dev_Acquire, Dev_Unacquire, Dev_GetState, Dev_GetData,
	Dev_SetFmt, Dev_SetEvent, Dev_SetCoop, Dev_ObjInfo, Dev_DevInfo,
	Dev_Panel, Dev_Init
};

static HRESULT DI_QI(IDirectInput8A *t, REFIID i, LPVOID *p)
{
	(void)i;
	if (!p)
		return E_POINTER;
	t->ref++;
	*p = t;
	return S_OK;
}
static ULONG DI_AddRef(IDirectInput8A *t) { return (ULONG)++t->ref; }
static ULONG DI_Release(IDirectInput8A *t)
{
	if (--t->ref > 0)
		return (ULONG)t->ref;
	free(t);
	return 0;
}
static HRESULT DI_CreateDevice(IDirectInput8A *t, REFGUID g, LPDIRECTINPUTDEVICE8 *out,
			       LPVOID unk)
{
	IDirectInputDevice8A *dev;

	(void)t;
	(void)g;
	(void)unk;
	if (!out)
		return E_POINTER;
	dev = (IDirectInputDevice8A *)memalign(32, sizeof(*dev));
	if (!dev)
		return E_OUTOFMEMORY;
	memset(dev, 0, sizeof(*dev));
	dev->lpVtbl = &s_dev;
	dev->ref = 1;
	*out = (LPDIRECTINPUTDEVICE8)dev;
	host_log("DI CreateDevice");
	return DI_OK;
}
static HRESULT DI_Enum(IDirectInput8A *t, DWORD a, LPVOID b, LPVOID c, DWORD d)
{
	(void)t; (void)a; (void)b; (void)c; (void)d;
	host_log("DI EnumDevices");
	return DI_OK;
}
static HRESULT DI_Status(IDirectInput8A *t, REFGUID g)
{
	(void)t;
	(void)g;
	return DI_OK;
}
static HRESULT DI_Panel(IDirectInput8A *t, HWND h, DWORD f)
{
	(void)t; (void)h; (void)f;
	return E_NOTIMPL;
}
static HRESULT DI_Init(IDirectInput8A *t, HINSTANCE i, DWORD v)
{
	(void)t; (void)i; (void)v;
	return DI_OK;
}

static const IDirectInput8Vtbl s_di8 = {
	DI_QI, DI_AddRef, DI_Release, DI_CreateDevice, DI_Enum, DI_Status,
	DI_Panel, DI_Init
};

HRESULT DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
			   LPVOID *ppv, LPVOID unk)
{
	IDirectInput8A *di;

	(void)inst;
	(void)version;
	(void)iid;
	(void)unk;
	if (!ppv)
		return E_POINTER;
	di = (IDirectInput8A *)memalign(32, sizeof(*di));
	if (!di)
		return E_OUTOFMEMORY;
	memset(di, 0, sizeof(*di));
	di->lpVtbl = &s_di8;
	di->ref = 1;
	*ppv = di;
	host_log("DirectInput8Create");
	return DI_OK;
}
