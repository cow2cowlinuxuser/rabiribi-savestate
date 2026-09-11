import struct, sys, collections

def load_tga(p):
    d = open(p, 'rb').read()
    idlen, cmap, imgtype = d[0], d[1], d[2]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    off = 18 + idlen
    assert imgtype == 2 and bpp in (24, 32), (imgtype, bpp)
    n = bpp // 8
    px = []
    for y in range(h):
        row = y if (desc & 0x20) else (h - 1 - y)
        base = off + row * w * n
        px.append(d[base:base + w * n])
    return w, h, px, n

def load_did(p):
    d = open(p, 'rb').read()
    for hdr in (12, 8, 4):
        for pos in (0, 4):
            if pos + 8 > hdr:
                continue
            w, h = struct.unpack_from('<II', d, pos)
            if w and h and len(d) == hdr + w * h * 4:
                return w, h, struct.unpack_from('<%dI' % (w * h), d, hdr)
    raise SystemExit('unrecognised .did header: %r size=%d' % (d[:16], len(d)))

tga = sys.argv[1]
did = sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])

w, h, px, N = load_tga(tga)
print('tga %dx%d' % (w, h))

dw, dh, ids = load_did(did)
print('did %dx%d' % (dw, dh))

# Which draw owns the region?
own = collections.Counter()
for y in range(y0, y1):
    for x in range(x0, x1):
        own[ids[y * dw + x]] += 1
print('owners:', own.most_common(6))

# Row-to-row identity: a point-minified image repeats no rows, but drops them.
# Compare each row against the previous within the region.
prev = None
diffs = []
for y in range(y0, y1):
    row = px[y][x0 * N:x1 * N]
    if prev is not None:
        nd = sum(1 for i in range(0, len(row), N) if row[i:i+3] != prev[i:i+3])
        diffs.append((y, nd))
    prev = row

ident = [y for y, nd in diffs if nd == 0]
print('rows identical to previous: %d of %d' % (len(ident), len(diffs)))
if ident:
    gaps = collections.Counter(b - a for a, b in zip(ident, ident[1:]))
    print('  spacing histogram:', gaps.most_common(8))
    print('  first few:', ident[:20])

# Same for columns
identc = []
for x in range(x0 + 1, x1):
    same = True
    for y in range(y0, y1):
        if px[y][x*N:x*N+3] != px[y][(x-1)*N:(x-1)*N+3]:
            same = False
            break
    if same:
        identc.append(x)
print('cols identical to previous: %d of %d' % (len(identc), x1 - x0 - 1))
if identc:
    gaps = collections.Counter(b - a for a, b in zip(identc, identc[1:]))
    print('  spacing histogram:', gaps.most_common(8))
