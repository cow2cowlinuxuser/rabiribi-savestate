"""Compare two tagged allocation traces by block NAME rather than by address.

trdiff.py answers "did the same offsets come back", which is a question about
the board. This answers "did the same block come back, and where did it land
this time", which is a question about the ball. They disagree exactly where it
matters: two blocks can swap offsets between sessions and leave the offset
column identical, and a block can be perfectly stable while everything around
it moves.

A name here is (call site, ordinal) - the code that asked, and how many times
that code had already asked for that size. gameheap.c writes both into
gh_trace.txt as two extra columns. Traces captured before tagging existed have
only three columns and will report as entirely unnamed rather than failing.

    py -3 tools/tagdiff.py a/gh_trace.txt b/gh_trace.txt
    py -3 tools/tagdiff.py a/gh_trace.txt b/gh_trace.txt --chutes
"""

import argparse
import collections

NONE = 0xFFFFFFFF


class Trace:
    def __init__(self, path):
        self.path = path
        self.heap = None
        self.image = None
        self.ops = 0
        self.unnamed = 0
        # offset -> (site, ordinal, size), the blocks still live at the end
        self.live = {}
        # How many times each site allocated, over the whole run rather than
        # just what survived. An ordinal is a count, so a site that was called a
        # different number of times in two runs has its later blocks numbered
        # differently and they pair up wrongly - which looks exactly like the
        # heap being unstable. Comparing these counts is what tells the two
        # apart.
        self.site_calls = collections.Counter()
        self._read(path)

    def _read(self, path):
        with open(path, "r", errors="replace") as f:
            for line in f:
                if line.startswith("#"):
                    bits = line.split()
                    if len(bits) >= 3 and bits[1] == "heap":
                        self.heap = int(bits[2], 16)
                    elif len(bits) >= 3 and bits[1] == "image":
                        self.image = int(bits[2], 16)
                    continue
                bits = line.split()
                if len(bits) < 3:
                    continue
                op = bits[0]
                try:
                    size = int(bits[1], 16)
                    off = int(bits[2], 16)
                except ValueError:
                    continue
                site = int(bits[3], 16) if len(bits) > 3 else NONE
                ordinal = int(bits[4], 16) if len(bits) > 4 else NONE
                self.ops += 1
                if op in ("A", "C", "R"):
                    if off == NONE:
                        continue
                    if site == NONE:
                        self.unnamed += 1
                    else:
                        self.site_calls[site] += 1
                    # A realloc that stayed put rewrites its own entry, which is
                    # correct: it is the same block at the same place.
                    self.live[off] = (site, ordinal, size)
                elif op == "F":
                    self.live.pop(off, None)

    def by_name(self):
        """Blocks that carry a usable name, keyed by it.

        A duplicate name would mean the ordinal is not doing its job, so it is
        counted rather than silently overwritten."""
        out = {}
        dupes = 0
        for off, (site, ordinal, size) in self.live.items():
            if site == NONE or ordinal == NONE:
                continue
            key = (site, ordinal, size)
            if key in out:
                dupes += 1
                continue
            out[key] = off
        return out, dupes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--chutes", action="store_true",
                    help="list the call sites that allocate the most blocks")
    ap.add_argument("--show", type=int, default=12,
                    help="how many disagreeing blocks to print")
    args = ap.parse_args()

    a, b = Trace(args.a), Trace(args.b)
    for t in (a, b):
        print("%s: %d op(s), heap %s, image %s, %d live at the end, %d unnamed" % (
            t.path, t.ops,
            "%08X" % t.heap if t.heap is not None else "?",
            "%08X" % t.image if t.image is not None else "?",
            len(t.live), t.unnamed))

    na, dup_a = a.by_name()
    nb, dup_b = b.by_name()
    if not na or not nb:
        print("\nOne of these traces carries no names. Capture both with a build "
              "that writes the site and ordinal columns.")
        return
    if dup_a or dup_b:
        print("\n%d and %d block(s) shared a name - the ordinal is colliding, so "
              "the numbers below are optimistic" % (dup_a, dup_b))

    shared = set(na) & set(nb)
    same = [k for k in shared if na[k] == nb[k]]
    moved = sorted(shared - set(same), key=lambda k: -k[2])

    print("\nnamed blocks       %d in A, %d in B" % (len(na), len(nb)))
    print("present in both    %d (%.1f%% of A)" % (
        len(shared), 100.0 * len(shared) / len(na)))
    print("same offset too    %d (%.1f%% of the shared)" % (
        len(same), 100.0 * len(same) / len(shared) if shared else 0.0))

    # The comparison the whole exercise is for: a block that keeps its offset
    # under its own name is one a restore can place without knowing an address.
    big = [k for k in shared if k[2] >= 4096]
    big_same = [k for k in big if na[k] == nb[k]]
    print("of those >= 4 KB   %d of %d agree" % (len(big_same), len(big)))

    only_a = set(na) - set(nb)
    if only_a:
        print("in A only          %d - the sessions did not ask for the same "
              "things" % len(only_a))

    # ---- bins against balls ----
    #
    # The distinction the whole exercise turns on. A bin is a place a block can
    # land, identified by offset and size; a ball is a particular block. If the
    # bins agree while the names in them do not, the heap is handing out the
    # same set of addresses in a different order - the pile looks identical from
    # outside and every individual block has moved. Comparing offset columns, as
    # trdiff does, cannot see the difference.
    bins_a = {(off, size): (site, ordinal)
              for off, (site, ordinal, size) in a.live.items()}
    bins_b = {(off, size): (site, ordinal)
              for off, (site, ordinal, size) in b.live.items()}
    shared_bins = set(bins_a) & set(bins_b)
    same_ball = [k for k in shared_bins if bins_a[k] == bins_b[k]]
    print("\nbins in both       %d of %d (%.1f%%) - same offset and size" % (
        len(shared_bins), len(bins_a),
        100.0 * len(shared_bins) / len(bins_a) if bins_a else 0.0))
    print("holding the same   %d (%.1f%% of the shared bins)" % (
        len(same_ball),
        100.0 * len(same_ball) / len(shared_bins) if shared_bins else 0.0))

    # ---- the control for ordinal drift ----
    #
    # Restricted to sites called the same number of times in both runs. For
    # those the ordinal cannot have drifted, so a disagreement here is the heap
    # genuinely placing a block elsewhere rather than the comparison pairing up
    # the wrong two blocks.
    steady = {s for s in a.site_calls
              if a.site_calls[s] == b.site_calls.get(s)}
    drifted = len(set(a.site_calls) | set(b.site_calls)) - len(steady)
    ctl = [k for k in shared if k[0] in steady]
    ctl_same = [k for k in ctl if na[k] == nb[k]]
    print("\nsites called the same number of times: %d (%d drifted)" % (
        len(steady), drifted))
    if ctl:
        print("named blocks from those sites: %d, %d at the same offset (%.1f%%)"
              % (len(ctl), len(ctl_same), 100.0 * len(ctl_same) / len(ctl)))
        print("  -> this is the number that means something; the headline above\n"
              "     mixes in blocks whose ordinals could not line up")
    else:
        print("no site was called the same number of times in both runs, so no\n"
              "block can be paired with confidence")

    if moved:
        print("\nnamed blocks that landed somewhere else, largest first:")
        for k in moved[:args.show]:
            site, ordinal, size = k
            print("  site +%06X #%-4d %7d bytes   %08X -> %08X  (%+d)" % (
                site, ordinal, size, na[k], nb[k], nb[k] - na[k]))
        if len(moved) > args.show:
            print("  ... and %d more" % (len(moved) - args.show))

    if args.chutes:
        # The pachinko view: how many balls each chute released, and how many of
        # them landed in the same bin both times.
        per = collections.defaultdict(lambda: [0, 0])
        for k in shared:
            per[k[0]][0] += 1
            if na[k] == nb[k]:
                per[k[0]][1] += 1
        print("\nchutes by traffic:")
        for site, (n, ok) in sorted(per.items(), key=lambda kv: -kv[1][0])[:20]:
            print("  site +%06X  %4d block(s), %4d stable (%.0f%%)" % (
                site, n, ok, 100.0 * ok / n))


if __name__ == "__main__":
    main()
