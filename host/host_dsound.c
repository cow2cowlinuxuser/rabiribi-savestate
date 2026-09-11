#include <malloc.h>
#include <string.h>

#include "dsound.h"
#include "host.h"

typedef struct IDirectSound8 IDirectSound8;
typedef struct IDirectSound8Vtbl {
	HRESULT (*QueryInterface)(IDirectSound8 *, REFIID, LPVOID *);
	ULONG (*AddRef)(IDirectSound8 *);
	ULONG (*Release)(IDirectSound8 *);
	HRESULT (*CreateSoundBuffer)(IDirectSound8 *, LPVOID, LPVOID *, LPVOID);
	HRESULT (*GetCaps)(IDirectSound8 *, LPVOID);
	HRESULT (*DuplicateSoundBuffer)(IDirectSound8 *, LPVOID, LPVOID *);
	HRESULT (*SetCooperativeLevel)(IDirectSound8 *, HWND, DWORD);
	HRESULT (*Compact)(IDirectSound8 *);
	HRESULT (*GetSpeakerConfig)(IDirectSound8 *, DWORD *);
	HRESULT (*SetSpeakerConfig)(IDirectSound8 *, DWORD);
	HRESULT (*Initialize)(IDirectSound8 *, GUID *);
	HRESULT (*VerifyCertification)(IDirectSound8 *, DWORD *);
} IDirectSound8Vtbl;

struct IDirectSound8 {
	const IDirectSound8Vtbl *lpVtbl;
	LONG ref;
};

static HRESULT DS_QI(IDirectSound8 *t, REFIID i, LPVOID *p)
{
	(void)i;
	if (!p)
		return E_POINTER;
	t->ref++;
	*p = t;
	return S_OK;
}
static ULONG DS_AddRef(IDirectSound8 *t) { return (ULONG)++t->ref; }
static ULONG DS_Release(IDirectSound8 *t)
{
	if (--t->ref > 0)
		return (ULONG)t->ref;
	free(t);
	return 0;
}
static HRESULT DS_CreateBuf(IDirectSound8 *t, LPVOID a, LPVOID *b, LPVOID c)
{
	(void)a; (void)b; (void)c;
	host_trap("DS.CreateSoundBuffer");
	return E_NOTIMPL;
}
static HRESULT DS_GetCaps(IDirectSound8 *t, LPVOID a)
{
	(void)t; (void)a;
	host_trap("DS.GetCaps");
	return E_NOTIMPL;
}
static HRESULT DS_Dup(IDirectSound8 *t, LPVOID a, LPVOID *b)
{
	(void)a; (void)b;
	host_trap("DS.DuplicateSoundBuffer");
	return E_NOTIMPL;
}
static HRESULT DS_SetCoop(IDirectSound8 *t, HWND h, DWORD f)
{
	(void)t;
	(void)h;
	host_log("DS SetCooperativeLevel %x", (unsigned)f);
	return DS_OK;
}
static HRESULT DS_Compact(IDirectSound8 *t) { (void)t; return DS_OK; }
static HRESULT DS_GetSpk(IDirectSound8 *t, DWORD *c)
{
	if (c)
		*c = 0;
	(void)t;
	return DS_OK;
}
static HRESULT DS_SetSpk(IDirectSound8 *t, DWORD c)
{
	(void)t;
	(void)c;
	return DS_OK;
}
static HRESULT DS_Init(IDirectSound8 *t, GUID *g)
{
	(void)t;
	(void)g;
	return DS_OK;
}
static HRESULT DS_Verify(IDirectSound8 *t, DWORD *c)
{
	if (c)
		*c = 0;
	(void)t;
	return DS_OK;
}

static const IDirectSound8Vtbl s_ds8 = {
	DS_QI, DS_AddRef, DS_Release, DS_CreateBuf, DS_GetCaps, DS_Dup,
	DS_SetCoop, DS_Compact, DS_GetSpk, DS_SetSpk, DS_Init, DS_Verify
};

HRESULT DirectSoundCreate8(GUID *guid, LPDIRECTSOUND8 *ppv, LPVOID unk)
{
	IDirectSound8 *ds;

	(void)guid;
	(void)unk;
	if (!ppv)
		return E_POINTER;
	ds = (IDirectSound8 *)memalign(32, sizeof(*ds));
	if (!ds)
		return E_OUTOFMEMORY;
	memset(ds, 0, sizeof(*ds));
	ds->lpVtbl = &s_ds8;
	ds->ref = 1;
	*ppv = ds;
	host_log("DirectSoundCreate8");
	return DS_OK;
}
