"""Name the tracks behind an allocation trace by matching buffer sizes to durations.

The game's Ogg comment headers carry no TITLE, so a track load in gh_trace.txt is
anonymous. What it does have is size: a decoded sound is one buffer, recorded as a
B operation because it is far too large for the private heap. Divided by
rate * channels * 2 that size is a duration, and the store soundtrack supplies the
same durations with names attached.

Reads gh_trace.txt and the ost.csv written by ostlen.py.
"""

import argparse
import csv
import os
import sys

# The formats a decoded buffer could plausibly be in. Rabi-Ribi's music is
# 44.1 kHz stereo with a handful of 48 kHz pieces; both are tried, and mono is
# tried too because a buffer that is exactly half a track's length is still that
# track, just not interleaved yet.
LAYOUTS = [
    ("44100 stereo", 44100 * 2 * 2),
    ("48000 stereo", 48000 * 2 * 2),
    ("44100 mono", 44100 * 2),
    ("48000 mono", 48000 * 2),
]


def load_ops(path):
    """Return (index, kind, size) for every record in a trace."""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        i = 0
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 2:
                i += 1
                continue
            try:
                out.append((i, parts[0], int(parts[1], 16)))
            except ValueError:
                pass
            i += 1
    return out


def load_ost(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out.append((row["track"], float(row["seconds"])))
    return out


def best(secs, ost, tol):
    hits = [(abs(s - secs), name, s) for name, s in ost if abs(s - secs) <= tol]
    hits.sort()
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default="gh_trace.txt")
    ap.add_argument("--ost", default="ost.csv")
    ap.add_argument("--tol", type=float, default=0.05, help="seconds of slack")
    ap.add_argument("--min", type=int, default=262144, help="ignore buffers under this")
    args = ap.parse_args()

    for p in (args.trace, args.ost):
        if not os.path.exists(p):
            print("missing: %s" % p, file=sys.stderr)
            return 1

    ops = load_ops(args.trace)
    ost = load_ost(args.ost)
    bigs = [(i, n) for i, kind, n in ops if kind == "B" and n >= args.min]
    print("%d record(s), %d oversize buffer(s), %d track(s) in the table\n"
          % (len(ops), len(bigs), len(ost)))

    named = {}
    unnamed = {}
    for op, n in bigs:
        got = None
        for label, rate in LAYOUTS:
            hits = best(n / float(rate), ost, args.tol)
            if hits:
                got = (hits[0][1], hits[0][2], label, hits[0][0])
                break
        if got:
            named.setdefault((n, got[0], got[2]), []).append(op)
        else:
            unnamed.setdefault(n, []).append(op)

    print("=== buffers that land on a track ===")
    rows = sorted(named.items(), key=lambda kv: -len(kv[1]))
    for (n, name, layout), opl in rows:
        print("  %11d bytes  x%-4d %-14s %s" % (n, len(opl), layout, name))
        print("               first seen at operation %s"
              % ", ".join(str(o) for o in opl[:6]))
    if not rows:
        print("  none")

    print("\n=== buffers with no match within %.2f s ===" % args.tol)
    for n, opl in sorted(unnamed.items(), key=lambda kv: -len(kv[1]))[:20]:
        print("  %11d bytes  x%-4d  %7.3f s at 44100 stereo"
              % (n, len(opl), n / 176400.0))
    if not unnamed:
        print("  none")

    hit = sum(len(v) for v in named.values())
    print("\n%d of %d buffer(s) named" % (hit, len(bigs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
