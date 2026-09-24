/* Same-folder steam_api.dll that records what Rabi-Ribi actually calls.
 *
 * The real Steam client still answers. Rename the game's steam_api.dll to
 * steam_api_real.dll and put this file in its place. SteamAPI_Init goes
 * through to the real DLL, which is what the running game needs in order to
 * stay a Steam process. Every other call is forwarded too, and written to
 * steam_shim.log beside this DLL.
 *
 * Startup refuses to open a window unless this file's size is in
 * [182373, 201570] bytes. The original DLL is 187472. After linking, pad
 * the file out to that size; a smaller or larger shim exits before the
 * title screen.
 *
 * The ten imports are not the surface. SteamUser() and the others return an
 * interface, and the game calls methods through its vtable. Those methods are
 * thiscall. Each slot in the replacement vtable logs its index and jumps to
 * the real method with ecx set back to the real object.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NIFACE 5
#define NSLOT 128

struct wrap {
	void *vt;
	void *real;
	const char *name;
	int which;
};

extern void *thunk_table[];

static HMODULE real_dll;
static CRITICAL_SECTION cs;
static volatile LONG cs_state;
static unsigned short hits[NIFACE][NSLOT];
static unsigned run_n;
static int summary_written;
static volatile LONG crash_logged;

static void note(const char *line);

static void lock(void)
{
	LONG s = InterlockedCompareExchange(&cs_state, 1, 0);
	if (s == 0) {
		InitializeCriticalSection(&cs);
		InterlockedExchange(&cs_state, 2);
	} else {
		while (cs_state != 2)
			Sleep(0);
	}
	EnterCriticalSection(&cs);
}

static void unlock(void)
{
	LeaveCriticalSection(&cs);
}

static void note(const char *line)
{
	HMODULE self = NULL;
	wchar_t path[MAX_PATH], *slash;
	HANDLE f;
	DWORD n, wrote;
	char buf[640];
	SYSTEMTIME st;

	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)note, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	wcscpy(slash + 1, L"steam_shim.log");
	GetLocalTime(&st);
	n = wsprintfA(buf, "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute, st.wSecond,
		      st.wMilliseconds, line);
	f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	WriteFile(f, buf, n, &wrote, NULL);
	CloseHandle(f);
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	char buf[160];

	/* OutputDebugString and the MSVC thread-name exception are not crashes. */
	if (code == 0x40010006 || code == 0x4001000A || code == 0x406D1388)
		return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedExchange(&crash_logged, 1) == 0) {
		wsprintfA(buf, "exception %08lx at %p", code,
			  ep->ExceptionRecord->ExceptionAddress);
		note(buf);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	char buf[80];

	(void)inst;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		AddVectoredExceptionHandler(1, crash_veh);
		wsprintfA(buf, "attach pid=%lu", GetCurrentProcessId());
		note(buf);
	} else if (reason == DLL_PROCESS_DETACH) {
		wsprintfA(buf, "detach pid=%lu", GetCurrentProcessId());
		note(buf);
	}
	return TRUE;
}

static HMODULE real_mod(void)
{
	wchar_t path[MAX_PATH], *slash;
	HMODULE self = NULL;

	if (real_dll)
		return real_dll;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)real_mod, &self);
	if (!self || !GetModuleFileNameW(self, path, MAX_PATH))
		return NULL;
	slash = wcsrchr(path, L'\\');
	if (!slash)
		return NULL;
	wcscpy(slash + 1, L"steam_api_real.dll");
	real_dll = LoadLibraryW(path);
	if (!real_dll)
		note("steam_api_real.dll did not load; rename the original steam_api.dll to that");
	return real_dll;
}

static FARPROC real_proc(const char *name)
{
	HMODULE m = real_mod();
	FARPROC f = m ? GetProcAddress(m, name) : NULL;
	char buf[128];
	if (!f) {
		wsprintfA(buf, "missing export %s", name);
		note(buf);
	}
	return f;
}

/* Public callback ids. The number is what CCallbackBase stores; the name is
 * only so the log can be read without the SDK open. Unknown ids stay numeric. */
static const char *callback_name(int id)
{
	switch (id) {
	case 101: return "SteamServersConnected";
	case 102: return "SteamServerConnectFailure";
	case 103: return "SteamServersDisconnected";
	case 113: return "ClientGameServerDeny";
	case 117: return "IPCFailure";
	case 125: return "LicensesUpdated";
	case 143: return "ValidateAuthTicketResponse";
	case 152: return "MicroTxnAuthorizationResponse";
	case 163: return "GetAuthSessionTicketResponse";
	case 164: return "GameWebCallback";
	case 304: return "PersonaStateChange";
	case 331: return "GameOverlayActivated";
	case 332: return "GameServerChangeRequested";
	case 333: return "GameLobbyJoinRequested";
	case 334: return "AvatarImageLoaded";
	case 337: return "GameRichPresenceJoinRequested";
	case 701: return "IPCountry";
	case 702: return "LowBatteryPower";
	case 703: return "SteamAPICallCompleted";
	case 704: return "SteamShutdown";
	case 1005: return "DlcInstalled";
	case 1014: return "NewUrlLaunchParameters";
	case 1101: return "UserStatsReceived";
	case 1102: return "UserStatsStored";
	case 1103: return "UserAchievementStored";
	case 1104: return "LeaderboardFindResult";
	case 1105: return "LeaderboardScoresDownloaded";
	case 1106: return "LeaderboardScoreUploaded";
	case 1107: return "NumberOfCurrentPlayers";
	case 1112: return "GlobalStatsReceived";
	default: return NULL;
	}
}

static void log_callback(const char *what, void *cb, unsigned long long call)
{
	char buf[256];
	int id = 0;
	unsigned flags = 0;
	const char *name = NULL;

	if (cb && !IsBadReadPtr(cb, 12)) {
		flags = *(unsigned char *)((char *)cb + 4);
		id = *(int *)((char *)cb + 8);
		name = callback_name(id);
	}
	if (name)
		wsprintfA(buf, "%s cb=%p id=%d %s flags=%u call=%08x%08x", what, cb, id, name,
			  flags, (unsigned)(call >> 32), (unsigned)call);
	else
		wsprintfA(buf, "%s cb=%p id=%d flags=%u call=%08x%08x", what, cb, id, flags,
			  (unsigned)(call >> 32), (unsigned)call);
	note(buf);
}

static struct wrap wraps[NIFACE];
static int nwrap;

static void *wrap_iface(const char *name, int which, void *real)
{
	int i;
	char buf[128];

	if (!real)
		return NULL;
	for (i = 0; i < nwrap; i++)
		if (wraps[i].real == real)
			return &wraps[i];
	if (nwrap >= NIFACE) {
		note("too many steam interfaces; returning the real pointer");
		return real;
	}
	wraps[nwrap].vt = thunk_table;
	wraps[nwrap].real = real;
	wraps[nwrap].name = name;
	wraps[nwrap].which = which;
	wsprintfA(buf, "%s -> %p", name, real);
	note(buf);
	return &wraps[nwrap++];
}

/* Called from steam_fwd.S with the wrap pointer and the vtable slot. */
void log_slot(struct wrap *w, int slot)
{
	char buf[160];

	if (!w || slot < 0 || slot >= NSLOT)
		return;
	lock();
	if (w->which >= 0 && w->which < NIFACE && hits[w->which][slot] < 0xffff)
		hits[w->which][slot]++;
	if (hits[w->which][slot] == 1) {
		wsprintfA(buf, "call %s slot %d", w->name, slot);
		note(buf);
	}
	unlock();
}

static void dump_hits(const char *why)
{
	char buf[160];
	int i, s, any = 0;

	if (summary_written && strcmp(why, "shutdown") != 0)
		return;
	lock();
	wsprintfA(buf, "summary (%s)", why);
	note(buf);
	for (i = 0; i < nwrap; i++) {
		for (s = 0; s < NSLOT; s++) {
			if (!hits[i][s])
				continue;
			any = 1;
			wsprintfA(buf, "  %s slot %d  x%u", wraps[i].name, s, hits[i][s]);
			note(buf);
		}
	}
	if (!any)
		note("  (no interface methods)");
	if (strcmp(why, "shutdown") == 0)
		summary_written = 1;
	unlock();
}

int SteamAPI_Init(void)
{
	typedef int (__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamAPI_Init");
	int r = fn ? fn() : 0;
	char buf[64];
	wsprintfA(buf, "SteamAPI_Init -> %d", r);
	note(buf);
	return r;
}

void SteamAPI_Shutdown(void)
{
	typedef void (__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamAPI_Shutdown");
	note("SteamAPI_Shutdown");
	dump_hits("shutdown");
	if (fn)
		fn();
}

void SteamAPI_RunCallbacks(void)
{
	typedef void (__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamAPI_RunCallbacks");
	run_n++;
	if (run_n == 1 || (run_n % 1000) == 0) {
		char buf[64];
		wsprintfA(buf, "SteamAPI_RunCallbacks n=%u", run_n);
		note(buf);
	}
	if (run_n == 100)
		dump_hits("after 100 RunCallbacks");
	if (fn)
		fn();
}

void SteamAPI_RegisterCallResult(void *cb, unsigned long long call)
{
	typedef void (__cdecl *fn_t)(void *, unsigned long long);
	fn_t fn = (fn_t)real_proc("SteamAPI_RegisterCallResult");
	log_callback("RegisterCallResult", cb, call);
	if (fn)
		fn(cb, call);
}

void SteamAPI_UnregisterCallResult(void *cb, unsigned long long call)
{
	typedef void (__cdecl *fn_t)(void *, unsigned long long);
	fn_t fn = (fn_t)real_proc("SteamAPI_UnregisterCallResult");
	log_callback("UnregisterCallResult", cb, call);
	if (fn)
		fn(cb, call);
}

void *SteamApps(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamApps");
	return wrap_iface("SteamApps", 0, fn ? fn() : NULL);
}

void *SteamFriends(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamFriends");
	return wrap_iface("SteamFriends", 1, fn ? fn() : NULL);
}

void *SteamUser(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUser");
	return wrap_iface("SteamUser", 2, fn ? fn() : NULL);
}

void *SteamUserStats(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUserStats");
	return wrap_iface("SteamUserStats", 3, fn ? fn() : NULL);
}

void *SteamUtils(void)
{
	typedef void *(__cdecl *fn_t)(void);
	fn_t fn = (fn_t)real_proc("SteamUtils");
	return wrap_iface("SteamUtils", 4, fn ? fn() : NULL);
}
