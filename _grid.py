import sys
from PIL import Image

path, x0, y0, x1, y1 = sys.argv[1], *(int(v) for v in sys.argv[2:6])
im = Image.open(path).convert('RGB')
px = im.load()

# A seam is a one-pixel line that sits uniformly above or below BOTH of its
# neighbours across the whole span. Content edges fail the "both sides" test.
def scan(rows):
    out = []
    lo, hi = (y0 + 1, y1 - 1) if rows else (x0 + 1, x1 - 1)
    span = range(x0, x1) if rows else range(y0, y1)
    for i in range(lo, hi):
        tot = 0.0
        agree = 0
        n = 0
        for j in span:
            if rows:
                c, a, b = px[j, i], px[j, i - 1], px[j, i + 1]
            else:
                c, a, b = px[i, j], px[i - 1, j], px[i + 1, j]
            d = (c[0]+c[1]+c[2])/3.0 - ((a[0]+a[1]+a[2])/3.0 + (b[0]+b[1]+b[2])/3.0)/2.0
            tot += d
            if abs(d) > 1.0:
                agree += 1 if d > 0 else -1
            n += 1
        out.append((i, tot / n, agree / float(n)))
    return out

for name, rows in (('row y', True), ('col x', False)):
    data = scan(rows)
    hits = [(i, m, a) for i, m, a in data if abs(a) > 0.55 and abs(m) > 1.0]
    print('%s: %d uniform lines' % (name, len(hits)))
    for i, m, a in hits[:40]:
        print('   %4d  mean=%+7.2f agree=%+.2f' % (i, m, a))
    if len(hits) > 1:
        print('   gaps:', [b[0] - a[0] for a, b in zip(hits, hits[1:])])
