"""Are the seam lines periodic, and at what period?

A one-pixel line is a row (or column) that departs from both its neighbours
while those neighbours agree with each other. Scoring every row that way and
sorting turns "I can see faint lines" into a list of coordinates, and the
spacing between those coordinates names the mechanism: 64 is the rasteriser's
tile grid, anything else is not.

Run over the offscreen and the backbuffer separately. The composite blit is a
uniform blur, and a uniform blur cannot create periodic structure - so a period
present in the backbuffer must already be in the offscreen, and if it is not,
the composite is not as uniform as it looks.
"""
import struct, sys


def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    n = d[16] // 8
    off = 18 + idlen
    topdown = bool(d[17] & 0x20)
    return d, off, w, h, n, topdown


def loader(path):
    d, off, w, h, n, topdown = read_tga(path)

    def px(x, y):
        yy = y if topdown else (h - 1 - y)
        i = off + yy * w * n + x * n
        return (d[i + 2], d[i + 1], d[i])

    return px, w, h


def score_rows(px, x0, x1, y0, y1):
    out = []
    for y in range(y0 + 1, y1 - 1):
        s = 0
        n = 0
        for x in range(x0, x1, 3):
            a, b, c = px(x, y - 1), px(x, y), px(x, y + 1)
            # Neighbours must agree, or this is real content, not a line.
            if max(abs(a[i] - c[i]) for i in range(3)) > 3:
                continue
            n += 1
            s += max(abs(b[i] - a[i]) for i in range(3))
        if n > 40:
            out.append((s / float(n), y))
    out.sort(reverse=True)
    return out


def score_cols(px, x0, x1, y0, y1):
    out = []
    for x in range(x0 + 1, x1 - 1):
        s = 0
        n = 0
        for y in range(y0, y1, 3):
            a, b, c = px(x - 1, y), px(x, y), px(x + 1, y)
            if max(abs(a[i] - c[i]) for i in range(3)) > 3:
                continue
            n += 1
            s += max(abs(b[i] - a[i]) for i in range(3))
        if n > 40:
            out.append((s / float(n), x))
    out.sort(reverse=True)
    return out


def report(tag, scored, axis):
    print("  top 14 %s by line score:" % axis)
    top = scored[:14]
    for s, c in top:
        print("     %s=%-5d score %5.2f    mod64=%-3d mod32=%-3d" % (axis[0], c, s, c % 64, c % 32))
    coords = sorted(c for _, c in top)
    if len(coords) > 2:
        gaps = [b - a for a, b in zip(coords, coords[1:])]
        print("     sorted:", coords)
        print("     gaps  :", gaps)


def main(path, x0, x1, y0, y1):
    px, w, h = loader(path)
    x1 = min(x1, w - 1)
    y1 = min(y1, h - 1)
    print("== %s  (%dx%d), analysing x %d..%d, y %d..%d" % (path.split('\\')[-1], w, h, x0, x1, y0, y1))
    report(path, score_rows(px, x0, x1, y0, y1), "row")
    report(path, score_cols(px, x0, x1, y0, y1), "col")
    print()


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
