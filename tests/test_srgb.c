/* Checks the sRGB tables used by the texture downscaler.
 *
 * The tables are duplicated here rather than shared, because the point is to
 * confirm the CHOSEN SIZES are adequate. A test that imported the real ones
 * could only prove they agree with themselves. */
#include <math.h>
#include <stdio.h>

#define SRGB_LUT_N 16384

static float lin_from_srgb[256];
static unsigned char srgb_from_lin[SRGB_LUT_N];

static void build(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		double c = i / 255.0;

		lin_from_srgb[i] =
			(float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
	}
	for (i = 0; i < SRGB_LUT_N; i++) {
		double l = (double)i / (SRGB_LUT_N - 1);
		double s = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
		int v = (int)(s * 255.0 + 0.5);

		srgb_from_lin[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
	}
}

static unsigned char to_srgb(double l)
{
	int i;

	if (!(l > 0.0))
		return 0;
	if (l >= 1.0)
		return 255;
	i = (int)(l * (SRGB_LUT_N - 1) + 0.5);
	return srgb_from_lin[i];
}

int main(void)
{
	int i, worst = 0, worst_at = -1, fail = 0;

	build();

	/* 1. Round trip. Every 8-bit value must survive sRGB -> linear -> sRGB,
	 * or a texture that is merely COPIED through the filter shifts colour. */
	for (i = 0; i < 256; i++) {
		int back = to_srgb(lin_from_srgb[i]);
		int err = back - i < 0 ? i - back : back - i;

		if (err > worst) {
			worst = err;
			worst_at = i;
		}
	}
	printf("round trip: worst error %d level(s) at input %d\n", worst, worst_at);
	if (worst > 0) {
		printf("FAIL: the reverse table is too coarse\n");
		fail = 1;
	}

	/* 2. A flat colour must average to itself. This is the property the old
	 * gamma-space filter broke only for mixtures, so it should hold either
	 * way - a failure here would mean the tables are simply wrong. */
	for (i = 0; i < 256; i++) {
		double l = lin_from_srgb[i];
		int back = to_srgb((l + l + l + l) / 4.0);

		if (back != i) {
			printf("FAIL: flat %d averaged to %d\n", i, back);
			fail = 1;
			break;
		}
	}
	if (!fail)
		printf("flat average: all 256 levels preserved\n");

	/* 3. The case that motivated the change: half black, half white.
	 * In linear light the answer is 188, not 128. If this prints 128 the
	 * conversion is not happening at all. */
	{
		double mid = (lin_from_srgb[0] + lin_from_srgb[255]) / 2.0;
		int got = to_srgb(mid);

		printf("50%% black/white mixes to %d (sRGB midpoint would be 128)\n", got);
		if (got < 180 || got > 192) {
			printf("FAIL: expected about 188\n");
			fail = 1;
		}
	}

	printf(fail ? "srgb: FAILED\n" : "srgb: all checks passed\n");
	return fail;
}
