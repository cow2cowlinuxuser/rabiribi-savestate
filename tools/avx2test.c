/* Same-answer check for the SIMD our renderer relies on.
 *
 *   C:\zig\zig.exe cc -O2 -target x86-windows-gnu -o x86\avx2test.exe tools\avx2test.c
 *
 * Run it on every machine and compare the lines. "exact" rows are IEEE or
 * integer results and must match everywhere; "approx" rows (rcp/rsqrt) are
 * left to the CPU design and are allowed to differ. */
#include <immintrin.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define N 4096

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static unsigned long long xgetbv0(void)
{
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((unsigned long long)hi << 32) | lo;
}

static uint32_t fnv(const void *p, size_t n)
{
	const unsigned char *b = p;
	uint32_t h = 2166136261u;
	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static uint32_t g_seed = 12345;
static uint32_t rnd(void)
{
	g_seed = g_seed * 1103515245u + 12345u;
	return g_seed;
}

static float fin[N], fin2[N], fout[N];
static int32_t iin[N], iout[N];
static uint32_t tex[1024];

static void row(const char *kind, const char *name, const void *p, size_t n)
{
	printf("%-6s %-28s %08X\n", kind, name, fnv(p, n));
}

__attribute__((target("avx2"))) static void run_avx2(void)
{
	int i;
	for (i = 0; i < N; i += 8) {
		__m256i a = _mm256_loadu_si256((const __m256i *)(iin + i));
		__m256i b = _mm256_mullo_epi32(a, _mm256_set1_epi32(0x9E3779B1));
		b = _mm256_xor_si256(_mm256_add_epi32(b, _mm256_srli_epi32(a, 7)), _mm256_slli_epi32(a, 3));
		_mm256_storeu_si256((__m256i *)(iout + i), b);
	}
	row("exact", "avx2 int mul/add/shift", iout, sizeof(iout));

	for (i = 0; i < N; i += 8) {
		__m256 a = _mm256_loadu_ps(fin + i), b = _mm256_loadu_ps(fin2 + i);
		__m256 r = _mm256_add_ps(_mm256_mul_ps(a, b), _mm256_div_ps(a, _mm256_add_ps(b, _mm256_set1_ps(3.0f))));
		_mm256_storeu_ps(fout + i, _mm256_sqrt_ps(_mm256_max_ps(r, _mm256_setzero_ps())));
	}
	row("exact", "avx float mul/add/div/sqrt", fout, sizeof(fout));

	for (i = 0; i < N; i += 8) {
		__m256 a = _mm256_mul_ps(_mm256_loadu_ps(fin + i), _mm256_set1_ps(255.0f));
		__m256i t = _mm256_cvttps_epi32(a), r = _mm256_cvtps_epi32(a);
		_mm256_storeu_si256((__m256i *)(iout + i), _mm256_xor_si256(t, _mm256_slli_epi32(r, 16)));
	}
	row("exact", "avx float->int convert", iout, sizeof(iout));

	for (i = 0; i < N; i += 8) {
		__m256i idx = _mm256_and_si256(_mm256_loadu_si256((const __m256i *)(iin + i)), _mm256_set1_epi32(1023));
		_mm256_storeu_si256((__m256i *)(iout + i), _mm256_i32gather_epi32((const int *)tex, idx, 4));
	}
	row("exact", "avx2 gather (texture fetch)", iout, sizeof(iout));
}

__attribute__((target("avx2,fma"))) static void run_fma(void)
{
	int i;
	for (i = 0; i < N; i += 8) {
		__m256 a = _mm256_loadu_ps(fin + i), b = _mm256_loadu_ps(fin2 + i);
		_mm256_storeu_ps(fout + i, _mm256_fmadd_ps(a, b, _mm256_set1_ps(0.1f)));
	}
	row("exact", "fma (not used by us)", fout, sizeof(fout));
}

__attribute__((target("avx"))) static void run_approx(void)
{
	int i;
	for (i = 0; i < N; i += 8)
		_mm256_storeu_ps(fout + i, _mm256_rcp_ps(_mm256_loadu_ps(fin2 + i)));
	row("approx", "rcp (not used by us)", fout, sizeof(fout));
	for (i = 0; i < N; i += 8)
		_mm256_storeu_ps(fout + i, _mm256_rsqrt_ps(_mm256_loadu_ps(fin2 + i)));
	row("approx", "rsqrt (not used by us)", fout, sizeof(fout));
}

static void run_scalar(void)
{
	int i;
	for (i = 0; i < N; i++) {
		float r = fin[i] * fin2[i] + fin[i] / (fin2[i] + 3.0f);
		volatile float v = r > 0.0f ? r : 0.0f;
		fout[i] = __builtin_sqrtf(v);
	}
	row("exact", "scalar float (same math)", fout, sizeof(fout));
}

int main(void)
{
	unsigned r1[4], r7[4], r0[4], brand[12];
	unsigned long long xcr0 = 0;
	int avx, avx2, fma, avx512, i;
	char name[49];

	cpuid(0x80000000u, 0, r0);
	memset(name, 0, sizeof(name));
	if (r0[0] >= 0x80000004u) {
		cpuid(0x80000002u, 0, brand);
		cpuid(0x80000003u, 0, brand + 4);
		cpuid(0x80000004u, 0, brand + 8);
		memcpy(name, brand, 48);
	}
	cpuid(1, 0, r1);
	cpuid(7, 0, r7);
	if ((r1[2] >> 27) & 1)
		xcr0 = xgetbv0();
	avx = ((r1[2] >> 28) & 1) && (xcr0 & 6) == 6;
	avx2 = avx && ((r7[1] >> 5) & 1);
	fma = avx && ((r1[2] >> 12) & 1);
	avx512 = (xcr0 & 0xE6) == 0xE6 && ((r7[1] >> 16) & 1) && ((r7[1] >> 30) & 1) && ((r7[1] >> 31) & 1);
	printf("cpu    %s\n", name);
	printf("has    avx=%d avx2=%d fma=%d avx512(f,bw,vl)=%d  mxcsr=%08X\n", avx, avx2, fma, avx512,
	       _mm_getcsr());

	for (i = 0; i < N; i++) {
		iin[i] = (int32_t)rnd();
		fin[i] = (float)(rnd() & 0xFFFFFF) / 16777216.0f;
		fin2[i] = (float)(rnd() & 0xFFFF) / 256.0f + 0.25f;
	}
	for (i = 0; i < 1024; i++)
		tex[i] = rnd();

	run_scalar();
	if (avx2)
		run_avx2();
	else
		printf("skip   no AVX2 on this machine\n");
	if (fma)
		run_fma();
	if (avx)
		run_approx();
	return 0;
}
