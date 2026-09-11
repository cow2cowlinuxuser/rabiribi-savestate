import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert('RGB')
W, H = im.size
px = im.load()
B = 16

hits = []
for by in range(0, H - B, B):
    for bx in range(0, W - B, B):
        se = so = 0.0
        ne = no = 0
        for y in range(by, by + B):
            for x in range(bx, bx + B):
                c = px[x, y]
                v = (c[0] + c[1] + c[2]) / 3.0
                if (x + y) & 1:
                    so += v; no += 1
                else:
                    se += v; ne += 1
        d = abs(se / ne - so / no)
        if d > 3.0:
            hits.append((d, bx, by))

hits.sort(reverse=True)
print('%s %dx%d: %d blocks with checkerboard parity split > 3' % (sys.argv[1], W, H, len(hits)))
for d, bx, by in hits[:30]:
    print('   block (%4d,%4d)  parity delta = %.1f' % (bx, by, d))
