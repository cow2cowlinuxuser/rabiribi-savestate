#include <string.h>

#include "windows.h"
#include "host.h"
#include "escape.h"
#include "rbo_input.h"

/* Same COM call shape Ghidra emits: (**(code **)(*obj + off))(obj, ...). */
#define VT(obj, off) (*(void **)((char *)(*(void **)(obj)) + (off)))

typedef HRESULT (*fn_setcoop)(void *, HWND, DWORD);
typedef HRESULT (*fn_setmode)(void *, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef HRESULT (*fn_create)(void *, LPDDSURFACEDESC2, LPDIRECTDRAWSURFACE7 *, LPVOID);
typedef HRESULT (*fn_lock)(void *, LPRECT, LPDDSURFACEDESC2, DWORD, HANDLE);
typedef HRESULT (*fn_unlock)(void *, LPRECT);
typedef HRESULT (*fn_flip)(void *, void *, DWORD);
typedef HRESULT (*fn_ds_coop)(void *, HWND, DWORD);

static void fill_checker(u16 *p, DWORD w, DWORD h)
{
	DWORD x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			u16 c = ((x ^ y) & 16) ? 0xF800 : 0x001F;
			if (y < 40)
				c = 0x07E0;
			p[y * w + x] = c;
		}
	}
}

int host_winmain(void)
{
	LPDIRECTDRAW7 dd = NULL;
	LPDIRECTDRAWSURFACE7 surf = NULL;
	LPDIRECTSOUND8 ds = NULL;
	LPDIRECTINPUT8 di = NULL;
	DDSURFACEDESC2 desc;
	HRESULT hr;

	host_log("host_winmain");
	CoInitialize(NULL);
	hr = DirectDrawCreateEx(NULL, (LPVOID *)&dd, NULL, NULL);
	if (hr != DD_OK || !dd) {
		host_trap("DirectDrawCreateEx fail");
		return 1;
	}
	hr = ((fn_setcoop)VT(dd, 0x50))(dd, (HWND)1,
					DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
	hr = ((fn_setmode)VT(dd, 0x54))(dd, 640, 480, 16, 0, 0);
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);
	desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
	desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
	desc.dwBackBufferCount = 1;
	hr = ((fn_create)VT(dd, 0x18))(dd, &desc, &surf, NULL);
	if (hr != DD_OK || !surf) {
		host_trap("CreateSurface fail");
		return 1;
	}
	DirectSoundCreate8(NULL, &ds, NULL);
	if (ds)
		((fn_ds_coop)VT(ds, 0x18))(ds, (HWND)1, DSSCL_PRIORITY);
	DirectInput8Create(NULL, 0x800, NULL, (LPVOID *)&di, NULL);

	for (;;) {
		DDSURFACEDESC2 lockd;

		rbo_input_poll();
		if (rbo_input_held(PAD_BUTTON_START) && escape_available())
			escape_now();
		memset(&lockd, 0, sizeof(lockd));
		lockd.dwSize = sizeof(lockd);
		if (((fn_lock)VT(surf, 0x64))(surf, NULL, &lockd, DDLOCK_WAIT, NULL) == DD_OK) {
			if (lockd.lpSurface)
				fill_checker((u16 *)lockd.lpSurface, lockd.dwWidth,
					     lockd.dwHeight);
			((fn_unlock)VT(surf, 0x80))(surf, NULL);
		}
		((fn_flip)VT(surf, 0x2C))(surf, NULL, DDFLIP_WAIT);
	}
}
