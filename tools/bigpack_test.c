/* Can a 32-bit process own 16 GB of backing store and reach all of it through
 * small views? Two backings: a pagefile section (commit charged up front, no
 * pages until touched) and a sparse file on disk that outlives the process.
 *
 *   bigpack_test pagefile
 *   bigpack_test file D:\ddpr-pack\bigpack_test.pack
 *   bigpack_test reopen D:\ddpr-pack\bigpack_test.pack   (checks what "file" wrote)
 *   bigpack_test banks D:\ddpr-pack\bigpack_test.pack    (64 MB banks through fixed slots)
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#define GB (1024ull * 1024ull * 1024ull)
#define TOTAL (16ull * GB)
#define VIEW (64u * 1024u * 1024u)

static const unsigned long long kOffs[] = { 0, 4 * GB, 8 * GB, 12 * GB, TOTAL - VIEW };
#define NOFF (sizeof(kOffs) / sizeof(kOffs[0]))

static unsigned long long commit_mb(void)
{
	PERFORMANCE_INFORMATION pi = { sizeof(pi) };

	GetPerformanceInfo(&pi, sizeof(pi));
	return (unsigned long long)pi.CommitTotal * pi.PageSize >> 20;
}

static void proc_line(const char *when)
{
	PROCESS_MEMORY_COUNTERS_EX pm = { sizeof(pm) };

	GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
	printf("  %-28s system commit %llu MB | process private %lu MB, working set %lu MB\n", when,
	       commit_mb(), (unsigned long)(pm.PrivateUsage >> 20),
	       (unsigned long)(pm.WorkingSetSize >> 20));
}

static unsigned pattern(unsigned i, unsigned at)
{
	return 0xB16B0000u ^ (i << 8) ^ at;
}

/* Map each window, stamp its first and last dword, unmap; then map them again in
 * reverse order, at whatever address Windows picks, and check every stamp. */
static int walk(HANDLE sec, int write)
{
	unsigned i;
	int bad = 0;

	for (i = 0; write && i < NOFF; i++) {
		unsigned *v = (unsigned *)MapViewOfFile(sec, FILE_MAP_WRITE, (DWORD)(kOffs[i] >> 32),
							(DWORD)kOffs[i], VIEW);
		if (!v) {
			printf("  map for write at %llu GB failed, error %lu\n", kOffs[i] / GB,
			       GetLastError());
			return 0;
		}
		v[0] = pattern(i, 0);
		v[VIEW / 4 - 1] = pattern(i, 1);
		printf("  wrote window at offset %6.2f GB through view %p\n",
		       (double)kOffs[i] / GB, (void *)v);
		UnmapViewOfFile(v);
	}
	for (i = NOFF; i-- > 0;) {
		unsigned *v = (unsigned *)MapViewOfFile(sec, FILE_MAP_READ, (DWORD)(kOffs[i] >> 32),
							(DWORD)kOffs[i], VIEW);
		int ok;

		if (!v) {
			printf("  map for read at %llu GB failed, error %lu\n", kOffs[i] / GB,
			       GetLastError());
			return 0;
		}
		ok = v[0] == pattern(i, 0) && v[VIEW / 4 - 1] == pattern(i, 1);
		bad += !ok;
		printf("  read  window at offset %6.2f GB through view %p: %s\n",
		       (double)kOffs[i] / GB, (void *)v, ok ? "ok" : "MISMATCH");
		UnmapViewOfFile(v);
	}
	return !bad;
}

static int run_pagefile(void)
{
	HANDLE sec;
	int ok;

	printf("pagefile-backed section, %llu GB, SEC_COMMIT\n", TOTAL / GB);
	proc_line("before");
	sec = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT,
				 (DWORD)(TOTAL >> 32), (DWORD)TOTAL, NULL);
	if (!sec) {
		printf("  CreateFileMapping failed, error %lu\n", GetLastError());
		return 1;
	}
	proc_line("section created");
	ok = walk(sec, 1);
	proc_line("after the windows");
	CloseHandle(sec);
	proc_line("section closed");
	printf("pagefile: %s\n", ok ? "PASS" : "FAIL");
	return !ok;
}

static HANDLE open_pack(const wchar_t *path, int create, HANDLE *file)
{
	HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			       create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE sec;
	DWORD got;
	LARGE_INTEGER end;

	if (f == INVALID_HANDLE_VALUE) {
		printf("  CreateFile failed, error %lu\n", GetLastError());
		return NULL;
	}
	if (create) {
		if (!DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL))
			printf("  could not mark sparse, error %lu - the file will take real space\n",
			       GetLastError());
		end.QuadPart = (LONGLONG)TOTAL;
		if (!SetFilePointerEx(f, end, NULL, FILE_BEGIN) || !SetEndOfFile(f)) {
			printf("  could not size the file, error %lu\n", GetLastError());
			CloseHandle(f);
			return NULL;
		}
	}
	sec = CreateFileMappingW(f, NULL, PAGE_READWRITE, (DWORD)(TOTAL >> 32), (DWORD)TOTAL, NULL);
	if (!sec) {
		printf("  CreateFileMapping failed, error %lu\n", GetLastError());
		CloseHandle(f);
		return NULL;
	}
	*file = f;
	return sec;
}

static void disk_line(const wchar_t *path)
{
	DWORD hi = 0, lo = GetCompressedFileSizeW(path, &hi);

	printf("  on disk: %llu MB actually allocated of a %llu GB file\n",
	       (((unsigned long long)hi << 32) | lo) >> 20, TOTAL / GB);
}

static int run_file(const wchar_t *path, int create)
{
	HANDLE f, sec;
	int ok;

	printf("file-backed section, %llu GB, %s\n", TOTAL / GB,
	       create ? "new sparse file" : "reopening what an earlier run wrote");
	proc_line("before");
	sec = open_pack(path, create, &f);
	if (!sec)
		return 1;
	proc_line("section created");
	ok = walk(sec, create);
	proc_line("after the windows");
	CloseHandle(sec);
	FlushFileBuffers(f);
	CloseHandle(f);
	disk_line(path);
	printf("file %s: %s\n", create ? "write" : "reopen", ok ? "PASS" : "FAIL");
	return !ok;
}

/* Banks through a fixed arena: one placeholder reservation split into SLOTS
 * slots, and every 64 MB bank of the file swapped through them in turn. A slot
 * stays a placeholder between unmap and map, so nothing else can take it. */
#define SLOTS 6u
#define ARENA_BASE 0x40000000u
#define NBANK ((unsigned)(TOTAL / VIEW))
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

typedef PVOID(WINAPI *PFN_VA2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *PFN_MV3)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL(WINAPI *PFN_UV2)(HANDLE, PVOID, ULONG);

static int run_banks(const wchar_t *path)
{
	HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
	PFN_VA2 va2 = (PFN_VA2)(void *)GetProcAddress(kb, "VirtualAlloc2");
	PFN_MV3 mv3 = (PFN_MV3)(void *)GetProcAddress(kb, "MapViewOfFile3");
	PFN_UV2 uv2 = (PFN_UV2)(void *)GetProcAddress(kb, "UnmapViewOfFile2");
	HANDLE proc = GetCurrentProcess(), f, sec;
	unsigned char *arena;
	int held[SLOTS] = { 0 };
	unsigned i, b, pass, bad = 0, misplaced = 0, stolen = 0;
	LARGE_INTEGER t0, t1, hz;

	printf("banks: %u x %u MB banks of a %llu GB file through %u fixed slots at %08X\n", NBANK,
	       VIEW >> 20, TOTAL / GB, SLOTS, ARENA_BASE);
	if (!va2 || !mv3 || !uv2) {
		printf("  placeholder APIs missing (VirtualAlloc2 %p, MapViewOfFile3 %p, "
		       "UnmapViewOfFile2 %p)\n",
		       (void *)va2, (void *)mv3, (void *)uv2);
		return 1;
	}
	sec = open_pack(path, 1, &f);
	if (!sec)
		return 1;
	arena = (unsigned char *)va2(proc, (PVOID)(uintptr_t)ARENA_BASE, (SIZE_T)SLOTS * VIEW,
				     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
	if (!arena || (uintptr_t)arena != ARENA_BASE) {
		printf("  placeholder reserve failed (%p), error %lu\n", (void *)arena,
		       GetLastError());
		return 1;
	}
	for (i = 0; i + 1 < SLOTS; i++)
		if (!VirtualFree(arena + (size_t)i * VIEW, VIEW,
				 MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
			printf("  could not split slot %u, error %lu\n", i, GetLastError());
			return 1;
		}
	proc_line("arena reserved and split");
	QueryPerformanceFrequency(&hz);
	QueryPerformanceCounter(&t0);
	for (pass = 0; pass < 2; pass++)
		for (b = 0; b < NBANK; b++) {
			unsigned s = b % SLOTS;
			unsigned char *at = arena + (size_t)s * VIEW;
			unsigned *v;

			if (held[s]) {
				if (!uv2(proc, at, MEM_PRESERVE_PLACEHOLDER)) {
					printf("  unmap slot %u failed, error %lu\n", s,
					       GetLastError());
					return 1;
				}
				held[s] = 0;
				/* The empty slot must still be ours. */
				if (VirtualAlloc(at, 65536, MEM_RESERVE, PAGE_READWRITE)) {
					stolen++;
					VirtualFree(at, 0, MEM_RELEASE);
				}
			}
			v = (unsigned *)mv3(sec, proc, at, (ULONG64)b * VIEW, VIEW,
					    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
			if (!v) {
				printf("  map bank %u into slot %u failed, error %lu\n", b, s,
				       GetLastError());
				return 1;
			}
			held[s] = 1;
			misplaced += (unsigned char *)v != at;
			if (pass == 0) {
				v[0] = pattern(b, 0);
				v[VIEW / 4 - 1] = pattern(b, 1);
			} else {
				bad += v[0] != pattern(b, 0) || v[VIEW / 4 - 1] != pattern(b, 1);
			}
		}
	QueryPerformanceCounter(&t1);
	proc_line("after both passes");
	printf("  %u swaps in %.1f ms (%.1f us each); %u bank(s) mismatched, %u mapped off "
	       "their slot, %u time(s) an empty slot could be taken by someone else\n",
	       2 * NBANK, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / hz.QuadPart,
	       (double)(t1.QuadPart - t0.QuadPart) * 1e6 / hz.QuadPart / (2 * NBANK), bad,
	       misplaced, stolen);
	for (i = 0; i < SLOTS; i++)
		if (held[i])
			uv2(proc, arena + (size_t)i * VIEW, 0);
	CloseHandle(sec);
	CloseHandle(f);
	disk_line(path);
	printf("banks: %s\n", !bad && !misplaced && !stolen ? "PASS" : "FAIL");
	return bad || misplaced || stolen;
}

int wmain(int argc, wchar_t **argv)
{
	BOOL wow = FALSE;

	IsWow64Process(GetCurrentProcess(), &wow);
	printf("%d-bit process%s\n", (int)sizeof(void *) * 8, wow ? " under WOW64" : "");
	if (argc >= 2 && !wcscmp(argv[1], L"pagefile"))
		return run_pagefile();
	if (argc >= 3 && !wcscmp(argv[1], L"file"))
		return run_file(argv[2], 1);
	if (argc >= 3 && !wcscmp(argv[1], L"reopen"))
		return run_file(argv[2], 0);
	if (argc >= 3 && !wcscmp(argv[1], L"banks"))
		return run_banks(argv[2]);
	printf("usage: bigpack_test pagefile | file PATH | reopen PATH | banks PATH\n");
	return 2;
}
