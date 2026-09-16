/* xinput1_4.dll, dropped next to the game.
 *
 * DxLib asks LoadLibraryW for xinput1_4.dll, then xinput1_3.dll, then
 * xinput9_1_0.dll, and resolves XInputGetState and XInputSetState by name. None
 * of those are KnownDLLs, so the game folder wins the search and this file is
 * what answers.
 *
 * The reason to answer at all is determinism. Akaginite, who built a TAS
 * environment for this game from outside the process, found that the game's
 * random number consumption changes with the recognition state of the XInput
 * controller - so a run with a pad recognised and a run without it diverge, and
 * a pad that arrives or leaves mid-session diverges from itself. That makes the
 * allocation stream unreproducible for reasons that have nothing to do with the
 * allocator, which is where we had been looking.
 *
 * Pinned, every pad is permanently absent and the game asks the random number
 * generator the same questions every time. Forwarding is the default, because
 * this DLL is deployed next to a game somebody might want to play with a pad,
 * and a wrapper that silently disables hardware is a worse bug than the one it
 * fixes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif

/* Declared here rather than pulled from xinput.h: this file only ever passes
 * these through or zeroes them, and the header is not uniformly present across
 * the toolchains that build the rest of the tree. */
typedef struct {
	WORD wButtons;
	BYTE bLeftTrigger, bRightTrigger;
	SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XI_GAMEPAD;

typedef struct {
	DWORD dwPacketNumber;
	XI_GAMEPAD Gamepad;
} XI_STATE;

typedef struct {
	WORD wLeftMotorSpeed, wRightMotorSpeed;
} XI_VIBRATION;

static HMODULE g_real;
static int g_pinned = -1;

/* The knob is read the way gameheap reads its own: the environment first, then
 * d3d9_sw.cfg beside this module. This DLL cannot call into the savestate
 * engine for it, because the engine lives in d3d11.dll and there is no promise
 * about which of us the game loads first. */
static int knob_pinned(void)
{
	char val[16];
	wchar_t path[MAX_PATH];
	wchar_t *slash;
	HANDLE f;
	char buf[4096];
	DWORD got = 0;
	HMODULE self = NULL;
	const char *p;

	if (GetEnvironmentVariableA("D3D9SW_XINPUT", val, sizeof(val)) == 1)
		return val[0] == '0';

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)knob_pinned, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return 0;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return 0;
	wcscpy(slash + 1, L"d3d9_sw.cfg");

	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
	CloseHandle(f);
	buf[got] = 0;

	/* Only a line that starts the name counts, so D3D9SW_XINPUT is not matched
	 * inside some longer knob that happens to end with it. */
	for (p = buf; (p = strstr(p, "D3D9SW_XINPUT=")) != NULL; p++) {
		if (p != buf && p[-1] != '\n' && p[-1] != '\r')
			continue;
		return p[14] == '0';
	}
	return 0;
}

static void say(const char *what)
{
	wchar_t path[MAX_PATH];
	wchar_t *slash;
	HMODULE self = NULL;
	HANDLE f;
	DWORD wrote;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)say, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	wcscpy(slash + 1, L"xinput_sw.log");

	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, what, lstrlenA(what), &wrote, NULL);
	CloseHandle(f);
}

/* Explicitly out of the system directory. A bare LoadLibrary of the same name
 * would find this file again, since we are the copy the search order prefers. */
static HMODULE real(void)
{
	static const wchar_t *names[] = { L"xinput1_4.dll", L"xinput1_3.dll",
					  L"xinput9_1_0.dll" };
	wchar_t path[MAX_PATH];
	UINT n;
	int i;

	if (g_real)
		return g_real;
	n = GetSystemDirectoryW(path, MAX_PATH);
	if (!n || n >= MAX_PATH - 20)
		return NULL;
	for (i = 0; i < 3; i++) {
		wcscpy(path + n, L"\\");
		wcscpy(path + n + 1, names[i]);
		g_real = LoadLibraryW(path);
		if (g_real)
			return g_real;
	}
	return NULL;
}

static void *sym(const char *name)
{
	HMODULE h = real();
	return h ? (void *)GetProcAddress(h, name) : NULL;
}

static int pinned(void)
{
	if (g_pinned < 0) {
		g_pinned = knob_pinned();
		say(g_pinned ? "xinput_sw: pinned - every pad reports absent, so "
			       "the game asks the RNG the same questions every run\r\n"
			     : "xinput_sw: forwarding to the system XInput - pad "
			       "state is whatever the hardware says, which is not "
			       "reproducible run to run\r\n");
	}
	return g_pinned;
}

DWORD WINAPI XInputGetState(DWORD idx, XI_STATE *st)
{
	DWORD(WINAPI * fn)(DWORD, XI_STATE *);

	if (pinned()) {
		/* Zeroed rather than left alone: a caller that ignores the return
		 * value then reads a stale packet number would see motion that
		 * never happened, which is the opposite of the point. */
		if (st)
			memset(st, 0, sizeof(*st));
		return ERROR_DEVICE_NOT_CONNECTED;
	}
	fn = (DWORD(WINAPI *)(DWORD, XI_STATE *))sym("XInputGetState");
	return fn ? fn(idx, st) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD idx, XI_VIBRATION *v)
{
	DWORD(WINAPI * fn)(DWORD, XI_VIBRATION *);

	if (pinned())
		return ERROR_DEVICE_NOT_CONNECTED;
	fn = (DWORD(WINAPI *)(DWORD, XI_VIBRATION *))sym("XInputSetState");
	return fn ? fn(idx, v) : ERROR_DEVICE_NOT_CONNECTED;
}

/* The game resolves only the two above. The rest are here so that anything else
 * loading this folder's XInput finds a complete DLL rather than a partial one. */
DWORD WINAPI XInputGetCapabilities(DWORD idx, DWORD flags, void *caps)
{
	DWORD(WINAPI * fn)(DWORD, DWORD, void *);

	if (pinned())
		return ERROR_DEVICE_NOT_CONNECTED;
	fn = (DWORD(WINAPI *)(DWORD, DWORD, void *))sym("XInputGetCapabilities");
	return fn ? fn(idx, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD idx, BYTE kind, void *info)
{
	DWORD(WINAPI * fn)(DWORD, BYTE, void *);

	if (pinned())
		return ERROR_DEVICE_NOT_CONNECTED;
	fn = (DWORD(WINAPI *)(DWORD, BYTE,
			      void *))sym("XInputGetBatteryInformation");
	return fn ? fn(idx, kind, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD idx, DWORD reserved, void *key)
{
	DWORD(WINAPI * fn)(DWORD, DWORD, void *);

	if (pinned())
		return ERROR_DEVICE_NOT_CONNECTED;
	fn = (DWORD(WINAPI *)(DWORD, DWORD, void *))sym("XInputGetKeystroke");
	return fn ? fn(idx, reserved, key) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable)
{
	void(WINAPI * fn)(BOOL);

	if (pinned())
		return;
	fn = (void(WINAPI *)(BOOL))sym("XInputEnable");
	if (fn)
		fn(enable);
}
