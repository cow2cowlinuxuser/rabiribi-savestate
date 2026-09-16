"""How much room is there inside an allocation that nobody is using?

The tagging idea needs somewhere to put a tag, and the cheapest somewhere is
space the allocator is already reserving and not handing out. This replays
gh_trace.txt, tracks which blocks are live, and measures the gap between the
end of one block's payload and the start of the next - which is the allocator's
own header plus whatever rounding it did.

Run:  py -3 tools/ghslack.py <gh_trace.txt> [more traces...]
"""
import sys
import collections


def load(path):
    ops = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            p = line.split()
            if len(p) != 3:
                continue
            op, size, off = p[0], int(p[1], 16), int(p[2], 16)
            ops.append((op, size, off))
    return ops


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        ops = load(path)
        live = {}          # offset -> requested size
        gaps = collections.Counter()
        sizes = collections.Counter()
        peak_live = 0
        served = 0

        for op, size, off in ops:
            if off == 0xFFFFFFFF:
                continue
            if op in 'ACR':
                live[off] = size
                sizes[size] += 1
                served += 1
                peak_live = max(peak_live, len(live))
            elif op == 'F':
                live.pop(off, None)

        # Gap between consecutive live blocks, at the end of the trace.
        ordered = sorted(live.items())
        for (o0, s0), (o1, _) in zip(ordered, ordered[1:]):
            gap = o1 - (o0 + s0)
            if 0 <= gap < 4096:
                gaps[gap] += 1

        payload = sum(live.values())
        span = (ordered[-1][0] + ordered[-1][1] - ordered[0][0]) if ordered else 0

        print("=" * 62)
        print(path)
        print("  %d operation(s), %d served, %d live at the end, peak %d" %
              (len(ops), served, len(live), peak_live))
        if not ordered:
            continue
        print("  live payload %.1f KB across a span of %.1f KB" %
              (payload / 1024.0, span / 1024.0))
        print("  so %.1f KB (%.0f%%) of the span is not payload" %
              ((span - payload) / 1024.0,
               100.0 * (span - payload) / span if span else 0))
        print("\n  gap between one block's end and the next block's start:")
        total = sum(gaps.values())
        for gap, n in sorted(gaps.items())[:12]:
            print("    %5d bytes   x%-6d %5.1f%%" % (gap, n, 100.0 * n / total))
        if total:
            avg = sum(g * n for g, n in gaps.items()) / float(total)
            print("    average %.1f bytes of gap per block" % avg)

        print("\n  most common requested sizes:")
        for sz, n in sizes.most_common(8):
            # What an 8-byte-granular allocator would round this up to.
            rounded = (sz + 7) & ~7
            print("    %8d bytes  x%-6d  rounds to %d, slack %d" %
                  (sz, n, rounded, rounded - sz))
    return 0


if __name__ == '__main__':
    sys.exit(main())
