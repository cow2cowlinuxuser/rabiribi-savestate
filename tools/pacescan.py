"""Relate the pacer's cost to everything else in the frame."""
import re, sys, statistics as st

log = sys.argv[1] if len(sys.argv) > 1 else 'd3d11_sw.log'
SPLIT = re.compile(r'^perf split ms/frame: draw ([\d.]+) \(raster [\d.]+ nested\) '
                   r'decode ([\d.]+) present ([\d.]+) pace ([\d.]+) \| flush elsewhere '
                   r'([\d.]+) \| game ([\d.]+) of ([\d.]+)')
PACE = re.compile(r'^perf pace: ([\d.]+) ms/frame total - ([\d.]+) in DwmFlush '
                  r'\(([\d.]+) call\(s\)/frame\), ([\d.]+) in sleep/spin \| display '
                  r'([\d.]+) Hz, target ([\d.]+) fps, (\d+) arrived late, (\d+) reset')

rows, cur = [], {}
for raw in open(log, errors='ignore'):
    s = raw.strip()
    m = SPLIT.match(s)
    if m:
        cur = dict(draw=float(m.group(1)), decode=float(m.group(2)),
                   present=float(m.group(3)), pace=float(m.group(4)),
                   elsewhere=float(m.group(5)), game=float(m.group(6)),
                   wall=float(m.group(7)))
        continue
    m = PACE.match(s)
    if m and cur:
        cur.update(ptot=float(m.group(1)), dwm=float(m.group(2)),
                   calls=float(m.group(3)), spin=float(m.group(4)),
                   hz=float(m.group(5)), target=float(m.group(6)),
                   late=int(m.group(7)), reset=int(m.group(8)))
        rows.append(cur)
        cur = {}

print(f"{len(rows)} sample(s) with both lines\n")

def col(k):
    return [r[k] for r in rows]

print("                    min   median      max")
for k in ['wall', 'draw', 'present', 'pace', 'dwm', 'spin', 'game']:
    v = col(k)
    print(f"  {k:10} {min(v):9.1f} {st.median(v):8.1f} {max(v):8.1f}")

dwm, calls = col('dwm'), col('calls')
per = [d / c for d, c in zip(dwm, calls) if c]
print(f"\nDwmFlush: {st.median(calls):.0f} call(s)/frame, "
      f"{st.median(per):.2f} ms each (median)")
print(f"one compositor frame at {rows[0]['hz']:.0f} Hz = {1000/rows[0]['hz']:.2f} ms")
print(f"so the flush loop costs {st.median(dwm):.1f} ms, and the frame budget at "
      f"{rows[0]['target']:.0f} fps is {1000/rows[0]['target']:.1f} ms")

# The decisive question: is pace a floor the work hides under, or a tax added
# on top of it? If it were a proper wait-until-deadline, pace would SHRINK as
# the rest of the frame grows. If it is a fixed blocking wait, it will not.
print("\nis the pacer absorbing the work, or adding to it?")
rows.sort(key=lambda r: r['game'] + r['draw'])
half = max(1, len(rows) // 3)
lo, hi = rows[:half], rows[-half:]
print(f"  {'':22} {'light frames':>14} {'heavy frames':>14}")
for k, lbl in [('draw', 'draw'), ('game', 'game'), ('pace', 'pace'),
               ('dwm', '  of which DwmFlush'), ('wall', 'total frame')]:
    a, b = st.median([r[k] for r in lo]), st.median([r[k] for r in hi])
    print(f"  {lbl:22} {a:14.1f} {b:14.1f}")

work = [r['draw'] + r['present'] + r['game'] for r in rows]
wall = col('wall')
print(f"\n  median work excluding pace: {st.median(work):.1f} ms")
print(f"  median pace:                {st.median(col('pace')):.1f} ms")
print(f"  median total:               {st.median(wall):.1f} ms")
add = st.median([w + p - t for w, p, t in zip(work, col('pace'), wall)])
print(f"  work + pace - total = {add:+.1f} ms  (0 means pace is pure addition,")
print(f"                                    negative means it absorbs work)")
print(f"\n  frames arriving past the deadline: median {st.median(col('late')):.0f} per sample")
