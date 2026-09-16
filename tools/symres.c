/* Resolve module-relative addresses from a crash report into function names and
 * source lines.
 *
 * The fault reports name their frames as "module+RVA" because that is all a
 * handler inside the faulting process can cheaply know. Turning those back into
 * code means asking dbghelp to load the PDB beside the binary and look the
 * offsets up, which is what this does - against the file on disk, so it can run
 * long after the process that faulted is gone.
 *
 *   symres <path-to-dll-or-exe> <rva> [rva ...]
 *
 * RVAs are hex, with or without a leading 0x. A 64-bit host resolves symbols
 * for a 32-bit image perfectly well; the load base below is arbitrary and only
 * has to be somewhere nothing else lives.
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>

#define FAKE_BASE 0x10000000ull

int main(int argc, char **argv)
{
	HANDLE proc = GetCurrentProcess();
	DWORD64 base;
	int i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <image> <rva> [rva ...]\n", argv[0]);
		return 2;
	}

	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	if (!SymInitialize(proc, NULL, FALSE)) {
		fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
		return 1;
	}

	base = SymLoadModuleEx(proc, NULL, argv[1], NULL, FAKE_BASE, 0, NULL, 0);
	if (!base) {
		fprintf(stderr, "SymLoadModuleEx(%s) failed: %lu\n", argv[1],
			GetLastError());
		return 1;
	}

	for (i = 2; i < argc; i++) {
		char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
		SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
		IMAGEHLP_LINE64 line;
		DWORD64 disp = 0;
		DWORD ldisp = 0;
		unsigned long long rva = strtoull(argv[i], NULL, 16);

		memset(buf, 0, sizeof(buf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = MAX_SYM_NAME;

		printf("+%llX  ", rva);
		if (SymFromAddr(proc, base + rva, &disp, sym))
			printf("%s+%llu", sym->Name, (unsigned long long)disp);
		else
			printf("<no symbol: %lu>", GetLastError());

		memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		if (SymGetLineFromAddr64(proc, base + rva, &ldisp, &line))
			printf("   %s:%lu", line.FileName, line.LineNumber);
		printf("\n");
	}

	SymCleanup(proc);
	return 0;
}
