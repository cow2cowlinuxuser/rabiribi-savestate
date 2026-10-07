/* A module xfuzz can load at a different address in each launch, standing in
 * for PhysX, XAudio2_7 or any other DLL the OS places. */
#include <windows.h>

__declspec(dllexport) volatile DWORD xfm_data[64];

__declspec(dllexport) int xfm_fn(int x)
{
	return x * 3 + 1;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
	(void)h, (void)why, (void)r;
	return TRUE;
}
