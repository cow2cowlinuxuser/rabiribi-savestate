#ifndef HOST_D3D_H
#define HOST_D3D_H

#include "host_win.h"
#include "ddraw.h"

#define D3DPT_POINTLIST 1
#define D3DPT_LINELIST 2
#define D3DPT_LINESTRIP 3
#define D3DPT_TRIANGLELIST 4
#define D3DPT_TRIANGLESTRIP 5
#define D3DPT_TRIANGLEFAN 6

#define D3DFVF_XYZRHW 0x004
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_SPECULAR 0x080
#define D3DFVF_TEX1 0x100
#define D3DFVF_RBO_SPRITE (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

#define DDBD_16 0x00000400
#define D3D_OK S_OK

struct IDirect3D7;
struct IDirect3DDevice7;
typedef struct IDirect3D7 *LPDIRECT3D7;
typedef struct IDirect3DDevice7 *LPDIRECT3DDEVICE7;

extern const GUID IID_IDirect3D7;
extern const GUID IID_IDirect3DRGBDevice;

int host_guid_eq(REFIID a, const GUID *b);

/* IDirectDraw7 QI helper: IID_IDirect3D7 -> IDirect3D7 on this DD. */
HRESULT host_d3d7_query(LPDIRECTDRAW7 dd, REFIID iid, LPVOID *ppv);

#endif
