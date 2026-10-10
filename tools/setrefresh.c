/* setrefresh HZ  - put the primary display on HZ at its current resolution,
 *                  for this session only (the saved setting is untouched)
 * setrefresh     - back to the saved setting */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	DEVMODEA dm = { 0 };
	LONG r;

	if (argc < 2) {
		r = ChangeDisplaySettingsA(NULL, 0);
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		printf("restored saved display setting: %lux%lu at %lu Hz (result %ld)\n", dm.dmPelsWidth, dm.dmPelsHeight,
		       dm.dmDisplayFrequency, r);
		return r != DISP_CHANGE_SUCCESSFUL;
	}
	dm.dmSize = sizeof(dm);
	EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
	printf("was %lux%lu at %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
	dm.dmDisplayFrequency = (DWORD)atoi(argv[1]);
	dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	r = ChangeDisplaySettingsA(&dm, 0);
	EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
	printf("now %lux%lu at %lu Hz (result %ld)\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, r);
	return r != DISP_CHANGE_SUCCESSFUL;
}
