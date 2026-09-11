import struct, sys, glob, os

def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    return d, 18 + idlen, w, h, bpp // 8, bool(desc & 0x20)

def analyse(path):
    d, off, w, h, n, topdown = read_tga(path)
    def px(x, y):
        yy = y if topdown else (h - 1 - y)
        i = off + yy*w*n + x*n
        return (d[i+2], d[i+1], d[i])
    # A seam pixel: both neighbours agree closely, centre departs from them.
    vs = []
    for x in range(2, w-2):
        cnt = 0
        for y in range(10, h-10, 2):
            a = px(x-1, y); b = px(x, y); c = px(x+1, y)
            if max(abs(a[i]-c[i]) for i in range(3)) > 3:
                continue
            if max(abs(b[i]-a[i]) for i in range(3)) >= 6:
                cnt += 1
        if cnt >= 15:
            vs.append((x, cnt))
    hs = []
    for y in range(2, h-2):
        cnt = 0
        for x in range(10, w-10, 2):
            a = px(x, y-1); b = px(x, y); c = px(x, y+1)
            if max(abs(a[i]-c[i]) for i in range(3)) > 3:
                continue
            if max(abs(b[i]-a[i]) for i in range(3)) >= 6:
                cnt += 1
        if cnt >= 15:
            hs.append((y, cnt))
    return px, w, h, vs, hs

for path in sys.argv[1:]:
    px, w, h, vs, hs = analyse(path)
    print("== %s (%dx%d)" % (os.path.basename(path), w, h))
    print("   vertical seams  :", [x for x, _ in vs][:16])
    print("   horizontal seams:", [y for y, _ in hs][:16])
    for x, c in vs[:3]:
        ys = [y for y in range(10, h-10)
              if max(abs(px(x,y)[i]-px(x-1,y)[i]) for i in range(3)) >= 6
              and max(abs(px(x-1,y)[i]-px(x+1,y)[i]) for i in range(3)) <= 3]
        if ys:
            y = ys[len(ys)//2]
            print("     x=%-4d (%d rows) e.g. y=%d: left=%s SEAM=%s right=%s" %
                  (x, c, y, px(x-1,y), px(x,y), px(x+1,y)))
    for y, c in hs[:3]:
        xs = [x for x in range(10, w-10)
              if max(abs(px(x,y)[i]-px(x,y-1)[i]) for i in range(3)) >= 6
              and max(abs(px(x,y-1)[i]-px(x,y+1)[i]) for i in range(3)) <= 3]
        if xs:
            x = xs[len(xs)//2]
            print("     y=%-4d (%d cols) e.g. x=%d: above=%s SEAM=%s below=%s" %
                  (y, c, x, px(x,y-1), px(x,y), px(x,y+1)))
    print()
