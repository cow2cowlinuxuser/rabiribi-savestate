"""Who painted the seam line?

The rasteriser can record, per pixel, which draw last wrote it. That turns the
central ambiguity into a lookup rather than an argument:

  - the seam pixel has a DIFFERENT owner from the rows above and below
      -> it is a boundary between two draws, i.e. a coverage problem
  - the seam pixel has the SAME owner as its neighbours
      -> one draw painted the line itself, i.e. a sampling problem

Those two answers point at opposite halves of the rasteriser, and nothing else
we can measure separates them as cleanly.
"""
import struct, sys


def load_tga(path):
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


def load_did(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'DID1', "not a DID1 file"
    w, h = struct.unpack_from('<ii', d, 4)
    base = 12

    def owner(x, y):
        return struct.unpack_from('<I', d, base + (y * w + x) * 4)[0]

    return owner, w, h


def main(tga, did, x0, x1, y0, y1):
    px, w, h = load_tga(tga)
    owner, ow, oh = load_did(did)
    print("colour %dx%d, owners %dx%d" % (w, h, ow, oh))

    # Rank rows by how strongly they read as a one-pixel line.
    scored = []
    for y in range(y0 + 1, y1 - 1):
        s = 0
        n = 0
        for x in range(x0, x1, 3):
            a, b, c = px(x, y - 1), px(x, y), px(x, y + 1)
            if max(abs(a[i] - c[i]) for i in range(3)) > 4:
                continue
            n += 1
            s += max(abs(b[i] - a[i]) for i in range(3))
        if n > 60:
            scored.append((s / float(n), y))
    scored.sort(reverse=True)

    print("\nstrongest one-pixel rows in the offscreen:")
    for s, y in scored[:6]:
        print("   y=%-5d score %5.2f" % (y, s))

    for s, y in scored[:3]:
        print("\n=== row y=%d (score %.2f) ===" % (y, s))
        same = 0
        diff = 0
        shown = 0
        for x in range(x0, x1):
            a, b, c = px(x, y - 1), px(x, y), px(x, y + 1)
            if max(abs(a[i] - c[i]) for i in range(3)) > 4:
                continue
            if max(abs(b[i] - a[i]) for i in range(3)) < 6:
                continue
            oa, ob, oc = owner(x, y - 1), owner(x, y), owner(x, y + 1)
            if ob == oa and ob == oc:
                same += 1
            else:
                diff += 1
            if shown < 8:
                print("   x=%-5d above%-16s owner %-5d | SEAM%-16s owner %-5d | below%-16s owner %-5d"
                      % (x, str(a), oa, str(b), ob, str(c), oc))
                shown += 1
        tot = same + diff
        if tot:
            print("   -> seam pixels sharing their neighbours' owner: %d/%d (%.0f%%)"
                  % (same, tot, 100.0 * same / tot))
            print("   -> verdict: %s" % ("ONE DRAW painted the line - sampling"
                                         if same > diff else
                                         "BOUNDARY between draws - coverage"))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
