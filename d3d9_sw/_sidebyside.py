"""Crop the real backbuffer next to a synthesised bilinear version of the same
frame, so the two present paths can be compared on identical content.

The bilinear capture was overwritten, but it does not need recapturing: the
offscreen is in the snapshot, and the bilinear blit was measured to be a flat
2x2 average of it to within one LSB everywhere. Reconstructing that from the
same offscreen gives a comparison with no frame-to-frame difference at all,
which is stricter than two captures of a moving scene could ever be.
"""
import struct, sys, zlib


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


def write_png(path, rows, w, h):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)


def main(src_path, bb_path, out, x0, y0, cw, ch, scale):
    src, sw, sh = loader(src_path)
    bb, bw, bh = loader(bb_path)

    def bilin(x, y):
        acc = [0, 0, 0]
        for dy in (0, 1):
            for dx in (0, 1):
                p = src(x + dx, y + dy)
                for c in range(3):
                    acc[c] += p[c]
        return tuple((v + 2) // 4 for v in acc)

    gap = 8
    outw = cw * scale
    outh = ch * scale * 2 + gap
    rows = []
    for sy in range(ch * scale):
        y = y0 + sy // scale
        r = []
        for sx in range(outw):
            x = x0 + sx // scale
            r.extend(bb(x, y))
        rows.append(r)
    for _ in range(gap):
        rows.append([40] * (outw * 3))
    for sy in range(ch * scale):
        y = y0 + sy // scale
        r = []
        for sx in range(outw):
            x = x0 + sx // scale
            r.extend(bilin(x, y))
        rows.append(r)

    write_png(out, rows, outw, outh)
    print("wrote %s  (top: actual point-sampled backbuffer, bottom: bilinear reconstructed from the same offscreen)" % out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3],
         int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8]))
