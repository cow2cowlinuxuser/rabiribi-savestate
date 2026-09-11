import struct, sys, zlib

def read_tga(p):
    d = open(p, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp, desc = d[16], d[17]
    off = 18 + idlen
    n = bpp // 8
    rows = [d[off + y*w*n: off + (y+1)*w*n] for y in range(h)]
    if not (desc & 0x20):
        rows.reverse()
    return w, h, n, rows

def write_png(p, w, h, rgbrows):
    raw = b''.join(b'\x00' + r for r in rgbrows)
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

src, dst = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (0,0,0,0)
zoom = int(sys.argv[7]) if len(sys.argv) > 7 else 1
w, h, n, rows = read_tga(src)
if x1 == 0: x1, y1 = w, h
x1 = min(x1, w); y1 = min(y1, h)
out = []
for y in range(y0, y1):
    r = rows[y]
    line = bytes(b for x in range(x0, x1)
                 for b in (r[x*n+2], r[x*n+1], r[x*n]) for _ in range(zoom))
    for _ in range(zoom):
        out.append(line)
write_png(dst, (x1-x0)*zoom, (y1-y0)*zoom, out)
print("%s -> %s  (%dx%d, zoom %d)" % (src, dst, (x1-x0)*zoom, (y1-y0)*zoom, zoom))
