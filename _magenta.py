import struct, sys, os
from collections import Counter

def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    return d, 18 + idlen, w, h, bpp // 8, bool(desc & 0x20)

for path in sys.argv[1:]:
    d, off, w, h, n, topdown = read_tga(path)
    def px(x, y):
        yy = y if topdown else (h - 1 - y)
        i = off + yy*w*n + x*n
        return (d[i+2], d[i+1], d[i])
    hits = []
    colours = Counter()
    for y in range(h):
        for x in range(w):
            r, g, b = px(x, y)
            # strongly magenta: red and blue high, green clearly lower
            if r > 150 and b > 150 and g + 60 < min(r, b):
                hits.append((x, y))
                colours[(r, g, b)] += 1
    print("== %s (%dx%d): %d magenta pixels" % (os.path.basename(path), w, h, len(hits)))
    for c, k in colours.most_common(6):
        print("     rgb%-18s x%d" % (str(c), k))
    if hits:
        xs = [p[0] for p in hits]; ys = [p[1] for p in hits]
        print("     extent x=%d..%d y=%d..%d" % (min(xs), max(xs), min(ys), max(ys)))
        # group into horizontal runs to see if they are dashes
        hits_set = set(hits)
        runs = []
        seen = set()
        for (x, y) in hits:
            if (x, y) in seen: continue
            n2 = 0
            while (x + n2, y) in hits_set:
                seen.add((x + n2, y)); n2 += 1
            runs.append((x, y, n2))
        runs.sort(key=lambda r: -r[2])
        print("     %d runs; longest:" % len(runs))
        for x, y, ln in runs[:10]:
            print("        x=%-5d y=%-5d len=%d" % (x, y, ln))
    print()
