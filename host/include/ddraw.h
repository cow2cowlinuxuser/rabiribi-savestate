#ifndef HOST_DDRAW_H
#define HOST_DDRAW_H

#include "host_win.h"

#define DDSCAPS_PRIMARYSURFACE 0x00000200
#define DDSCAPS_FLIP 0x00000010
#define DDSCAPS_COMPLEX 0x00000008
#define DDSCAPS_OFFSCREENPLAIN 0x00000040
#define DDSCAPS_BACKBUFFER 0x00000004
#define DDSCAPS_SYSTEMMEMORY 0x00000800
#define DDSCAPS_3DDEVICE 0x00002000
#define DDSCAPS_TEXTURE 0x00001000
#define DDSCAPS_VIDEOMEMORY 0x00004000

#define DDSD_CAPS 0x00000001
#define DDSD_HEIGHT 0x00000002
#define DDSD_WIDTH 0x00000004
#define DDSD_PITCH 0x00000008
#define DDSD_BACKBUFFERCOUNT 0x00000020
#define DDSD_PIXELFORMAT 0x00001000

#define DDPF_RGB 0x00000040
#define DDLOCK_WAIT 0x00000001
#define DDFLIP_WAIT 0x00000001
#define DDSCL_FULLSCREEN 0x00000001
#define DDSCL_EXCLUSIVE 0x00000010
#define DDSCL_NORMAL 0x00000008

#define DDENUMRET_OK 1
#define DDENUMRET_CANCEL 0

typedef struct _DDPIXELFORMAT {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwFourCC;
	DWORD dwRGBBitCount;
	DWORD dwRBitMask;
	DWORD dwGBitMask;
	DWORD dwBBitMask;
	DWORD dwRGBAlphaBitMask;
} DDPIXELFORMAT, *LPDDPIXELFORMAT;

typedef struct _DDSCAPS2 {
	DWORD dwCaps;
	DWORD dwCaps2;
	DWORD dwCaps3;
	DWORD dwCaps4;
} DDSCAPS2, *LPDDSCAPS2;

typedef struct _DDCOLORKEY {
	DWORD dwColorSpaceLowValue;
	DWORD dwColorSpaceHighValue;
} DDCOLORKEY, *LPDDCOLORKEY;

/* Microsoft DDSURFACEDESC2 is 0x7c. Ghidra locals and GetDisplayMode
 * read dwRGBBitCount at +0x54 (pixel format at +0x48). */
typedef struct _DDSURFACEDESC2 {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwHeight;
	DWORD dwWidth;
	union {
		LONG lPitch;
		DWORD dwLinearSize;
	};
	DWORD dwBackBufferCount;
	union {
		DWORD dwMipMapCount;
		DWORD dwRefreshRate;
		DWORD dwSrcVBHandle;
	};
	DWORD dwAlphaBitDepth;
	DWORD dwReserved;
	LPVOID lpSurface;
	union {
		DDCOLORKEY ddckCKDestOverlay;
		DWORD dwEmptyFaceColor;
	};
	DDCOLORKEY ddckCKDestBlt;
	DDCOLORKEY ddckCKSrcOverlay;
	DDCOLORKEY ddckCKSrcBlt;
	DDPIXELFORMAT ddpfPixelFormat;
	DDSCAPS2 ddsCaps;
	DWORD dwTextureStage;
} DDSURFACEDESC2, *LPDDSURFACEDESC2;

typedef char _ddsd2_size_ok[(sizeof(DDSURFACEDESC2) == 0x7c) ? 1 : -1];

typedef struct _DDDEVICEIDENTIFIER2 {
	char szDriver[512];
	char szDescription[512];
	DWORD liDriverVersionLow;
	DWORD liDriverVersionHigh;
	DWORD dwVendorId, dwDeviceId, dwSubSysId, dwRevision;
	GUID guidDeviceIdentifier;
	DWORD dwWHQLLevel;
} DDDEVICEIDENTIFIER2, *LPDDDEVICEIDENTIFIER2;

typedef HRESULT (*LPDDENUMMODESCALLBACK2)(LPDDSURFACEDESC2, LPVOID);

struct IDirectDraw7;
struct IDirectDrawSurface7;

typedef struct IDirectDraw7 *LPDIRECTDRAW7;
typedef struct IDirectDrawSurface7 *LPDIRECTDRAWSURFACE7;

HRESULT DirectDrawCreateEx(GUID *guid, LPVOID *ppv, REFIID iid, LPVOID unk);

int host_surf_rgb565(LPDIRECTDRAWSURFACE7 s, unsigned short **px, DWORD *w,
		     DWORD *h);
LPDIRECTDRAWSURFACE7 host_dd_create_rgb565(DWORD w, DWORD h);
void host_surf_release(void *s);

#endif
