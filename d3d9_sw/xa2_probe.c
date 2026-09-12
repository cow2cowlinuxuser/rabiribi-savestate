/* Did -xaudio2's LoadLibrary land on us?
 *
 * The savestate log with `-xaudio2` names the module `xaudio2_9.DLL` (not
 * xaudio2_7, which is only leftover in SYSROOT). Unlike dsound, Steam does not
 * already have this file in the process when we attach, so a same-folder copy
 * is allowed to win the search. That is the entire test. There is no device,
 * no voice, no mixer.
 *
 * Proof is xaudio2_probe.txt next to this DLL: attach says which file loaded,
 * XAudio2Create says DxLib actually called in. Returning E_FAIL is deliberate -
 * the load is the question, not a fake IXAudio2. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void note(const char *line)
{
	HMODULE self = NULL;
	wchar_t path[MAX_PATH], *slash;
	HANDLE f;
	DWORD n, wrote;
	char buf[512];
	SYSTEMTIME st;

	/* The log sits next to this DLL, so a copy left in x86\ cannot be
	 * mistaken for a hit inside the game folder. */
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
	n = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u pid %lu %s\r\n",
		      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
		      GetCurrentProcessId(), line);
	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, buf, n, &wrote, NULL);
	CloseHandle(f);
}

HRESULT WINAPI XAudio2Create(void **pp, UINT32 flags, UINT32 processor)
{
	char line[160];

	wsprintfA(line, "XAudio2Create flags=%u processor=%u - refusing, load is the test",
		  flags, processor);
	note(line);
	if (pp)
		*pp = NULL;
	(void)processor;
	return E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	char shown[MAX_PATH], line[MAX_PATH + 64];

	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	DisableThreadLibraryCalls(inst);
	shown[0] = 0;
	if (!GetModuleFileNameA(inst, shown, MAX_PATH))
		lstrcpynA(shown, "(no path)", MAX_PATH);
	wsprintfA(line, "attached as %s", shown);
	note(line);
	return TRUE;
}
