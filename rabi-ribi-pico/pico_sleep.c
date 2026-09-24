/* Window-subsystem sleeper. No console, so Windows does not create a conhost
 * beside it — a conhost would be a second process and would trip the job's
 * active-process limit before the test ever ran.
 *
 * --sleep           block until the job kills us
 * --breakaway PATH  try CREATE_BREAKAWAY_FROM_JOB and write the result
 * --nest PATH       start a --sleep child inside the same job and write its pid
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void write_result(const wchar_t *path, const char *text)
{
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD n;
	if (h == INVALID_HANDLE_VALUE)
		return;
	WriteFile(h, text, (DWORD)lstrlenA(text), &n, NULL);
	CloseHandle(h);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	wchar_t line[1024];
	wchar_t *arg;

	(void)inst;
	(void)prev;
	(void)cmd;
	(void)show;
	GetCommandLineW();
	lstrcpynW(line, GetCommandLineW(), 1024);
	arg = wcsstr(line, L"--breakaway");
	if (arg) {
		wchar_t *path = arg + 11;
		wchar_t self[MAX_PATH];
		STARTUPINFOW si;
		PROCESS_INFORMATION pi;
		wchar_t child[MAX_PATH + 16];

		while (*path == L' ')
			path++;
		GetModuleFileNameW(NULL, self, MAX_PATH);
		wsprintfW(child, L"\"%s\" --sleep", self);
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		memset(&pi, 0, sizeof(pi));
		if (CreateProcessW(NULL, child, NULL, NULL, FALSE,
				   CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED | CREATE_NO_WINDOW,
				   NULL, NULL, &si, &pi)) {
			TerminateProcess(pi.hProcess, 1);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			write_result(path, "breakaway-succeeded\n");
		} else {
			write_result(path, "breakaway-refused\n");
		}
		return 0;
	}
	arg = wcsstr(line, L"--nest");
	if (arg) {
		wchar_t *path = arg + 6;
		wchar_t self[MAX_PATH];
		STARTUPINFOW si;
		PROCESS_INFORMATION pi;
		wchar_t child[MAX_PATH + 16];
		char lineout[64];

		while (*path == L' ')
			path++;
		GetModuleFileNameW(NULL, self, MAX_PATH);
		wsprintfW(child, L"\"%s\" --sleep", self);
		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		memset(&pi, 0, sizeof(pi));
		if (!CreateProcessW(NULL, child, NULL, NULL, FALSE, CREATE_NO_WINDOW,
				    NULL, NULL, &si, &pi)) {
			write_result(path, "nest-failed\n");
			return 1;
		}
		wsprintfA(lineout, "child %lu\n", pi.dwProcessId);
		write_result(path, lineout);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		Sleep(60000);
		return 0;
	}
	Sleep(60000);
	return 0;
}
