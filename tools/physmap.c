/* physmap - which physical pages back a process's virtual pages.
 *
 *   physmap selftest            map a buffer of our own and find it again
 *   physmap watch OUT N GAMEDIR wait for default.exe N times; when its slot is
 *                               written, record every private page of it below
 *                               4 GB as "va pfn" in OUT_<i>.txt
 *   physmap compare A B         how many virtual pages sit on the same
 *                               physical page in both captures
 *
 * Windows keeps the page-to-owner map in the kernel's PFN database. The one
 * way to read it from user mode is the query RAMMap uses: the Superfetch PFN
 * query behind NtQuerySystemInformation, admin only. The layouts below are the
 * undocumented ones in public use; selftest is there to prove they hold on
 * this build before any game capture is believed. */
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

#define SystemSuperfetchInformation 79
#define SF_VERSION 45
#define SF_MAGIC 0x6B756843 /* 'kuhC' */
enum { SuperfetchPfnQuery = 6, SuperfetchMemoryRangesQuery = 17 };

typedef struct { ULONG Version, Magic, InfoClass; PVOID Data; ULONG Length; } SfInfo;
typedef struct { ULONG_PTR BasePfn, PageCount; } PfRange;
typedef struct { ULONG Version, RangeCount; PfRange Ranges[1]; } PfRangesV1;
typedef struct { ULONG Version, Flags, RangeCount; PfRange Ranges[1]; } PfRangesV2;
typedef struct { ULONGLONG u1; ULONG_PTR PageFrameIndex; ULONG_PTR u2; } PfnId;
typedef struct {
	ULONG Version, RequestFlags;
	ULONG_PTR PfnCount;
	ULONG_PTR MemInfo[22];	/* SYSTEM_MEMORY_LIST_INFORMATION */
	PfnId PageData[1];
} PfnReq;

typedef NTSTATUS(NTAPI *QsiFn)(ULONG, PVOID, ULONG, PULONG);
static QsiFn Qsi;

static int priv(const char *name)
{
	HANDLE t;
	TOKEN_PRIVILEGES tp;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return 0;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!LookupPrivilegeValueA(NULL, name, &tp.Privileges[0].Luid)) return 0;
	AdjustTokenPrivileges(t, FALSE, &tp, 0, NULL, NULL);
	CloseHandle(t);
	return GetLastError() == ERROR_SUCCESS;
}

static NTSTATUS sf(ULONG cls, void *data, ULONG len)
{
	SfInfo s = { SF_VERSION, SF_MAGIC, cls, data, len };
	ULONG ret = 0;
	return Qsi(SystemSuperfetchInformation, &s, sizeof(s), &ret);
}

static PfRange *ranges(ULONG *n)
{
	static unsigned char buf[1 << 16];
	PfRangesV2 *v2 = (PfRangesV2 *)buf;
	PfRangesV1 *v1 = (PfRangesV1 *)buf;
	NTSTATUS st;
	memset(buf, 0, sizeof(buf));
	v2->Version = 2;
	if ((st = sf(SuperfetchMemoryRangesQuery, buf, sizeof(buf))) >= 0) { *n = v2->RangeCount; return v2->Ranges; }
	memset(buf, 0, sizeof(buf));
	v1->Version = 1;
	if ((st = sf(SuperfetchMemoryRangesQuery, buf, sizeof(buf))) >= 0) { *n = v1->RangeCount; return v1->Ranges; }
	fprintf(stderr, "memory ranges query failed: %08lX (run elevated?)\n", (unsigned long)st);
	return NULL;
}

/* Calls fn for every process-private page: owner key, virtual address, PFN. */
typedef void (*PageFn)(ULONGLONG key, ULONG_PTR va, ULONG_PTR pfn, void *ctx);
#define BATCH 65536
static int scan(PageFn fn, void *ctx)
{
	ULONG nr, r;
	PfRange *rg = ranges(&nr);
	PfnReq *q = malloc(sizeof(PfnReq) + BATCH * sizeof(PfnId));
	if (!rg || !q) return 0;
	for (r = 0; r < nr; r++) {
		ULONG_PTR done = 0;
		while (done < rg[r].PageCount) {
			ULONG_PTR n = rg[r].PageCount - done, k;
			ULONG len;
			NTSTATUS st;
			if (n > BATCH) n = BATCH;
			len = (ULONG)(sizeof(PfnReq) + n * sizeof(PfnId));
			memset(q, 0, len);
			q->Version = 1;
			q->RequestFlags = 1;
			q->PfnCount = n;
			for (k = 0; k < n; k++) q->PageData[k].PageFrameIndex = rg[r].BasePfn + done + k;
			if ((st = sf(SuperfetchPfnQuery, q, len)) < 0) {
				fprintf(stderr, "PFN query failed at %llX: %08lX\n", (unsigned long long)(rg[r].BasePfn + done), (unsigned long)st);
				return 0;
			}
			for (k = 0; k < n; k++) {
				const PfnId *p = &q->PageData[k];
				unsigned use = (unsigned)(p->u1 & 0xF), list = (unsigned)((p->u1 >> 4) & 7);
				/* use 0: process private; list 6: active, in a working set */
				if (use == 0 && list == 6)
					fn((p->u1 >> 9) & ((1ull << 48) - 1), p->u2 & ~(ULONG_PTR)0xFFF, p->PageFrameIndex, ctx);
			}
			done += n;
		}
	}
	free(q);
	return 1;
}

/* ---- selftest */
typedef struct { ULONG_PTR lo, hi; ULONGLONG key[16]; unsigned n[16], nk; unsigned hits; } Self;
static void self1(ULONGLONG key, ULONG_PTR va, ULONG_PTR pfn, void *ctx)
{
	Self *s = ctx;
	unsigned i;
	if (va < s->lo || va >= s->hi) return;
	s->hits++;
	for (i = 0; i < s->nk && s->key[i] != key; i++);
	if (i == s->nk && s->nk < 16) { s->key[i] = key; s->n[i] = 0; s->nk++; }
	if (i < 16) s->n[i]++;
}

static int selftest(void)
{
	SIZE_T sz = 64u << 20, i;
	unsigned char *b = VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	static Self s;
	unsigned k;
	for (i = 0; i < sz; i += 4096) b[i] = 1;
	s.lo = (ULONG_PTR)b; s.hi = s.lo + sz;
	if (!scan(self1, &s)) return 1;
	printf("selftest: %u of %u pages of our own %p buffer found at their virtual addresses\n", s.hits, (unsigned)(sz / 4096), b);
	for (k = 0; k < s.nk; k++) printf("  owner key %llX: %u page(s)\n", s.key[k], s.n[k]);
	return s.hits * 10 < (unsigned)(sz / 4096) * 9;
}

/* ---- capture */
typedef struct { ULONGLONG key; FILE *f; unsigned long n; } Cap;
static ULONGLONG g_key_votes_key[64];
static unsigned g_key_votes_n[64], g_nkeys;
static void vote(ULONGLONG key, ULONG_PTR va, ULONG_PTR pfn, void *ctx)
{
	unsigned i;
	/* the game's pinned arenas: 10000000 (48 MB) and 14000000 (704 MB) */
	if (!((va >= 0x10000000 && va < 0x13000000) || (va >= 0x14000000 && va < 0x40000000))) return;
	for (i = 0; i < g_nkeys && g_key_votes_key[i] != key; i++);
	if (i == g_nkeys && g_nkeys < 64) { g_key_votes_key[i] = key; g_key_votes_n[i] = 0; g_nkeys++; }
	if (i < 64) g_key_votes_n[i]++;
}
static void cap1(ULONGLONG key, ULONG_PTR va, ULONG_PTR pfn, void *ctx)
{
	Cap *c = ctx;
	if (key != c->key || va >= 0x100000000ull) return;
	fprintf(c->f, "%08llX %llX\n", (unsigned long long)va, (unsigned long long)pfn);
	c->n++;
}

static DWORD find_game(void)
{
	HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 e = { sizeof(e) };
	DWORD pid = 0;
	if (Process32First(s, &e)) do { if (!_stricmp(e.szExeFile, "default.exe")) pid = e.th32ProcessID; } while (!pid && Process32Next(s, &e));
	CloseHandle(s);
	return pid;
}

static int watch(const char *out, int runs, const char *gamedir)
{
	char bin[MAX_PATH], path[MAX_PATH];
	int r;
	sprintf(bin, "%s\\d3d9sw_slot0.bin", gamedir);
	for (r = 1; r <= runs; r++) {
		DWORD pid;
		HANDLE h;
		FILETIME ct, x1, x2, x3;
		WIN32_FILE_ATTRIBUTE_DATA fa;
		unsigned i, best = 0;
		Cap c;
		printf("run %d: waiting for default.exe\n", r);
		fflush(stdout);
		while (!(pid = find_game())) Sleep(200);
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
		GetProcessTimes(h, &ct, &x1, &x2, &x3);
		printf("run %d: pid %lu, waiting for its save\n", r, pid);
		fflush(stdout);
		for (;;) {
			if (WaitForSingleObject(h, 200) == WAIT_OBJECT_0) { printf("run %d: game exited before saving\n", r); break; }
			if (GetFileAttributesExA(bin, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &ct) > 0) {
				g_nkeys = 0;
				if (!scan(vote, NULL)) return 1;
				for (i = 1; i < g_nkeys; i++) if (g_key_votes_n[i] > g_key_votes_n[best]) best = i;
				if (!g_nkeys) { printf("run %d: no pages in the pinned arenas found\n", r); break; }
				c.key = g_key_votes_key[best];
				c.n = 0;
				sprintf(path, "%s_%d.txt", out, r);
				c.f = fopen(path, "w");
				if (!scan(cap1, &c)) return 1;
				fclose(c.f);
				printf("run %d: owner key %llX (%u pages in the pinned arenas; next best key has %u); %lu pages written to %s\n", r,
				       c.key, g_key_votes_n[best], g_nkeys > 1 ? g_key_votes_n[best == 0 ? 1 : 0] : 0, c.n, path);
				break;
			}
		}
		fflush(stdout);
		WaitForSingleObject(h, INFINITE);
		CloseHandle(h);
	}
	return 0;
}

/* ---- compare */
typedef struct { unsigned va; unsigned long long pfn; } Pg;
static int pg_cmp(const void *a, const void *b)
{
	unsigned x = ((const Pg *)a)->va, y = ((const Pg *)b)->va;
	return x < y ? -1 : x > y;
}
static Pg *load_cap(const char *path, size_t *n)
{
	FILE *f = fopen(path, "r");
	size_t cap = 1 << 20;
	Pg *p = malloc(cap * sizeof(Pg));
	unsigned va;
	unsigned long long pfn;
	*n = 0;
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	while (fscanf(f, "%X %llX", &va, &pfn) == 2) {
		if (*n == cap) p = realloc(p, (cap *= 2) * sizeof(Pg));
		p[*n].va = va; p[(*n)++].pfn = pfn;
	}
	fclose(f);
	qsort(p, *n, sizeof(Pg), pg_cmp);
	return p;
}

static int compare(const char *a, const char *b)
{
	size_t na, nb, i = 0, j = 0, both = 0, same = 0, same_mb = 0, same_bit[4] = { 0 };
	Pg *A = load_cap(a, &na), *B = load_cap(b, &nb);
	while (i < na && j < nb) {
		if (A[i].va < B[j].va) i++;
		else if (A[i].va > B[j].va) j++;
		else {
			unsigned long long x = A[i].pfn, y = B[j].pfn;
			both++;
			if (x == y) same++;
			if (x >> 8 == y >> 8) same_mb++;
			/* single physical address bits often in channel and bank hashes:
			 * 8, 9, 12, 13 (address bits, so PFN bits minus 12 for >= 12) */
			if (((x ^ y) & 1) == 0) same_bit[0]++;	/* PA bit 12 */
			if (((x ^ y) & 2) == 0) same_bit[1]++;	/* PA bit 13 */
			if (((x ^ y) & 0x100) == 0) same_bit[2]++;	/* PA bit 20 */
			if (((x ^ y) & 0xFF) == 0) same_bit[3]++;	/* PA bits 12-19 together */
			i++; j++;
		}
	}
	printf("%zu resident page(s) in the first capture, %zu in the second, %zu at the same virtual address in both\n", na, nb, both);
	printf("  on the same physical page:            %zu (%.3f%%)\n", same, both ? 100.0 * same / both : 0);
	printf("  in the same 1 MB of physical memory:  %zu (%.3f%%)\n", same_mb, both ? 100.0 * same_mb / both : 0);
	printf("  physical address bit 12 agrees:       %.1f%% (chance: 50%%)\n", both ? 100.0 * same_bit[0] / both : 0);
	printf("  physical address bit 13 agrees:       %.1f%% (chance: 50%%)\n", both ? 100.0 * same_bit[1] / both : 0);
	printf("  physical address bit 20 agrees:       %.1f%% (chance: 50%%)\n", both ? 100.0 * same_bit[2] / both : 0);
	printf("  physical address bits 12-19 all agree: %.2f%% (chance: 0.39%%)\n", both ? 100.0 * same_bit[3] / both : 0);
	return 0;
}

int main(int argc, char **argv)
{
	Qsi = (QsiFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
	if (argc >= 2 && !strcmp(argv[1], "compare") && argc == 4) return compare(argv[2], argv[3]);
	if (!priv("SeProfileSingleProcessPrivilege") || !priv("SeDebugPrivilege"))
		fprintf(stderr, "could not enable the profiling and debug privileges - run elevated\n");
	if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest();
	if (argc == 5 && !strcmp(argv[1], "watch")) return watch(argv[2], atoi(argv[3]), argv[4]);
	fprintf(stderr, "usage: physmap selftest | watch OUT N GAMEDIR | compare A B\n");
	return 1;
}
