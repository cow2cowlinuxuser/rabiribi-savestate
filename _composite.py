"""What does the composite blit actually do to the frame?

The backbuffer is produced by drawing one textured quad that samples the
offscreen surface. If that blit were the identity - 1:1, point sampled, aligned
- the backbuffer would equal the offscreen pixel for pixel. It does not have to
be, and the difference is measurable rather than arguable: for each candidate
mapping, count how many pixels it explains exactly.
"""
import struct, sys


def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    n = bpp // 8
    off = 18 + idlen
    topdown = bool(desc & 0x20)

    def px(x, y):
        yy = y if topdown else (h - 1 - y)
        i = off + yy * w * n + x * n
        return (d[i + 2], d[i + 1], d[i])

    return px, w, h


def main(src_path, bb_path):
    src, sw, sh = read_tga(src_path)
    bb, bw, bh = read_tga(bb_path)
    print("offscreen %dx%d   backbuffer %dx%d" % (sw, sh, bw, bh))

    # Candidate mappings for backbuffer pixel i, sampling the offscreen.
    def ident(x, y):
        return src(x, y)

    def shift1(x, y):
        return src(x + 1, y)

    def avg01(x, y):
        a, b = src(x, y), src(x + 1, y)
        return tuple((a[c] + b[c] + 1) // 2 for c in range(3))

    def avg_prev(x, y):
        a, b = src(x - 1, y), src(x, y)
        return tuple((a[c] + b[c] + 1) // 2 for c in range(3))

    # Bilinear 50/50 in both axes, which is what a half-texel-off 1:1 blit does.
    def avg2d(x, y):
        acc = [0, 0, 0]
        for dy in (0, 1):
            for dx in (0, 1):
                p = src(x + dx, y + dy)
                for c in range(3):
                    acc[c] += p[c]
        return tuple((v + 2) // 4 for v in acc)

    cands = [("identity          bb[i]=src[i]", ident),
             ("shift by one      bb[i]=src[i+1]", shift1),
             ("avg(i, i+1)       half-texel right", avg01),
             ("avg(i-1, i)       half-texel left", avg_prev),
             ("avg 2x2 at (i,j)  half-texel both axes", avg2d)]

    # Sample a grid well inside the frame so edge handling cannot skew it.
    pts = [(x, y) for y in range(8, bh - 8, 7) for x in range(8, bw - 8, 11)]
    print("comparing %d interior pixels\n" % len(pts))

    for name, f in cands:
        exact = 0
        close = 0
        worst = 0
        for (x, y) in pts:
            got = bb(x, y)
            want = f(x, y)
            d = max(abs(got[c] - want[c]) for c in range(3))
            if d == 0:
                exact += 1
            if d <= 1:
                close += 1
            worst = max(worst, d)
        print("%-42s exact %6.2f%%   within 1 %6.2f%%   worst %d"
              % (name, 100.0 * exact / len(pts), 100.0 * close / len(pts), worst))

    # Show a concrete row so the mapping is visible, not just scored.
    y = bh // 2
    print("\nrow y=%d, x=200..207" % y)
    print("  offscreen :", [src(x, y) for x in range(200, 208)])
    print("  backbuffer:", [bb(x, y) for x in range(200, 208)])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
