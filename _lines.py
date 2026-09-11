import struct, sys

def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    return d, 18 + idlen, w, h, bpp // 8, bool(desc & 0x20)

d, off, w, h, n, topdown = read_tga(sys.argv[1])
def px(x, y):
    yy = y if topdown else (h - 1 - y)
    i = off + yy*w*n + x*n
    return (d[i+2], d[i+1], d[i])

def runs(flags, lo):
    out, s = [], None
    for i, f in enumerate(flags):
        if f and s is None:
            s = i
        elif not f and s is not None:
            if i - s >= 8:
                out.append((s + lo, i - 1 + lo))
            s = None
    if s is not None and len(flags) - s >= 8:
        out.append((s + lo, len(flags) - 1 + lo))
    return out

print("vertical seam segments (x, y-range):")
for x in [int(v) for v in sys.argv[2].split(',')] if len(sys.argv) > 2 else []:
    flags = []
    for y in range(h):
        a, b, c = px(x-1, y), px(x, y), px(x+1, y)
        near = max(abs(a[i]-c[i]) for i in range(3)) <= 6
        diff = max(abs(b[i]-a[i]) for i in range(3)) >= 4
        flags.append(near and diff)
    for s, e in runs(flags, 0):
        print("   x=%-5d y=%d..%d   (len %d)" % (x, s, e, e - s + 1))

print("horizontal seam segments (y, x-range):")
for y in [int(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else []:
    flags = []
    for x in range(w):
        a, b, c = px(x, y-1), px(x, y), px(x, y+1)
        near = max(abs(a[i]-c[i]) for i in range(3)) <= 6
        diff = max(abs(b[i]-a[i]) for i in range(3)) >= 4
        flags.append(near and diff)
    for s, e in runs(flags, 0):
        print("   y=%-5d x=%d..%d   (len %d)" % (y, s, e, e - s + 1))
