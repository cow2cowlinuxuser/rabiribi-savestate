"""Build a duration table for the Rabi-Ribi soundtrack from its FLAC files.

The game's Ogg comment headers carry no TITLE, so a track load in the allocator
trace cannot name itself. What it can report is a size: a decoded buffer, or an
oversize request recorded as a B operation. Divided by rate * channels * 2 that
size is a duration, and a duration can be matched against the store soundtrack,
which is the same 52 pieces of music with their names attached.

This writes the reference half of that match. Run it once; it only reads the
FLAC STREAMINFO block, so it is fast and needs nothing installed.
"""

import argparse
import os
import struct
import sys


def streaminfo(path):
    """Return (rate, channels, total_samples) from a FLAC file's STREAMINFO."""
    with open(path, "rb") as f:
        if f.read(4) != b"fLaC":
            return None
        # The first metadata block is always STREAMINFO.
        head = f.read(4)
        if len(head) != 4:
            return None
        size = int.from_bytes(head[1:4], "big")
        blk = f.read(size)
        if len(blk) < 18:
            return None

    # STREAMINFO packs these as a bit field starting at byte 10:
    #   20 bits rate, 3 bits channels-1, 5 bits depth-1, 36 bits total samples.
    bits = int.from_bytes(blk[10:18], "big")
    rate = (bits >> 44) & 0xFFFFF
    channels = ((bits >> 41) & 0x7) + 1
    total = bits & 0xFFFFFFFFF
    return rate, channels, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--ost",
        default=r"D:\SteamLibrary\steamapps\music\Rabi-Ribi - Original Soundtrack",
        help="soundtrack folder, the FLAC subfolder is found under it",
    )
    ap.add_argument("--out", default="ost.csv")
    args = ap.parse_args()

    files = []
    for root, _, names in os.walk(args.ost):
        for n in names:
            if n.lower().endswith(".flac"):
                files.append(os.path.join(root, n))
    files.sort()
    if not files:
        print("no FLAC files under %s" % args.ost, file=sys.stderr)
        return 1

    rows = []
    for p in files:
        info = streaminfo(p)
        if not info:
            print("  skipped, no STREAMINFO: %s" % os.path.basename(p))
            continue
        rate, ch, total = info
        name = os.path.splitext(os.path.basename(p))[0]
        # The store names every file with the same prefix; the track is the tail.
        for sep in (" - ",):
            if sep in name:
                name = name.split(sep, 1)[1]
        rows.append((name, rate, ch, total, total / float(rate)))

    rows.sort(key=lambda r: r[4])
    print("%-44s %7s %2s %11s %9s" % ("track", "rate", "ch", "samples", "seconds"))
    for name, rate, ch, total, secs in rows:
        print("%-44s %7d %2d %11d %9.3f" % (name[:44], rate, ch, total, secs))

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("track,rate,channels,samples,seconds\n")
        for name, rate, ch, total, secs in rows:
            f.write('"%s",%d,%d,%d,%.3f\n' % (name.replace('"', "'"), rate, ch, total, secs))
    print("\n%d track(s) written to %s" % (len(rows), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
