/* dsound.dll is a same-folder trampoline into d3d11.dll, where the software
 * DirectSound actually lives.
 *
 * The point is not to hook Windows' DirectSound better than we already do. It
 * is to keep Windows' DirectSound out of the process entirely. Launching
 * Rabi-Ribi with -nosound is the only configuration that has ever survived a
 * restore cleanly - through room changes, into a boss room, repeatedly - and
 * the difference is not that the game stops making sound. It is that
 * dsound.dll, AUDIOSES, MMDevApi, three system threads and a DMA engine that
 * writes while every thread is suspended are all absent. The game's own audio
 * state rewinds fine; it is the stack underneath that cannot go back.
 *
 * So this reproduces that process shape while DxLib still takes its sound-on
 * path. It works because rabiribi.exe imports only KERNEL32, USER32, GDI32,
 * SHELL32 and steam_api - dsound is resolved at runtime, so the game folder
 * wins the search - and because dsound is not one of the 37 KnownDLLs, which
 * would have bypassed the folder and made this silently do nothing.
 *
 * Everything of substance is in ds_sw.c inside d3d11.dll. This file exists only
 * so that the name DxLib asks for resolves to us. Keeping the implementation in
 * the wrapper means one image, one set of savestate rules, and no second CRT
 * heap to reason about. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HMODULE impl_mod(void)
{
	static HMODULE h;
	wchar_t path[MAX_PATH];
	wchar_t *slash;
	HMODULE self = NULL;

	if (h)
		return h;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)impl_mod, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return NULL;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return NULL;
	wcscpy(slash + 1, L"d3d11.dll");
	h = LoadLibraryW(path);
	return h;
}

static void *sym(const char *name)
{
	HMODULE h = impl_mod();

	return h ? (void *)GetProcAddress(h, name) : NULL;
}

HRESULT WINAPI DirectSoundCreate(const GUID *dev, void **out, void *outer)
{
	HRESULT(WINAPI * fn)(const GUID *, void **, void *);

	fn = (HRESULT(WINAPI *)(const GUID *, void **, void *))sym("ds_sw_create");
	return fn ? fn(dev, out, outer) : E_FAIL;
}

HRESULT WINAPI DirectSoundCreate8(const GUID *dev, void **out, void *outer)
{
	HRESULT(WINAPI * fn)(const GUID *, void **, void *);

	fn = (HRESULT(WINAPI *)(const GUID *, void **, void *))sym("ds_sw_create");
	return fn ? fn(dev, out, outer) : E_FAIL;
}

HRESULT WINAPI DirectSoundEnumerateA(void *cb, void *ctx)
{
	HRESULT(WINAPI * fn)(void *, void *);

	fn = (HRESULT(WINAPI *)(void *, void *))sym("ds_sw_enum_a");
	return fn ? fn(cb, ctx) : E_FAIL;
}

HRESULT WINAPI DirectSoundEnumerateW(void *cb, void *ctx)
{
	HRESULT(WINAPI * fn)(void *, void *);

	fn = (HRESULT(WINAPI *)(void *, void *))sym("ds_sw_enum_w");
	return fn ? fn(cb, ctx) : E_FAIL;
}

/* Never seen in the survey - QueryInterface was never called across 705 buffers,
 * so nothing asks for a class object - but a DLL claiming to be dsound should
 * not fail the loader's questions if something does look. */
HRESULT WINAPI DllCanUnloadNow(void)
{
	return S_FALSE;
}

HRESULT WINAPI DllGetClassObject(const GUID *cls, const GUID *iid, void **out)
{
	(void)cls;
	(void)iid;
	if (out)
		*out = NULL;
	return E_NOINTERFACE;
}

HRESULT WINAPI GetDeviceID(const GUID *src, GUID *dst)
{
	if (!dst)
		return E_INVALIDARG;
	if (src)
		*dst = *src;
	else
		ZeroMemory(dst, sizeof(*dst));
	return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(inst);
	return TRUE;
}
