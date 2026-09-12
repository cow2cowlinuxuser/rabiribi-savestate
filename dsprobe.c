/* Does a same-folder dsound.dll actually win the search from inside the game
 * directory? The stub deployed cleanly and the game still got Windows'
 * DirectSound, and there are three separate explanations for that - the name
 * resolving somewhere else, the folder losing the search, or our DLL failing to
 * load and the loader moving on. Guessing between them is how an evening goes
 * missing, so this asks the loader directly. It has to live in the game folder,
 * because the application directory is the executable's, not the caller's. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void report(const char *what, HMODULE h)
{
	char path[MAX_PATH];

	if (!h) {
		printf("  %-22s FAILED, GetLastError %lu\n", what, GetLastError());
		return;
	}
	if (!GetModuleFileNameA(h, path, MAX_PATH))
		lstrcpynA(path, "(no path)", MAX_PATH);
	printf("  %-22s %s\n", what, path);
}

int main(void)
{
	HMODULE h;
	char self[MAX_PATH], *slash;

	GetModuleFileNameA(NULL, self, MAX_PATH);
	printf("running as: %s\n\n", self);

	printf("before anything is loaded:\n");
	report("GetModuleHandle", GetModuleHandleA("dsound.dll"));

	printf("\nthe name the game uses:\n");
	h = LoadLibraryA("DSound.DLL");
	report("LoadLibrary", h);

	printf("\nand by full path, to prove the file itself is loadable:\n");
	GetModuleFileNameA(NULL, self, MAX_PATH);
	slash = strrchr(self, '\\');
	if (slash) {
		lstrcpynA(slash + 1, "dsound.dll", MAX_PATH - (int)(slash + 1 - self));
		report("LoadLibrary(full)", LoadLibraryA(self));
	}

	printf("\nwhat the game would then resolve:\n");
	h = GetModuleHandleA("dsound.dll");
	if (h) {
		void *p = (void *)GetProcAddress(h, "DirectSoundCreate8");

		printf("  DirectSoundCreate8     %s\n", p ? "found" : "MISSING");
	}
	return 0;
}
