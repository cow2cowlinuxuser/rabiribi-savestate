"""Is the composite's 1-LSB residue random, or is it the seam?

The blit matches a flat 2x2 average of the offscreen to within one LSB
everywhere, but only ~74% of pixels match exactly. If that remaining 1-LSB
error were rounding noise it would be scattered. If it is structured - the same
rows and columns, at a fixed period - then it is not noise, it is the artefact,
and its period names the mechanism that produces it.

Why one LSB is enough to see: the blit samples exactly on a texel boundary, so
both bilinear weights are 128/256. That is the knife edge. Any drift in the
interpolated coordinate tips the weight to 127 or 129 and changes the output,
and a drift that varies systematically down the frame prints a line.
"""
import struct, sys


def loader(path):
    d = open(path, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    n = d[16] // 8
    off = 18 + idlen
    topdown = bool(d[17] & 0x20)

    def px(x, y):
        yy = y if topdown else (h - 1 - y)
        i = off + yy * w * n + x * n
        return (d[i + 2], d[i + 1], d[i])

    return px, w, h


def main(src_path, bb_path):
    src, sw, sh = loader(src_path)
    bb, bw, bh = loader(bb_path)

    def model(x, y):
        acc = [0, 0, 0]
        for dy in (0, 1):
            for dx in (0, 1):
                p = src(x + dx, y + dy)
                for c in range(3):
                    acc[c] += p[c]
        return tuple((v + 2) // 4 for v in acc)

    print("residue of backbuffer against a flat 2x2 average of the offscreen\n")

    rows = []
    for y in range(4, bh - 6):
        diff = 0
        n = 0
        for x in range(4, bw - 6, 5):
            g = bb(x, y)
            m = model(x, y)
            n += 1
            if max(abs(g[c] - m[c]) for c in range(3)) != 0:
                diff += 1
        rows.append((diff / float(n), y))

    avg = sum(r for r, _ in rows) / len(rows)
    print("mean fraction of pixels in a row that differ from the flat model: %.3f" % avg)

    rows.sort(reverse=True)
    print("\nrows where the model fits WORST (most 1-LSB departures):")
    for r, y in rows[:12]:
        print("   y=%-5d differ %5.1f%%   mod64=%-3d" % (y, 100 * r, y % 64))
    print("\nrows where the model fits BEST:")
    for r, y in rows[-8:]:
        print("   y=%-5d differ %5.1f%%   mod64=%-3d" % (y, 100 * r, y % 64))

    # If the residue is periodic, grouping rows by y mod 64 separates them.
    buckets = {}
    for r, y in rows:
        buckets.setdefault(y % 64, []).append(r)
    means = sorted(((sum(v) / len(v), k) for k, v in buckets.items()), reverse=True)
    print("\nmean departure grouped by y mod 64 (top 8 and bottom 4):")
    for m, k in means[:8]:
        print("   phase %-3d  %5.1f%%" % (k, 100 * m))
    print("   ...")
    for m, k in means[-4:]:
        print("   phase %-3d  %5.1f%%" % (k, 100 * m))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
