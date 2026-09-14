/* Ask every API that claims to know how big a monitor is, and print what each
 * one says.
 *
 * The reason this exists: a DPI-unaware process is told a 2560x1440 display at
 * 150% is 1707x960, and the borderless path believed it. The question is
 * whether the true size is reachable without changing the process's DPI
 * awareness, since awareness also shrinks the game in windowed mode and
 * changes what every coordinate in the process means.
 *
 * Two of these calls are documented to bypass the virtualisation entirely:
 * EnumDisplaySettings reads the display mode out of the driver, and
 * GetDeviceCaps(DESKTOPHORZRES) exists specifically so an unaware process can
 * discover the size of the thing it is being scaled onto. If they report true
 * pixels here, the wrapper can use them and leave awareness alone.
 *
 * Built deliberately without a DPI manifest so it sees what the game sees. */
#include <windows.h>
#include <stdio.h>

/* Not in the mingw headers at the version pinned here. */
#ifndef DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
#define DISPLAY_DEVICE_ATTACHED_TO_DESKTOP 0x00000001
#endif

typedef HRESULT(WINAPI *GetDpiForMonitor_t)(HMONITOR, int, UINT *, UINT *);
typedef BOOL(WINAPI *AreDpiAwarenessContextsEqual_t)(HANDLE, HANDLE);
typedef HANDLE(WINAPI *GetThreadDpiAwarenessContext_t)(void);

static int g_n;

static BOOL CALLBACK on_monitor(HMONITOR mon, HDC dc, LPRECT r, LPARAM p)
{
	MONITORINFOEXA mi;
	DEVMODEA dm;
	HDC mdc;
	UINT dx = 0, dy = 0;
	GetDpiForMonitor_t get_dpi;
	HMODULE shc;

	(void)dc;
	(void)r;
	(void)p;

	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi))
		return TRUE;

	printf("\nmonitor %d  %s%s\n", g_n++, mi.szDevice,
	       (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  (primary)" : "");

	/* What the wrapper used, and what it got wrong. */
	printf("  GetMonitorInfo rcMonitor      %4ld x %4ld  at %ld,%ld\n",
	       mi.rcMonitor.right - mi.rcMonitor.left,
	       mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left,
	       mi.rcMonitor.top);

	/* Straight out of the display driver, so no virtualisation applies. This
	 * also carries the refresh rate, which is the other number in question. */
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
		printf("  EnumDisplaySettings CURRENT   %4lu x %4lu  at %ld,%ld  "
		       "%lu Hz  %lu bpp\n",
		       dm.dmPelsWidth, dm.dmPelsHeight, dm.dmPosition.x,
		       dm.dmPosition.y, dm.dmDisplayFrequency, dm.dmBitsPerPel);
	else
		printf("  EnumDisplaySettings CURRENT   failed (%lu)\n", GetLastError());

	/* HORZRES is virtualised the same way GetMonitorInfo is; DESKTOPHORZRES is
	 * not. Printing both makes the scale factor visible as the ratio rather
	 * than as something to be inferred. */
	mdc = CreateDCA(mi.szDevice, mi.szDevice, NULL, NULL);
	if (mdc) {
		int hv = GetDeviceCaps(mdc, HORZRES), vv = GetDeviceCaps(mdc, VERTRES);
		int hp = GetDeviceCaps(mdc, DESKTOPHORZRES);
		int vp = GetDeviceCaps(mdc, DESKTOPVERTRES);

		printf("  GetDeviceCaps HORZRES         %4d x %4d  (virtualised)\n", hv,
		       vv);
		printf("  GetDeviceCaps DESKTOPHORZRES  %4d x %4d  (physical)  "
		       "%d Hz  logpixels %d\n",
		       hp, vp, GetDeviceCaps(mdc, VREFRESH),
		       GetDeviceCaps(mdc, LOGPIXELSX));
		if (hv > 0)
			printf("  implied scale                 %.0f%%\n",
			       100.0 * (double)hp / (double)hv);
		DeleteDC(mdc);
	} else
		printf("  CreateDC on the device failed (%lu)\n", GetLastError());

	shc = LoadLibraryA("shcore.dll");
	get_dpi = shc ? (GetDpiForMonitor_t)(void *)GetProcAddress(shc, "GetDpiForMonitor")
		      : NULL;
	if (get_dpi && get_dpi(mon, 0, &dx, &dy) == S_OK)
		printf("  GetDpiForMonitor EFFECTIVE    %u dpi  (%.0f%%)\n", dx,
		       100.0 * dx / 96.0);
	return TRUE;
}

int main(void)
{
	HMODULE u32 = GetModuleHandleA("user32.dll");
	GetThreadDpiAwarenessContext_t get_ctx;
	AreDpiAwarenessContextsEqual_t eq;
	DISPLAY_DEVICEA dd;
	DWORD i;

	get_ctx = (GetThreadDpiAwarenessContext_t)(void *)GetProcAddress(
		u32, "GetThreadDpiAwarenessContext");
	eq = (AreDpiAwarenessContextsEqual_t)(void *)GetProcAddress(
		u32, "AreDpiAwarenessContextsEqual");
	printf("this probe's own DPI awareness: ");
	if (get_ctx && eq) {
		HANDLE c = get_ctx();
		printf("%s\n", eq(c, (HANDLE)(intptr_t)-1)   ? "UNAWARE (same as the game)"
			       : eq(c, (HANDLE)(intptr_t)-2) ? "SYSTEM AWARE"
			       : eq(c, (HANDLE)(intptr_t)-3) ? "PER-MONITOR AWARE"
			       : eq(c, (HANDLE)(intptr_t)-4) ? "PER-MONITOR AWARE V2"
							     : "something else");
	} else
		printf("unknown (pre-1607 user32)\n");
	printf("virtual screen metrics:        %d x %d  at %d,%d\n",
	       GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
	       GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN));

	EnumDisplayMonitors(NULL, NULL, on_monitor, 0);

	/* The modes the driver will accept, so the 480/240 Hz question has an
	 * answer that does not depend on what the panel happens to be doing now. */
	printf("\nrefresh rates offered at each attached device's current size:\n");
	memset(&dd, 0, sizeof(dd));
	dd.cb = sizeof(dd);
	for (i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++, dd.cb = sizeof(dd)) {
		DEVMODEA cur, m;
		DWORD k;
		int shown = 0;

		if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
			continue;
		memset(&cur, 0, sizeof(cur));
		cur.dmSize = sizeof(cur);
		if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &cur))
			continue;
		printf("  %s (%s) at %lux%lu:", dd.DeviceName, dd.DeviceString,
		       cur.dmPelsWidth, cur.dmPelsHeight);
		memset(&m, 0, sizeof(m));
		m.dmSize = sizeof(m);
		for (k = 0; EnumDisplaySettingsA(dd.DeviceName, k, &m);
		     k++, m.dmSize = sizeof(m)) {
			if (m.dmPelsWidth != cur.dmPelsWidth ||
			    m.dmPelsHeight != cur.dmPelsHeight)
				continue;
			printf(" %lu", m.dmDisplayFrequency);
			shown++;
		}
		printf("%s  <- now %lu Hz\n", shown ? "" : " none", cur.dmDisplayFrequency);
	}
	return 0;
}
