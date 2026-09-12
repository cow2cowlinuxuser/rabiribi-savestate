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
 * account for. This file only has to make the name resolve to us.
 *
 * SysWOW64\xaudio2_9.dll exports ten names, not one. DxLib delay-loads
 * X3DAudioInitialize from the same module it just got XAudio2Create from -
 * on Win10 that entry lives here, not in X3DAudio1_7.dll. The CRT delay-load
 * helper uses its own GetProcAddress, so d3d11's IAT hook never records the
 * miss; the IAT slot stays NULL and the next CALL is execute-at-0. The last
 * such crash was X3DAudioInitialize(SPEAKER_STEREO, 343.5f, handle) at
 * rabiribi.exe+0x4d714, after CreateMasteringVoice had already succeeded.
 * The ordinals below match the real DLL so a bind-by-ordinal hits us too. */
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

static HRESULT do_create(void **pp, UINT32 flags, UINT32 processor, const char *who)
{
	HRESULT(WINAPI * fn)(void **, UINT32, UINT32);
	HMODULE h = impl_mod();
	char line[200];

	fn = h ? (HRESULT(WINAPI *)(void **, UINT32, UINT32))GetProcAddress(h, "xa2_sw_create")
	       : NULL;
	if (!fn) {
		wsprintfA(line, "%s - the wrapper next to us had no xa2_sw_create, so "
				"the game gets nothing",
			  who);
		note(line);
		if (pp)
			*pp = NULL;
		return E_FAIL;
	}
	wsprintfA(line, "%s flags=%u processor=%u - handing it the software engine", who, flags,
		  processor);
	note(line);
	return fn(pp, flags, processor);
}

HRESULT WINAPI XAudio2Create(void **pp, UINT32 flags, UINT32 processor)
{
	return do_create(pp, flags, processor, "XAudio2Create");
}

HRESULT WINAPI XAudio2CreateV2_9(void **pp, UINT32 flags, UINT32 processor)
{
	return do_create(pp, flags, processor, "XAudio2CreateV2_9");
}

HRESULT WINAPI XAudio2CreateWithVersionInfo(void **pp, UINT32 flags, UINT32 processor, UINT32 ver)
{
	char line[120];

	wsprintfA(line, "XAudio2CreateWithVersionInfo ntddi=%08X", ver);
	note(line);
	return do_create(pp, flags, processor, "XAudio2CreateWithVersionInfo");
}

HRESULT WINAPI XAudio2CreateWithSharedContexts(void **pp, UINT32 flags, UINT32 processor,
					       UINT32 extra)
{
	char line[120];

	wsprintfA(line, "XAudio2CreateWithSharedContexts extra=%08X", extra);
	note(line);
	return do_create(pp, flags, processor, "XAudio2CreateWithSharedContexts");
}

static HRESULT notimpl_out(void **pp, const char *who)
{
	note(who);
	if (pp)
		*pp = NULL;
	return E_NOTIMPL;
}

HRESULT WINAPI CreateAudioReverb(void **pp)
{
	return notimpl_out(pp, "CreateAudioReverb - not implemented");
}

HRESULT WINAPI CreateAudioReverbV2_8(void **pp)
{
	return notimpl_out(pp, "CreateAudioReverbV2_8 - not implemented");
}

HRESULT WINAPI CreateAudioVolumeMeter(void **pp)
{
	return notimpl_out(pp, "CreateAudioVolumeMeter - not implemented");
}

HRESULT WINAPI CreateFX(const void *clsid, void **pp, const void *init, UINT32 init_bytes)
{
	(void)clsid;
	(void)init;
	(void)init_bytes;
	return notimpl_out(pp, "CreateFX - not implemented");
}

/* 20 bytes: X3DAUDIO_HANDLE_BYTESIZE. The game owns the blob in its .data. */
#define XA2_X3D_HANDLE_BYTES 20

HRESULT WINAPI X3DAudioInitialize(UINT32 mask, float speed, void *handle)
{
	UINT32 *w = (UINT32 *)handle;
	union {
		float f;
		UINT32 u;
	} conv;
	char line[160];

	conv.f = speed;
	wsprintfA(line, "X3DAudioInitialize mask=%u speed_bits=%08X handle=%08lX", mask, conv.u,
		  (unsigned long)(UINT_PTR)handle);
	note(line);
	if (!handle)
		return E_INVALIDARG;
	ZeroMemory(handle, XA2_X3D_HANDLE_BYTES);
	w[0] = mask;
	w[1] = conv.u;
	w[2] = 0x58334431; /* 'X3D1' so a later Calculate can see we wrote it */
	return S_OK;
}

typedef struct {
	float *matrix;
	float *delays;
	UINT32 src_ch;
	UINT32 dst_ch;
	float lpf_direct;
	float lpf_reverb;
	float reverb;
	float doppler;
	float angle;
	float dist;
	float emit_vel;
	float listen_vel;
} XA2_X3D_DSP;

void WINAPI X3DAudioCalculate(const void *handle, const void *listener, const void *emitter,
			      UINT32 flags, XA2_X3D_DSP *dsp)
{
	static LONG n;
	LONG i = InterlockedIncrement(&n);

	(void)handle;
	(void)listener;
	(void)emitter;
	if (i <= 4) {
		char line[160];

		wsprintfA(line, "X3DAudioCalculate #%ld flags=%08X dsp=%08lX src=%u dst=%u", i,
			  flags, (unsigned long)(UINT_PTR)dsp, dsp ? dsp->src_ch : 0,
			  dsp ? dsp->dst_ch : 0);
		note(line);
	}
	if (!dsp)
		return;
	dsp->lpf_direct = 1.0f;
	dsp->lpf_reverb = 0.75f;
	dsp->reverb = 0.0f;
	dsp->doppler = 1.0f; /* 0 would scale the resampler to silence */
	dsp->angle = 0.0f;
	dsp->dist = 0.0f;
	dsp->emit_vel = 0.0f;
	dsp->listen_vel = 0.0f;
	if (dsp->matrix && dsp->src_ch && dsp->dst_ch && dsp->src_ch <= 32 && dsp->dst_ch <= 32) {
		UINT32 s, d;

		for (d = 0; d < dsp->dst_ch; d++) {
			for (s = 0; s < dsp->src_ch; s++)
				dsp->matrix[dsp->src_ch * d + s] = 0.0f;
			s = d < dsp->src_ch ? d : 0;
			dsp->matrix[dsp->src_ch * d + s] = 1.0f;
		}
	}
	if (dsp->delays && dsp->dst_ch && dsp->dst_ch <= 32) {
		UINT32 d;

		for (d = 0; d < dsp->dst_ch; d++)
			dsp->delays[d] = 0.0f;
	}
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
