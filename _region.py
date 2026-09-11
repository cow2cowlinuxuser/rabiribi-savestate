"""Report which draws own each region of a target.

  python _region.py <target.did> <x0,y0,x1,y1> [<x0,y0,x1,y1> ...]
"""
import struct, sys, os
from collections import Counter

def read_did(p):
    d = open(p, 'rb').read()
    if d[:4] != b'DID1':
        raise SystemExit("not a draw-id file: %s" % p)
    w, h = struct.unpack_from('<ii', d, 4)
    return w, h, memoryview(d)[12:].cast('I')

path = sys.argv[1]
w, h, ids = read_did(path)
print("%s  %dx%d" % (os.path.basename(path), w, h))
total = Counter()
for y in range(h):
    row = y * w
    for x in range(w):
        total[ids[row + x]] += 1
print("draws that wrote anything, by pixel count:")
for k, c in total.most_common(20):
    print("   draw=%-5d %8d px" % (k, c))
print()

for spec in sys.argv[2:]:
    x0, y0, x1, y1 = (int(v) for v in spec.split(','))
    c = Counter()
    for y in range(max(0, y0), min(h, y1)):
        row = y * w
        for x in range(max(0, x0), min(w, x1)):
            c[ids[row + x]] += 1
    print("region x=%d..%d y=%d..%d" % (x0, x1, y0, y1))
    for k, n in c.most_common(10):
        print("   draw=%-5d %7d px" % (k, n))
    print()
