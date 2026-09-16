"""Stability of gh_trace*.txt as a function of offset, order, size, and site.

Throwaway analysis for the visualizer/hypothesis scoping pass. Reconstructs
the live set more carefully than tagdiff.py: a realloc that moved is recorded
as R at the new offset only, so the old offset must be retired by name.
"""
from __future__ import print_function

import collections
import os
import sys

NONE = 0xFFFFFFFF
GH_BIG = 256 * 1024
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi"

TRACES = [
    "gh_trace.txt.prev",
    "gh_trace1.txt",
    "gh_trace2.txt",
    "gh_trace3.txt",
    "gh_trace4.txt",
    "gh_trace5.txt",
    "gh_trace.txt",
]


class Op(object):
    __slots__ = ("op", "size", "off", "site", "ord")

    def __init__(self, op, size, off, site, ordinal):
        self.op = op
        self.size = size
        self.off = off
        self.site = site
        self.ord = ordinal


class Trace(object):
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.heap = None
        self.image = None
        self.ops = []
        self.named = 0
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
                self.ops.append(Op(op, size, off, site, ordinal))
                if site != NONE:
                    self.named += 1


def replay(ops):
    """Return live map, births, and counters.

    live: off -> (site, ord, size, birth_i)
    by_name: (site, ord, size) -> off   for currently live named blocks
    births: (site, ord, size) -> first op index that created this name
    """
    live = {}
    by_name = {}
    births = {}
    n_alloc = n_free = n_realloc = n_big = n_other = 0
    bytes_alloc = bytes_big = 0
    peak_live = 0
    peak_payload = 0
    served = 0
    unnamed_live = 0
    realloc_moved = 0
    max_off = 0
    ghost_frees = 0

    for i, o in enumerate(ops):
        if o.op == "B":
            n_big += 1
            bytes_big += o.size
            continue
        if o.op == "O":
            n_other += 1
            continue
        in_heap = o.off != NONE
        if o.op in ("A", "C"):
            n_alloc += 1
            bytes_alloc += o.size
            served += 1
            if not in_heap:
                continue
            key = (o.site, o.ord, o.size) if o.site != NONE else None
            if key is not None:
                if key not in births:
                    births[key] = i
                by_name[key] = o.off
            live[o.off] = (o.site, o.ord, o.size, births.get(key, i) if key else i)
            if o.off + o.size > max_off:
                max_off = o.off + o.size
        elif o.op == "R":
            n_realloc += 1
            served += 1
            if not in_heap:
                continue
            key = (o.site, o.ord, o.size) if o.site != NONE else None
            # Retire previous location of this name, if any. The recorded size
            # is the NEW size, so also try matching (site, ord, *) at one off.
            old = None
            if key is not None and key in by_name:
                old = by_name[key]
            elif o.site != NONE:
                for k, off in list(by_name.items()):
                    if k[0] == o.site and k[1] == o.ord:
                        old = off
                        del by_name[k]
                        break
            if old is not None and old != o.off:
                realloc_moved += 1
                live.pop(old, None)
            if key is not None:
                if key not in births:
                    births[key] = i
                by_name[key] = o.off
            live[o.off] = (o.site, o.ord, o.size, births.get(key, i) if key else i)
            if o.off + o.size > max_off:
                max_off = o.off + o.size
        elif o.op == "F":
            n_free += 1
            if not in_heap:
                continue
            rec = live.pop(o.off, None)
            if rec is None:
                ghost_frees += 1
            else:
                site, ordinal, size, _birth = rec
                if site != NONE:
                    by_name.pop((site, ordinal, size), None)

        nlive = len(live)
        if nlive > peak_live:
            peak_live = nlive
        payload = sum(v[2] for v in live.values())
        if payload > peak_payload:
            peak_payload = payload

    unnamed_live = sum(1 for v in live.values() if v[0] == NONE)
    return {
        "live": live,
        "by_name": by_name,
        "births": births,
        "n_alloc": n_alloc,
        "n_free": n_free,
        "n_realloc": n_realloc,
        "n_big": n_big,
        "n_other": n_other,
        "bytes_alloc": bytes_alloc,
        "bytes_big": bytes_big,
        "peak_live": peak_live,
        "peak_payload": peak_payload,
        "served": served,
        "unnamed_live": unnamed_live,
        "realloc_moved": realloc_moved,
        "max_off": max_off,
        "ghost_frees": ghost_frees,
        "payload": sum(v[2] for v in live.values()),
    }


def op_size_stream(ops):
    return [(o.op, o.size) for o in ops]


def first_diverge(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if (a[i].op, a[i].size) != (b[i].op, b[i].size):
            return i, "op+size"
        if a[i].off != b[i].off:
            return i, "offset"
        if a[i].site != b[i].site or a[i].ord != b[i].ord:
            return i, "name"
    if len(a) != len(b):
        return n, "length"
    return None, "identical"


def prefix_of(short, long_):
    if len(short) > len(long_):
        return False
    for i, o in enumerate(short):
        p = long_[i]
        if (o.op, o.size, o.off, o.site, o.ord) != (p.op, p.size, p.off, p.site, p.ord):
            return False
    return True


def bin_label(edges, x):
    for i in range(len(edges) - 1):
        if x < edges[i + 1] or i == len(edges) - 2:
            return i
    return len(edges) - 2


def pct(n, d):
    if not d:
        return "n/a"
    return "%.1f%%" % (100.0 * n / d)


def size_class(n):
    if n < 64:
        return "0-63"
    if n < 256:
        return "64-255"
    if n < 1024:
        return "256-1K"
    if n < 4096:
        return "1K-4K"
    if n < 65536:
        return "4K-64K"
    if n < GH_BIG:
        return "64K-256K"
    return ">=256K"


def compare_named(ra, rb, label, n_ops_a=None):
    na, nb = ra["by_name"], rb["by_name"]
    shared = set(na) & set(nb)
    same = [k for k in shared if na[k] == nb[k]]
    moved = [k for k in shared if na[k] != nb[k]]
    only_a = set(na) - set(nb)
    only_b = set(nb) - set(na)

    print("\n-- %s --" % label)
    print("  named live  A=%d B=%d  shared=%d (%s of A)  same-off=%d (%s of shared)  moved=%d"
          % (len(na), len(nb), len(shared), pct(len(shared), len(na)),
             len(same), pct(len(same), len(shared)), len(moved)))
    print("  A-only %d  B-only %d" % (len(only_a), len(only_b)))

    bins_a = {(off, rec[2]): (rec[0], rec[1]) for off, rec in ra["live"].items()
              if rec[0] != NONE}
    bins_b = {(off, rec[2]): (rec[0], rec[1]) for off, rec in rb["live"].items()
              if rec[0] != NONE}
    shared_bins = set(bins_a) & set(bins_b)
    same_ball = [k for k in shared_bins if bins_a[k] == bins_b[k]]
    print("  bins (off,size) in both %d of %d A (%s); same occupant %d (%s of shared bins)"
          % (len(shared_bins), len(bins_a), pct(len(shared_bins), len(bins_a)),
             len(same_ball), pct(len(same_ball), len(shared_bins))))

    if not shared:
        return

    # Position: equal-width tenths of occupied span in A.
    offs = [na[k] for k in shared]
    lo, hi = min(offs), max(offs)
    span = max(hi - lo, 1)
    width_counts = [[0, 0] for _ in range(10)]  # [shared, same]
    for k in shared:
        t = min(9, int((na[k] - lo) * 10 / span))
        width_counts[t][0] += 1
        if na[k] == nb[k]:
            width_counts[t][1] += 1
    print("  equal-width tenths of A's occupied span [%08X .. %08X]:" % (lo, hi))
    print("    bin   off-range              n  same   pct")
    for i, (n, ok) in enumerate(width_counts):
        a0 = lo + span * i // 10
        a1 = lo + span * (i + 1) // 10
        print("    %2d  %08X-%08X  %4d %4d  %s" % (i, a0, a1, n, ok, pct(ok, n)))

    # Equal-count deciles by offset (avoids empty high bins if heap is sparse).
    ranked = sorted(shared, key=lambda k: na[k])
    print("  equal-count offset deciles (sorted by A's offset):")
    print("    dec  n  same  pct   off-lo   off-hi")
    for d in range(10):
        sl = ranked[d * len(ranked) // 10: (d + 1) * len(ranked) // 10]
        if not sl:
            continue
        ok = sum(1 for k in sl if na[k] == nb[k])
        print("    %2d %4d %4d %6s  %08X %08X" % (
            d, len(sl), ok, pct(ok, len(sl)), na[sl[0]], na[sl[-1]]))

    # Edges vs center: lowest 10% / middle 80% / highest 10% by offset.
    n = len(ranked)
    edge_n = max(1, n // 10)
    regions = [
        ("low 10%", ranked[:edge_n]),
        ("mid 80%", ranked[edge_n:-edge_n] if n > 2 * edge_n else ranked),
        ("high 10%", ranked[-edge_n:]),
    ]
    print("  edges vs center by OFFSET (equal-count):")
    for name, sl in regions:
        ok = sum(1 for k in sl if na[k] == nb[k])
        print("    %-8s n=%4d same=%4d (%s)  span %08X-%08X" % (
            name, len(sl), ok, pct(ok, len(sl)), na[sl[0]], na[sl[-1]]))

    # Birth order: earliest 10% of births among shared vs latest 10%.
    births_a = ra["births"]
    born = sorted(shared, key=lambda k: births_a.get(k, 10 ** 9))
    print("  edges vs center by BIRTH ORDER (first op that created the name in A):")
    for name, sl in [
        ("early 10%", born[:edge_n]),
        ("mid 80%", born[edge_n:-edge_n] if n > 2 * edge_n else born),
        ("late 10%", born[-edge_n:]),
    ]:
        ok = sum(1 for k in sl if na[k] == nb[k])
        b0 = births_a.get(sl[0], -1)
        b1 = births_a.get(sl[-1], -1)
        print("    %-9s n=%4d same=%4d (%s)  birth ops %d..%d" % (
            name, len(sl), ok, pct(ok, len(sl)), b0, b1))

    # Correlation: offset rank vs birth rank among shared.
    # Spearman via average of squared rank difference.
    off_rank = {k: i for i, k in enumerate(ranked)}
    birth_rank = {k: i for i, k in enumerate(born)}
    nn = float(n)
    d2 = sum((off_rank[k] - birth_rank[k]) ** 2 for k in shared)
    spearman = 1.0 - (6.0 * d2) / (nn * (nn * nn - 1.0)) if nn > 1 else 1.0
    print("  Spearman rank correlation (offset vs birth) among shared: %.3f" % spearman)

    # Recycled vs never-freed: a name whose birth offset equals final offset
    # and who never moved is "still at first placement". Approximate: birth
    # op index vs whether the block sits in the low part of the heap.
    # Split shared into: final off is in the first-half of span vs second.
    # And into: birth in first half of ops vs second.
    if n_ops_a:
        early_birth = [k for k in shared if births_a.get(k, 0) < n_ops_a / 2]
        late_birth = [k for k in shared if births_a.get(k, 0) >= n_ops_a / 2]
        mid = lo + span / 2
        print("  2x2 birth-half x offset-half (same-offset rate):")
        for bname, bset in (("early-birth", early_birth), ("late-birth", late_birth)):
            for oname, pred in (("low-off", lambda k: na[k] < mid),
                                ("high-off", lambda k: na[k] >= mid)):
                sl = [k for k in bset if pred(k)]
                ok = sum(1 for k in sl if na[k] == nb[k])
                print("    %-12s %-8s n=%4d same=%s" % (bname, oname, len(sl), pct(ok, len(sl))))

    # Size class
    print("  by size class of the named block:")
    byc = collections.defaultdict(lambda: [0, 0])
    for k in shared:
        byc[size_class(k[2])][0] += 1
        if na[k] == nb[k]:
            byc[size_class(k[2])][1] += 1
    for c in ("0-63", "64-255", "256-1K", "1K-4K", "4K-64K", "64K-256K", ">=256K"):
        n, ok = byc[c]
        if n:
            print("    %-10s %4d  same %4d (%s)" % (c, n, ok, pct(ok, n)))

    # Call sites
    per = collections.defaultdict(lambda: [0, 0])
    for k in shared:
        per[k[0]][0] += 1
        if na[k] == nb[k]:
            per[k[0]][1] += 1
    print("  top chutes by shared traffic:")
    for site, (n, ok) in sorted(per.items(), key=lambda kv: -kv[1][0])[:12]:
        print("    site +%06X  %4d block(s), %4d stable (%s)" % (
            site, n, ok, pct(ok, n)))

    # Largest movers
    movers = sorted(moved, key=lambda k: -k[2])
    if movers:
        print("  named blocks that moved, largest first (up to 12):")
        for k in movers[:12]:
            site, ordinal, size = k
            print("    site +%06X #%-4d %7d B   %08X -> %08X  (%+d)" % (
                site, ordinal, size, na[k], nb[k], nb[k] - na[k]))
        # Where did movers live in A?
        m_off = [na[k] for k in movers]
        m_birth = [births_a.get(k, -1) for k in movers]
        print("  movers: n=%d  A-offset median %08X  birth-op median %d" % (
            len(movers), sorted(m_off)[len(m_off) // 2],
            sorted(m_birth)[len(m_birth) // 2]))
        low_m = sum(1 for k in movers if na[k] < lo + span * 0.1)
        high_m = sum(1 for k in movers if na[k] >= lo + span * 0.9)
        print("  movers in lowest 10%% of span: %d; highest 10%% of span: %d" % (low_m, high_m))


def summarise(t, st):
    print("\n==== %s ====" % t.name)
    print("  heap %s  image %s  ops %d  named-ops %d" % (
        ("%08X" % t.heap) if t.heap is not None else "?",
        ("%08X" % t.image) if t.image is not None else "?",
        len(t.ops), t.named))
    print("  A/C %d  R %d  F %d  B(GH_BIG) %d  O %d  realloc-moved %d  ghost-F %d" % (
        st["n_alloc"], st["n_realloc"], st["n_free"], st["n_big"], st["n_other"],
        st["realloc_moved"], st["ghost_frees"]))
    live_n = len(st["live"])
    print("  live at end %d (unnamed %d)  payload %.1f KB  peak live %d  peak payload %.1f KB  span-to %.1f KB" % (
        live_n, st["unnamed_live"], st["payload"] / 1024.0, st["peak_live"],
        st["peak_payload"] / 1024.0, st["max_off"] / 1024.0))
    tot_served_bytes = st["bytes_alloc"]  # A/C only; R is a resize
    tot_big_bytes = st["bytes_big"]
    tot_events = st["n_alloc"] + st["n_realloc"] + st["n_big"]
    print("  GH_BIG events %d / %d alloc-like (A/C/R/B) = %s of events" % (
        st["n_big"], tot_events, pct(st["n_big"], tot_events)))
    print("  GH_BIG bytes requested %.1f KB vs in-heap A/C bytes %.1f KB = %s of those bytes" % (
        tot_big_bytes / 1024.0, tot_served_bytes / 1024.0,
        pct(tot_big_bytes, tot_big_bytes + tot_served_bytes)))
    # Live size histogram
    hist = collections.Counter(size_class(v[2]) for v in st["live"].values())
    byte_hist = collections.defaultdict(int)
    for v in st["live"].values():
        byte_hist[size_class(v[2])] += v[2]
    print("  live size histogram (count / KB):")
    for c in ("0-63", "64-255", "256-1K", "1K-4K", "4K-64K", "64K-256K", ">=256K"):
        if hist[c]:
            print("    %-10s %4d  %7.1f KB" % (c, hist[c], byte_hist[c] / 1024.0))
    if st["live"]:
        offs = sorted(st["live"])
        print("  live offset min %08X max %08X  first-two %s" % (
            offs[0], offs[-1],
            ", ".join("%08X:%d" % (o, st["live"][o][2]) for o in offs[:3])))


def checkpoint_delta(ra, rb, la, lb):
    """Same-session later dump vs earlier: what appeared, what vanished."""
    na, nb = ra["by_name"], rb["by_name"]
    gone = set(na) - set(nb)
    new = set(nb) - set(na)
    stayed = set(na) & set(nb)
    moved = [k for k in stayed if na[k] != nb[k]]
    print("\n-- checkpoint %s -> %s (same session if prefix) --" % (la, lb))
    print("  stayed %d  gone %d  new %d  stayed-but-moved %d" % (
        len(stayed), len(gone), len(new), len(moved)))
    gone_bytes = sum(k[2] for k in gone)
    new_bytes = sum(k[2] for k in new)
    print("  gone bytes %.1f KB  new bytes %.1f KB" % (gone_bytes / 1024.0, new_bytes / 1024.0))
    print("  gone by size class:")
    ghist = collections.Counter(size_class(k[2]) for k in gone)
    nhist = collections.Counter(size_class(k[2]) for k in new)
    for c in ("0-63", "64-255", "256-1K", "1K-4K", "4K-64K", "64K-256K", ">=256K"):
        if ghist[c] or nhist[c]:
            print("    %-10s gone %4d  new %4d" % (c, ghist[c], nhist[c]))
    # Positions of gone vs new
    if gone and na:
        offs = [na[k] for k in gone]
        all_off = sorted(na.values())
        lo, hi = all_off[0], all_off[-1]
        span = max(hi - lo, 1)
        low = sum(1 for o in offs if o < lo + span * 0.1)
        high = sum(1 for o in offs if o >= lo + span * 0.9)
        print("  gone blocks in lowest 10%% span: %d/%d; highest 10%%: %d/%d" % (
            low, len(gone), high, len(gone)))
    if new and nb:
        offs = [nb[k] for k in new]
        all_off = sorted(nb.values())
        lo, hi = all_off[0], all_off[-1]
        span = max(hi - lo, 1)
        low = sum(1 for o in offs if o < lo + span * 0.1)
        high = sum(1 for o in offs if o >= lo + span * 0.9)
        print("  new blocks in lowest 10%% span: %d/%d; highest 10%%: %d/%d" % (
            low, len(new), high, len(new)))


def main():
    traces = []
    for fn in TRACES:
        p = os.path.join(GAME, fn)
        if not os.path.exists(p):
            print("MISSING", p)
            continue
        traces.append(Trace(p))

    states = []
    for t in traces:
        st = replay(t.ops)
        states.append(st)
        summarise(t, st)

    print("\n\n======== PREFIX / COMPARABILITY ========")
    byname = {t.name: (t, states[i]) for i, t in enumerate(traces)}
    pairs = [
        ("gh_trace3.txt", "gh_trace4.txt"),
        ("gh_trace4.txt", "gh_trace5.txt"),
        ("gh_trace3.txt", "gh_trace5.txt"),
        ("gh_trace2.txt", "gh_trace.txt"),
        ("gh_trace2.txt", "gh_trace5.txt"),
        ("gh_trace.txt", "gh_trace5.txt"),
        ("gh_trace.txt.prev", "gh_trace2.txt"),
        ("gh_trace.txt.prev", "gh_trace5.txt"),
    ]
    for a, b in pairs:
        if a not in byname or b not in byname:
            continue
        ta, sa = byname[a]
        tb, sb = byname[b]
        pref = prefix_of(ta.ops, tb.ops)
        di, why = first_diverge(ta.ops, tb.ops)
        sa_ops = op_size_stream(ta.ops)
        sb_ops = op_size_stream(tb.ops)
        n = min(len(ta.ops), len(tb.ops))
        opsize_eq = sa_ops[:n] == sb_ops[:n]
        print("\n%s (%d) vs %s (%d)" % (a, len(ta.ops), b, len(tb.ops)))
        print("  exact prefix: %s" % pref)
        print("  op+size identical over min length: %s" % opsize_eq)
        if di is None:
            print("  streams identical")
        else:
            print("  first diverge at op %d (%s)" % (di, why))
            if di < n:
                oa, ob = ta.ops[di], tb.ops[di]
                print("    A: %s size=%X off=%08X site=%08X #%d" % (
                    oa.op, oa.size, oa.off, oa.site, oa.ord))
                print("    B: %s size=%X off=%08X site=%08X #%d" % (
                    ob.op, ob.size, ob.off, ob.site, ob.ord))

    print("\n\n======== CROSS-SESSION (aligned to min length) ========")
    # Latest session (9701) vs earlier long session truncated to 9701.
    if "gh_trace2.txt" in byname and "gh_trace5.txt" in byname:
        t2, s2 = byname["gh_trace2.txt"]
        t5, _s5 = byname["gh_trace5.txt"]
        n = min(len(t2.ops), len(t5.ops))
        print("\nAligning gh_trace2 (%d) against first %d ops of gh_trace5" % (len(t2.ops), n))
        head5 = t5.ops[:n]
        di, why = first_diverge(t2.ops, head5)
        print("  first diverge at %s (%s)" % (di, why))
        opsize_eq = op_size_stream(t2.ops)[:n] == op_size_stream(head5)
        print("  op+size identical to alignment point: %s" % opsize_eq)
        r2 = replay(t2.ops)
        r5h = replay(head5)
        compare_named(r2, r5h, "session-latest(9701) vs long-session truncated to %d" % n,
                      n_ops_a=len(t2.ops))
        # Also full live-end of each (unaligned)
        r5 = replay(t5.ops)
        compare_named(r2, r5, "session-latest FULL vs long-session FULL (NOT length-aligned)",
                      n_ops_a=len(t2.ops))

    if "gh_trace.txt.prev" in byname and "gh_trace2.txt" in byname:
        tp, _sp = byname["gh_trace.txt.prev"]
        t2, _s2 = byname["gh_trace2.txt"]
        n = min(len(tp.ops), len(t2.ops))
        print("\nUnpinned prev (%d ops, heap %08X, unnamed) vs pinned latest, first %d"
              % (len(tp.ops), tp.heap or 0, n))
        di, why = first_diverge(tp.ops, t2.ops[:n] if len(t2.ops) >= n else t2.ops)
        print("  first diverge at %s (%s)" % (di, why))
        opsize_eq = op_size_stream(tp.ops)[:n] == op_size_stream(t2.ops)[:n]
        print("  op+size identical to alignment: %s" % opsize_eq)
        # Offset comparison of live sets without names: (off, size) bags
        rp = replay(tp.ops)
        r2 = replay(t2.ops[:n])
        bins_p = {(off, rec[2]) for off, rec in rp["live"].items()}
        bins_2 = {(off, rec[2]) for off, rec in r2["live"].items()}
        print("  live bins prev %d aligned-2 %d  intersection %d (%s of prev)" % (
            len(bins_p), len(bins_2), len(bins_p & bins_2),
            pct(len(bins_p & bins_2), len(bins_p))))
        # Entity table
        print("  first alloc prev: %s size=%X off=%08X" % (
            tp.ops[0].op, tp.ops[0].size, tp.ops[0].off))
        print("  first alloc now:  %s size=%X off=%08X" % (
            t2.ops[0].op, t2.ops[0].size, t2.ops[0].off))

    print("\n\n======== SAME-SESSION CHECKPOINTS (discarded vs added) ========")
    if "gh_trace3.txt" in byname and "gh_trace5.txt" in byname:
        t3, s3 = byname["gh_trace3.txt"]
        t4, s4 = byname["gh_trace4.txt"]
        t5, s5 = byname["gh_trace5.txt"]
        checkpoint_delta(s3, s4, "trace3", "trace4")
        checkpoint_delta(s4, s5, "trace4", "trace5")
        checkpoint_delta(s3, s5, "trace3", "trace5")
        # Extra ops between 3 and 5: GH_BIG fraction
        extra = t5.ops[len(t3.ops):]
        n_big = sum(1 for o in extra if o.op == "B")
        n_ac = sum(1 for o in extra if o.op in "AC")
        n_b_bytes = sum(o.size for o in extra if o.op == "B")
        n_ac_bytes = sum(o.size for o in extra if o.op in "AC")
        print("\n  extra ops from trace3 to trace5: %d  B=%d (%s events)  B-bytes %.1f KB  A/C-bytes %.1f KB" % (
            len(extra), n_big, pct(n_big, len(extra)), n_b_bytes / 1024.0, n_ac_bytes / 1024.0))

    print("\n\n======== GH_BIG DETAIL (latest session) ========")
    t2 = byname["gh_trace2.txt"][0]
    bigs = [o for o in t2.ops if o.op == "B"]
    print("  B count %d  unique sizes %d  total bytes %.1f KB  min %d max %d" % (
        len(bigs), len(set(o.size for o in bigs)),
        sum(o.size for o in bigs) / 1024.0,
        min(o.size for o in bigs) if bigs else 0,
        max(o.size for o in bigs) if bigs else 0))
    print("  largest GH_BIG requests:")
    for o in sorted(bigs, key=lambda x: -x.size)[:8]:
        print("    %7d bytes (%0.1f KB)" % (o.size, o.size / 1024.0))
    # How many B remain "conceptually live"? We cannot know; they are not in our heap.
    print("  note: B ops have off=FFFFFFFF; they never enter the live offset map.")


if __name__ == "__main__":
    main()
