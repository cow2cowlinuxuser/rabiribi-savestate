/* inrec FILE [FILE2] - read a D3D9SW_INPUT recording as a script by frame.
 *
 * One line per change: the frame, the device, and which keys or buttons went
 * down (+) or up (-). Answers that change nothing are counted, not printed.
 * Given a second recording, says where the two first differ - same device,
 * method and answer, call for call - which is what a replay has to match. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	unsigned seq, frame;
	unsigned short dev, method;
	int hr;
	unsigned cb, flags, in, out, len;
} Rec;

static unsigned char *slurp(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
	d = malloc(*n ? *n : 1);
	if (fread(d, 1, *n, f) != (size_t)*n) { fprintf(stderr, "short read %s\n", path); exit(1); }
	fclose(f);
	if (*n < 8 || ((unsigned *)d)[0] != 0x4E494444u || ((unsigned *)d)[1] != 1) {
		fprintf(stderr, "%s is not a version 1 input recording\n", path);
		exit(1);
	}
	return d;
}

static const char *dik(unsigned k)
{
	static char buf[16];
	static const struct { unsigned k; const char *n; } t[] = {
		{ 0x01, "Esc" }, { 0x0E, "Backspace" }, { 0x0F, "Tab" }, { 0x1C, "Enter" }, { 0x1D, "LCtrl" },
		{ 0x2A, "LShift" }, { 0x36, "RShift" }, { 0x38, "LAlt" }, { 0x39, "Space" },
		{ 0x2C, "Z" }, { 0x2D, "X" }, { 0x2E, "C" }, { 0x2F, "V" }, { 0x1E, "A" }, { 0x1F, "S" },
		{ 0x20, "D" }, { 0x11, "W" }, { 0x10, "Q" }, { 0x12, "E" },
		{ 0xC8, "Up" }, { 0xD0, "Down" }, { 0xCB, "Left" }, { 0xCD, "Right" },
		{ 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" }, { 0x3F, "F5" },
		{ 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" },
		{ 0x07, "6" }, { 0x08, "7" }, { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" },
	};
	unsigned i;
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (t[i].k == k) return t[i].n;
	sprintf(buf, "DIK_%02X", k);
	return buf;
}

int main(int argc, char **argv)
{
	long n, pos = 8, m = 0, pos2 = 8;
	unsigned char *d, *d2 = NULL;
	static unsigned char last[64][512];
	static unsigned lastlen[64];
	unsigned long nrec = 0, quiet = 0, lastframe = 0, ndev[64] = { 0 };

	if (argc < 2) { fprintf(stderr, "usage: inrec FILE [FILE2]\n"); return 1; }
	d = slurp(argv[1], &n);
	if (argc > 2) d2 = slurp(argv[2], &m);

	if (d2) {
		while (pos + (long)sizeof(Rec) <= n && pos2 + (long)sizeof(Rec) <= m) {
			const Rec *a = (const Rec *)(d + pos), *b = (const Rec *)(d2 + pos2);
			if (a->dev != b->dev || a->method != b->method || a->cb != b->cb || a->hr != b->hr ||
			    a->out != b->out || a->len != b->len || memcmp(a + 1, b + 1, a->len)) {
				printf("first difference at answer %u: frame %u vs %u, device %u vs %u, method %u vs %u, "
				       "hr %08X vs %08X, %u vs %u item(s)\n",
				       a->seq, a->frame, b->frame, a->dev, b->dev, a->method, b->method, a->hr, b->hr,
				       a->out, b->out);
				return 2;
			}
			nrec++;
			pos += sizeof(Rec) + a->len;
			pos2 += sizeof(Rec) + b->len;
		}
		printf("%lu answer(s) identical; %s\n", nrec,
		       pos < n ? "the first recording goes on longer" : pos2 < m ? "the second recording goes on longer" : "both end together");
		return 0;
	}

	while (pos + (long)sizeof(Rec) <= n) {
		const Rec *r = (const Rec *)(d + pos);
		const unsigned char *p = (const unsigned char *)(r + 1);
		unsigned dv = r->dev < 64 ? r->dev : 63, i;
		char line[2048];
		int any = 0, o = 0;

		if (pos + (long)sizeof(Rec) + (long)r->len > n) { printf("truncated at answer %u\n", r->seq); break; }
		nrec++;
		ndev[dv]++;
		lastframe = r->frame;
		o = sprintf(line, "frame %6u  dev %u  ", r->frame, r->dev);
		if (r->method == 9 && r->len == 256) {
			/* keyboard: 256 key bytes, high bit down */
			for (i = 0; i < 256; i++) {
				int was = lastlen[dv] == 256 && (last[dv][i] & 0x80) != 0, now = (p[i] & 0x80) != 0;
				if (was != now && o < 1900) { o += sprintf(line + o, "%skey@%u(%s) ", now ? "+" : "-", i, dik(i)); any = 1; }
			}
		} else if (r->method == 9 && r->len >= 48 && r->len <= 512) {
			/* DIJOYSTATE/DIJOYSTATE2: axes, sliders and POVs in the first 48
			 * bytes, buttons after */
			for (i = 0; i < 12; i++) {
				int v = ((const int *)p)[i], w = lastlen[dv] == r->len ? ((const int *)last[dv])[i] : 0;
				if (v != w && o < 1900) { o += sprintf(line + o, "axis%u=%d ", i, v); any = 1; }
			}
			for (i = 48; i < r->len && i < 48 + 128; i++) {
				int was = lastlen[dv] == r->len && (last[dv][i] & 0x80) != 0, now = (p[i] & 0x80) != 0;
				if (was != now && o < 1900) { o += sprintf(line + o, "%sbtn%u ", now ? "+" : "-", i - 48); any = 1; }
			}
		} else if (r->method == 10 && r->len) {
			for (i = 0; i < r->out && (i + 1) * r->cb <= r->len && r->cb >= 8 && o < 1900; i++) {
				const unsigned *ev = (const unsigned *)(p + i * r->cb);
				o += sprintf(line + o, "event ofs %u data %u  ", ev[0], ev[1]);
			}
			any = 1;
		} else if (r->hr < 0) {
			o += sprintf(line + o, "method %u failed %08X (and keeps failing until a line says otherwise)", r->method, (unsigned)r->hr);
			any = lastlen[dv] != 0xFFFFFFFFu;
			lastlen[dv] = 0xFFFFFFFFu;
		}
		if (r->hr >= 0 && lastlen[dv] == 0xFFFFFFFFu) lastlen[dv] = 0;
		if (r->method == 9 && r->len && r->len <= 512) { memcpy(last[dv], p, r->len); lastlen[dv] = r->len; }
		if (any) puts(line); else quiet++;
		pos += sizeof(Rec) + r->len;
	}
	printf("\n%lu answer(s) up to frame %lu, %lu changed nothing\n", nrec, lastframe, quiet);
	for (n = 0; n < 64; n++) if (ndev[n]) printf("  device %ld: %lu answer(s)\n", n, ndev[n]);
	return 0;
}
