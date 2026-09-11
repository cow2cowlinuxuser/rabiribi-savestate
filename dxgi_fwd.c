/* dxgi.dll is a same-folder trampoline into d3d11.dll, where the software
 * factory actually lives. Unity LoadLibrary's both names; one implementation
 * keeps factory and device on the same heap. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

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

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **pp)
{
	HRESULT(WINAPI * fn)(REFIID, void **);
	fn = (HRESULT(WINAPI *)(REFIID, void **))sym("CreateDXGIFactory");
	return fn ? fn(riid, pp) : E_FAIL;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **pp)
{
	HRESULT(WINAPI * fn)(REFIID, void **);
	fn = (HRESULT(WINAPI *)(REFIID, void **))sym("CreateDXGIFactory1");
	return fn ? fn(riid, pp) : E_FAIL;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **pp)
{
	HRESULT(WINAPI * fn)(UINT, REFIID, void **);
	fn = (HRESULT(WINAPI *)(UINT, REFIID, void **))sym("CreateDXGIFactory2");
	return fn ? fn(flags, riid, pp) : E_FAIL;
}
