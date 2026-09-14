"""Read a savestate slot written by D3D9SW_SLOTFILE=1.

A slot is three files next to the game:

    d3d9sw_slotN.bin       the captured bytes, regions concatenated in order
    d3d9sw_slotN.regions   base/size/offset/prot per region, plain text
    d3d9sw_slotN.meta      the whole Slot struct, not read here

The .regions index is what makes the .bin addressable: without it the region
table is only available inside a C struct with 65536-entry arrays. With it the
snapshot is a flat, seekable address space.

What this is for. A value that cannot be found in a dump is a value the
snapshot does not contain, and that is a different problem from one it contains
and fails to restore. Ribbon trailing the player after a restore is one or the
other, and nothing else we have distinguishes them.

    python slotdump.py regions                       what was captured
    python slotdump.py find --f32 14507 4873         a float pair, anywhere
    python slotdump.py at 0E1CF4B8 --f32 8           read a known address
    python slotdump.py diff other.bin --f32          what moved between saves

Start with the player. The .regions header records the position the witness
logged at save time, and those exact floats must be findable in the dump. If
they are not, the reader is wrong and nothing else it says is worth reading.
"""

import argparse
import re
import struct
import sys
from pathlib import Path


class Slot:
    def __init__(self, binpath):
        self.bin = Path(binpath)
        stem = self.bin.with_suffix("")
        self.idx = Path(str(stem) + ".regions")
        if not self.bin.exists():
            sys.exit("no such file: %s" % self.bin)
        if not self.idx.exists():
            sys.exit(
                "missing %s - the .bin cannot be addressed without it. It is "
                "written alongside the snapshot when D3D9SW_SLOTFILE=1." % self.idx
            )
        self.header = []
        self.regions = []  # (base, size, offset, prot)
        for lineno, line in enumerate(self.idx.read_text().splitlines(), 1):
            if line.startswith("#"):
                self.header.append(line)
                continue
            if not line.strip():
                continue
            try:
                base, size, off, prot = line.split()
                self.regions.append(
                    (int(base, 16), int(size, 16), int(off, 16), int(prot, 16))
                )
            except ValueError:
                sys.exit(
                    "%s line %d is not 'base size offset prot' in hex:\n"
                    "    %s\n"
                    "The index was written by a build whose formatter could not "
                    "print the field. Take a fresh save with a current build."
                    % (self.idx.name, lineno, line)
                )
        if not self.regions:
            sys.exit("%s lists no regions" % self.idx.name)
        self.fh = self.bin.open("rb")
        self.total = sum(r[1] for r in self.regions)
        have = self.bin.stat().st_size
        if have < self.total:
            print(
                "warning: index describes %d bytes but the file holds %d - "
                "regions past the end will read short" % (self.total, have),
                file=sys.stderr,
            )

    def player_entity(self):
        """The entity address the witness recorded, from the index header."""
        for h in self.header:
            m = re.search(r"player entity ([0-9A-Fa-f]+)", h)
            if m:
                return int(m.group(1), 16)
        return None

    def player(self):
        """The position the witness recorded, from the index header."""
        for h in self.header:
            m = re.search(r"x (-?\d+)\s+y (-?\d+)", h)
            if m:
                return int(m.group(1)), int(m.group(2))
        return None

    def region_of(self, addr):
        for base, size, off, prot in self.regions:
            if base <= addr < base + size:
                return base, size, off, prot
        return None

    def read(self, addr, n):
        r = self.region_of(addr)
        if not r:
            return None
        base, size, off, _ = r
        skip = addr - base
        n = min(n, size - skip)
        self.fh.seek(off + skip)
        return self.fh.read(n)

    def chunks(self, step=1 << 22):
        """Every captured byte, as (address, bytes), in address order."""
        for base, size, off, _ in self.regions:
            pos = 0
            while pos < size:
                take = min(step, size - pos)
                self.fh.seek(off + pos)
                buf = self.fh.read(take)
                if not buf:
                    break
                yield base + pos, buf
                pos += len(buf)


def pack_values(kind, values):
    """The needle, as bytes. Floats are matched exactly; the game stores
    positions as whole numbers in f32, so this is not as brittle as it looks."""
    fmt = {"f32": "<f", "f64": "<d", "u32": "<I", "i32": "<i"}[kind]
    return b"".join(struct.pack(fmt, t(v)) for v, t in
                    ((v, float if kind.startswith("f") else int) for v in values))


def cmd_regions(slot, args):
    for h in slot.header:
        print(h)
    print("%-10s %-10s %-14s %-6s" % ("base", "size", "offset", "prot"))
    for base, size, off, prot in slot.regions:
        print("%08X   %08X   %012X   %X" % (base, size, off, prot))
    print("\n%d region(s), %.1f MB" % (len(slot.regions), slot.total / (1024.0 * 1024.0)))


def f32_span(v, tol):
    """The uint32 range covering [v-tol, v+tol]. Float bit patterns increase
    monotonically with value for a fixed sign, so a value range is a contiguous
    integer range and can be tested with two comparisons."""
    lo = struct.unpack("<I", struct.pack("<f", v - tol))[0]
    hi = struct.unpack("<I", struct.pack("<f", v + tol))[0]
    return (lo, hi) if lo <= hi else (hi, lo)


def cmd_find(slot, args):
    kind = args.kind
    if args.tol and kind != "f32":
        sys.exit("--tol only makes sense for f32")

    if not args.tol:
        needle = pack_values(kind, args.values)
        print("searching %.1f MB for %s %s (%s)"
              % (slot.total / (1024.0 * 1024.0), kind, args.values, needle.hex()))
        hits, tail, tailaddr = 0, b"", 0
        for addr, buf in slot.chunks():
            # Straddle chunk boundaries, but only when they are actually adjacent.
            if tail and tailaddr + len(tail) == addr:
                buf = tail + buf
                addr -= len(tail)
            pos = buf.find(needle)
            while pos >= 0:
                where = addr + pos
                r = slot.region_of(where)
                print("  %08X   in region %08X+%X" % (where, r[0], r[1]))
                hits += 1
                if hits >= args.limit:
                    print("  (stopping at %d)" % args.limit)
                    return
                pos = buf.find(needle, pos + 1)
            tail, tailaddr = buf[-len(needle):], addr + len(buf) - len(needle)
        print("%d hit(s)" % hits)
        if hits == 0 and kind == "f32":
            print("\nNothing matched exactly. Positions are rarely whole numbers -\n"
                  "the log prints them as int casts - so try --tol 1.")
        return

    # Tolerant search. Every word in range is a candidate, which is far too many
    # to test in Python one at a time over 388 MB, so the high half of the first
    # value is used as a byte filter first: floats within a unit of each other
    # almost always share it, and bytes.find runs at C speed. Candidates that
    # survive are then checked properly.
    spans = [f32_span(v, args.tol) for v in args.values]
    prefix = struct.pack("<f", args.values[0])[2:4]
    print("searching %.1f MB for %s within %g, high half %s"
          % (slot.total / (1024.0 * 1024.0), args.values, args.tol, prefix.hex()))
    hits = 0
    for addr, buf in slot.chunks():
        pos = buf.find(prefix)
        while pos >= 0:
            start = pos - 2  # the prefix sits at bytes 2..3 of the value
            if start >= 0 and start % 4 == 0 and start + 4 * len(spans) <= len(buf):
                vals = struct.unpack_from("<%dI" % len(spans), buf, start)
                if all(lo <= v <= hi for v, (lo, hi) in zip(vals, spans)):
                    where = addr + start
                    r = slot.region_of(where)
                    shown = struct.unpack_from("<%df" % len(spans), buf, start)
                    print("  %08X   in region %08X+%X   %s"
                          % (where, r[0], r[1], "  ".join("%g" % f for f in shown)))
                    hits += 1
                    if hits >= args.limit:
                        print("  (stopping at %d)" % args.limit)
                        return
            pos = buf.find(prefix, pos + 1)
    print("%d hit(s)" % hits)


def cmd_scan(slot, args):
    """Every adjacent float pair whose values fall in two ranges.

    find answers "is this value here", which needs the value first, and that is
    the wrong way round for a character whose position is unknown. Four saves
    searched around the player turned up three addresses holding 16419.2 in
    every one of them - the same bits at the same addresses while the player
    moved 2300 units - which is a constant, not a follower. The question worth
    asking is not "is Ribbon at x" but "what in this region looks like a
    position at all", and that is an enumeration.

    Restrict it with --in. The entity arena is half a megabyte and answers
    instantly; the whole snapshot is 388 MB and takes a minute.
    """
    xlo, xhi = args.x
    ylo, yhi = args.y
    xs, ys = f32_span((xlo + xhi) / 2.0, (xhi - xlo) / 2.0), None
    ys = f32_span((ylo + yhi) / 2.0, (yhi - ylo) / 2.0)
    want = None
    if args.region is not None:
        want = int(args.region, 16)
    elif not args.everywhere:
        # The arena the player lives in, which is where the other entities are
        # and is half a megabyte rather than 388. Its base moves every session -
        # three consecutive runs put it at 0E34F000, 0E03F000 and 0E21F000 - so
        # looking it up by hand is a step that exists only to be got wrong. The
        # index header records the player's entity address at save time.
        ent = slot.player_entity()
        if ent is None:
            sys.exit("the index header has no player entity, so there is no "
                     "default region - pass --in <base> or --everywhere")
        r = slot.region_of(ent)
        if not r:
            sys.exit("the player entity %08X is in no captured region" % ent)
        want = r[0]
        print("defaulting to the player's arena at %08X (--everywhere for all)"
              % want)
    if want is not None:
        regions = [r for r in slot.regions if r[0] == want]
        if not regions:
            sys.exit("no captured region starts at %08X - run 'regions' to list them"
                     % want)
    else:
        regions = slot.regions
        print("scanning all %.1f MB; --in <base> restricts this to one region"
              % (slot.total / (1024.0 * 1024.0)))
    span = sum(r[1] for r in regions)
    print("scanning %.1f MB for x in [%g, %g] and y in [%g, %g]"
          % (span / (1024.0 * 1024.0), xlo, xhi, ylo, yhi))
    hits = 0
    for base, size, off, _ in regions:
        pos = 0
        while pos < size:
            take = min(1 << 22, size - pos)
            slot.fh.seek(off + pos)
            buf = slot.fh.read(take)
            if not buf:
                break
            n = len(buf) // 4
            words = struct.unpack_from("<%dI" % n, buf, 0)
            for i in range(n - 1):
                if xs[0] <= words[i] <= xs[1] and ys[0] <= words[i + 1] <= ys[1]:
                    where = base + pos + i * 4
                    x, y = struct.unpack_from("<2f", buf, i * 4)
                    print("  %08X   in region %08X+%X   %.2f  %.2f"
                          % (where, base, size, x, y))
                    hits += 1
                    if hits >= args.limit:
                        print("  (stopping at %d)" % args.limit)
                        return
            pos += len(buf)
    print("%d hit(s)" % hits)


def cmd_at(slot, args):
    addr = int(args.addr, 16)
    r = slot.region_of(addr)
    if not r:
        print("%08X is NOT in the snapshot - no captured region covers it" % addr)
        print("That is itself the answer: nothing here can restore that address.")
        return
    fmt, width = {"f32": ("<f", 4), "f64": ("<d", 8), "u32": ("<I", 4), "i32": ("<i", 4)}[args.kind]
    raw = slot.read(addr, width * args.count)
    print("%08X in region %08X+%X:" % (addr, r[0], r[1]))
    for i in range(0, len(raw) - width + 1, width):
        val = struct.unpack_from(fmt, raw, i)[0]
        print("  +%-4X %-18s %s" % (i, val, raw[i:i + width].hex()))


def cmd_diff(slot, args):
    other = Slot(args.other)
    fmt, width = {"f32": ("<f", 4), "u32": ("<I", 4), "i32": ("<i", 4)}[args.kind]
    print("comparing captured regions common to both slots")
    shown, differ = 0, 0
    for base, size, off, _ in slot.regions:
        if not other.region_of(base):
            continue
        a = slot.read(base, size)
        b = other.read(base, size)
        if a is None or b is None or a == b:
            continue
        n = min(len(a), len(b))
        for i in range(0, n - width + 1, width):
            if a[i:i + width] == b[i:i + width]:
                continue
            differ += 1
            if shown >= args.limit:
                continue
            va = struct.unpack_from(fmt, a, i)[0]
            vb = struct.unpack_from(fmt, b, i)[0]
            if args.near is not None and not (args.near[0] <= va <= args.near[1]):
                continue
            print("  %08X  %-16s -> %-16s" % (base + i, va, vb))
            shown += 1
    print("%d word(s) differ" % differ)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bin", default="d3d9sw_slot0.bin", help="the snapshot to read")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("regions", help="list what was captured")
    s.set_defaults(fn=cmd_regions)

    s = sub.add_parser("find", help="search for a value or run of values")
    s.add_argument("values", nargs="+", type=float)
    s.add_argument("--kind", dest="kind", default="f32",
                   choices=["f32", "f64", "u32", "i32"])
    s.add_argument("--tol", type=float, default=0.0,
                   help="match f32 within this much, for values the log only "
                        "prints as whole numbers")
    s.add_argument("--limit", type=int, default=64)
    s.set_defaults(fn=cmd_find)

    s = sub.add_parser("scan", help="enumerate anything shaped like a position")
    s.add_argument("--x", nargs=2, type=float, required=True, metavar=("LO", "HI"))
    s.add_argument("--y", nargs=2, type=float, required=True, metavar=("LO", "HI"))
    s.add_argument("--in", dest="region", metavar="BASE",
                   help="only this region, by base address in hex")
    s.add_argument("--everywhere", action="store_true",
                   help="the whole snapshot instead of the player's arena")
    s.add_argument("--limit", type=int, default=200)
    s.set_defaults(fn=cmd_scan)

    s = sub.add_parser("at", help="read a known address")
    s.add_argument("addr")
    s.add_argument("count", nargs="?", type=int, default=8)
    s.add_argument("--kind", dest="kind", default="f32",
                   choices=["f32", "f64", "u32", "i32"])
    s.set_defaults(fn=cmd_at)

    s = sub.add_parser("diff", help="what changed between two snapshots")
    s.add_argument("other")
    s.add_argument("--kind", dest="kind", default="f32", choices=["f32", "u32", "i32"])
    s.add_argument("--near", nargs=2, type=float,
                   help="only values in this range, to cut the noise")
    s.add_argument("--limit", type=int, default=64)
    s.set_defaults(fn=cmd_diff)

    args = p.parse_args()
    # find takes its values as floats for convenience; the packer casts.
    slot = Slot(args.bin)
    args.fn(slot, args)


if __name__ == "__main__":
    main()
