/* Where does an ASLR-enabled PE32 actually land?
 *
 * Seven launches of the game in one boot all reported image base 006A0000,
 * which rules out per-launch randomisation but not much else. The question
 * left over is whether that base is fixed for the life of the boot or for
 * something narrower, like a logon session - and that matters, because if it
 * is per boot then cross-session restore within one boot is a much smaller
 * problem than cross-boot, and the two deserve separate treatment.
 *
 * Asking the game costs a Steam launch, a title screen and thirty-five seconds.
 * Asking this costs a millisecond, needs no window, no graphics device, no
 * Steam and no DRM, and can therefore be run at boot, from a scheduled task, or
 * under another account - the places the game cannot go.
 *
 * Built PE32 with DYNAMICBASE so it is subject to the same treatment the game
 * is. It is not the game, and a conclusion drawn here is about the mechanism
 * rather than about rabiribi.exe; but the mechanism is the thing in doubt. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	HMODULE self = GetModuleHandleW(NULL);
	DWORD sess = 0;
	int i;

	ProcessIdToSessionId(GetCurrentProcessId(), &sess);
	printf("image %08lX  pid %lu  session %lu\n", (unsigned long)(UINT_PTR)self,
	       (unsigned long)GetCurrentProcessId(), (unsigned long)sess);

	/* Any module named on the command line is loaded and reported too, so the
	 * same run can say where our own wrappers land - they carry DYNAMICBASE as
	 * well, and a restore has to reconcile their addresses just as much as the
	 * executable's. */
	for (i = 1; i < argc; i++) {
		HMODULE h = LoadLibraryA(argv[i]);
		if (h)
			printf("  %-16s %08lX\n", argv[i], (unsigned long)(UINT_PTR)h);
		else
			printf("  %-16s failed, %lu\n", argv[i], GetLastError());
	}
	return 0;
}
