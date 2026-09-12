/* xaudio2_9.dll is a same-folder trampoline into d3d11.dll, where the software
 * XAudio2 lives.
 *
 * This is the interception DirectSound would not give us. Steam has
 * System32\dsound.dll in the process before our DLL attaches, so the name is
 * already taken; it does not preload xaudio2_9.dll, so the game folder wins.
 * That is measured rather than assumed - xa2_probe.c established it by loading
 * from here and watching DxLib call XAudio2Create(flags=0,
 * processor=0xFFFFFFFF) - and xa2_probe.c stays in the tree as the record of
 * it.
 *
 * The implementation lives in the wrapper rather than here for the same reason
 * ds_sw does: one image, one set of savestate rules, and no second CRT heap to
 * account for. This file only has to make the name resolve to us. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A line next to this DLL, because everything interesting here happens before
 * the savestate log exists - which is exactly how the DirectSound attempt
 * managed to fail silently for two runs. */
static void note(const char *line)
{
	HMODULE self = NULL;
	wchar_t path[MAX_PATH], *slash;
	HANDLE f;
	DWORD n, wrote;
	char buf[512];
	SYSTEMTIME st;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)note, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	wcscpy(slash + 1, L"xaudio2_probe.txt");
	GetLocalTime(&st);
	n = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u pid %lu %s\r\n", st.wYear, st.wMonth,
		      st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), line);
	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, buf, n, &wrote, NULL);
	CloseHandle(f);
}

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

HRESULT WINAPI XAudio2Create(void **pp, UINT32 flags, UINT32 processor)
{
	HRESULT(WINAPI * fn)(void **, UINT32, UINT32);
	HMODULE h = impl_mod();
	char line[200];

	fn = h ? (HRESULT(WINAPI *)(void **, UINT32, UINT32))GetProcAddress(h, "xa2_sw_create")
	       : NULL;
	if (!fn) {
		note("XAudio2Create - the wrapper next to us had no xa2_sw_create, so "
		     "the game gets nothing");
		if (pp)
			*pp = NULL;
		return E_FAIL;
	}
	wsprintfA(line, "XAudio2Create flags=%u processor=%u - handing it the software engine",
		  flags, processor);
	note(line);
	return fn(pp, flags, processor);
}

/* Present in xaudio2_9.dll and occasionally probed for. Nothing here uses it. */
HRESULT WINAPI CreateAudioReverb(void **pp)
{
	if (pp)
		*pp = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI CreateAudioVolumeMeter(void **pp)
{
	if (pp)
		*pp = NULL;
	return E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	char shown[MAX_PATH], line[MAX_PATH + 64];

	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	if (!GetModuleFileNameA(inst, shown, MAX_PATH))
		lstrcpynA(shown, "(no path)", MAX_PATH);
	wsprintfA(line, "attached as %s", shown);
	note(line);
	return TRUE;
}
