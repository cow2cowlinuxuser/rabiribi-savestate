"""Correlate the wrapper's 2-second perf samples against frame time.

The log emits a block of perf lines every two seconds. Each block is one
observation; stitching them into a series shows which component moves when
frame time moves, which a single block cannot.
"""
import re, sys, statistics as st

log = sys.argv[1] if len(sys.argv) > 1 else 'd3d11_sw.log'
rows, cur = [], {}

RX = {
    'fps':     re.compile(r'^perf ([\d.]+) fps \(([\d.]+) ms/frame\)'),
    'split':   re.compile(r'^perf split ms/frame: draw ([\d.]+) \(raster ([\d.]+) nested\) '
                          r'decode ([\d.]+) present ([\d.]+) \| flush elsewhere ([\d.]+) \| '
                          r'game ([\d.]+) of ([\d.]+)'),
    'gpu':     re.compile(r'^perf gpu: (\d+) draw\(s\)/frame, (\d+) vert\(s\)/frame, '
                          r'(\d+) upload\(s\)/frame \| (\d+) declined'),
    'geom':    re.compile(r'batch ([\d.]+)% of draw'),
    'present': re.compile(r'^present: ([\d.]+) fps over [\d.]+ s \((\w+)'),
}

for raw in open(log, errors='ignore'):
    s = raw.strip()
    m = RX['fps'].match(s)
    if m:
        if cur.get('ms'):
            rows.append(cur)
        cur = {'fps': float(m.group(1)), 'ms': float(m.group(2))}
        continue
    m = RX['split'].match(s)
    if m:
        cur.update(draw=float(m.group(1)), raster=float(m.group(2)),
                   decode=float(m.group(3)), present=float(m.group(4)),
                   elsewhere=float(m.group(5)), game=float(m.group(6)),
                   wall=float(m.group(7)))
        continue
    m = RX['gpu'].match(s)
    if m:
        cur.update(draws=int(m.group(1)), verts=int(m.group(2)),
                   uploads=int(m.group(3)), declined=int(m.group(4)))
        continue
    m = RX['geom'].search(s)
    if m:
        cur['batch'] = float(m.group(1))
    m = RX['present'].match(s)
    if m:
        cur['pfps'] = float(m.group(1))
        cur['focus'] = m.group(2)
if cur.get('ms'):
    rows.append(cur)

rows = [r for r in rows if 'present' in r]
print(f"{len(rows)} complete sample(s)\n")

ms = [r['ms'] for r in rows]
print(f"frame time: min {min(ms):.1f}  median {st.median(ms):.1f}  "
      f"mean {st.mean(ms):.1f}  max {max(ms):.1f} ms   (stdev {st.pstdev(ms):.1f})")
print(f"as fps:     max {1000/min(ms):.1f}  median {1000/st.median(ms):.1f}  "
      f"min {1000/max(ms):.1f}\n")

# Where does the frame time actually go, and what moves when it gets worse?
keys = ['draw', 'decode', 'present', 'elsewhere', 'game', 'raster']
print("component ms/frame, over all samples:")
print(f"  {'':10} {'min':>7} {'median':>8} {'max':>8}  {'share of median frame':>22}")
med_ms = st.median(ms)
for k in keys:
    v = [r[k] for r in rows]
    print(f"  {k:10} {min(v):7.1f} {st.median(v):8.1f} {max(v):8.1f}  "
          f"{100*st.median(v)/med_ms:21.1f}%")

# Split the samples into the good half and the bad half and compare.
rows.sort(key=lambda r: r['ms'])
half = len(rows) // 2
good, bad = rows[:half], rows[-half:]
print(f"\nfastest {len(good)} sample(s) vs slowest {len(bad)}:")
print(f"  {'':10} {'fast':>9} {'slow':>9} {'delta':>9}")
for k in ['ms'] + keys + ['draws', 'verts', 'uploads', 'declined', 'batch']:
    if k not in rows[0]:
        continue
    a, b = st.median([r[k] for r in good]), st.median([r[k] for r in bad])
    print(f"  {k:10} {a:9.1f} {b:9.1f} {b-a:+9.1f}")

# How much of the frame is unaccounted for? That is the number that says
# whether the cost is inside our code at all.
print("\nunaccounted time (wall minus every measured component):")
un = [r['wall'] - (r['draw'] + r['decode'] + r['present'] + r['elsewhere']) for r in rows]
print(f"  min {min(un):.1f}  median {st.median(un):.1f}  max {max(un):.1f} ms")
