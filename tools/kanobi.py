"""Read Rabi-Ribi's pack.kanobi archive.

The archive is a DxLib DXA version 4 container with every byte XORed against a
repeating 12-byte key. The key was recovered by frequency bias: the plaintext has
enough zero padding that the most common byte at each of the twelve positions is
the key byte itself, and it came out the same over 16 MB with a 3.4x margin.

What this is for: the game's Ogg comment headers carry no TITLE, so a track load
seen in the allocator trace cannot name itself. The archive can. Every Ogg stream
in it has a serial number that is fixed per file and a final granule position that
is the exact sample count, and a sample count is a duration, and the store
soundtrack supplies durations with names attached.

  py -3 kanobi.py streams        list every Ogg stream with serial and length
  py -3 kanobi.py names          list the file names the archive carries
  py -3 kanobi.py match          name each stream against ost.csv
"""

import argparse
import csv
import os
import re
import struct
import sys

KEY = bytes([0xBE, 0x43, 0xBD, 0x5A, 0xA5, 0xF6, 0xB4, 0xD7, 0x89, 0x4E, 0xC5, 0xFD])
DEFAULT = r"C:\Program Files (x86)\Steam\steamapps\common\Rabi-Ribi\pack.kanobi"


def decrypt(path):
    raw = open(path, "rb").read()
    key = (KEY * (len(raw) // len(KEY) + 2))[: len(raw)]
    return bytes(a ^ b for a, b in zip(raw, key))


def pages(d):
    """Yield (offset, header_type, granule, serial, seq) for every Ogg page."""
    at = d.find(b"OggS")
    while at >= 0:
        if at + 27 <= len(d) and d[at + 4] == 0:
            htype = d[at + 5]
            granule, serial, seq = struct.unpack_from("<qII", d, at + 6)
            yield at, htype, granule, serial, seq
        at = d.find(b"OggS", at + 4)


def streams(d):
    """Collapse pages into one record per logical stream, in archive order."""
    first = {}
    last = {}
    for off, htype, granule, serial, seq in pages(d):
        if serial not in first:
            first[serial] = off
        # A negative granule means the page holds no completed packet.
        if granule > last.get(serial, -1):
            last[serial] = granule

    out = []
    for serial, off in first.items():
        total = last.get(serial, 0)
        # The identification header follows the page header of the first page.
        rate, ch = 0, 0
        idp = d.find(b"\x01vorbis", off, off + 512)
        if idp >= 0:
            ch = d[idp + 11]
            rate = struct.unpack_from("<I", d, idp + 12)[0]
        vendor = b""
        m = re.compile(rb"Xiph\.Org libVorbis I \d+ \([^)]{0,40}\)").search(
            d, off, off + 8192
        )
        if m:
            vendor = m.group(0)
        secs = total / float(rate) if rate else 0.0
        out.append(
            {
                "offset": off,
                "serial": serial,
                "rate": rate,
                "channels": ch,
                "samples": total,
                "seconds": secs,
                "vendor": vendor.decode("latin1"),
                "vendor_bytes": len(vendor) + 1 if vendor else 0,
            }
        )
    out.sort(key=lambda r: r["offset"])
    return out


def name_at(d, nametable, addr):
    """Decode one name table entry: a 4-byte head, the upper-cased name, then
    the real one, each NUL padded to a 4-byte boundary."""
    at = nametable + addr + 4
    end = d.index(b"\0", at)
    upper_len = ((end - at) // 4 + 1) * 4
    at += upper_len
    end = d.index(b"\0", at)
    return d[at:end].decode("latin1")


def files(d):
    """Walk the DXA file table. Entries are 44 bytes: name address, attributes,
    three FILETIMEs, then the data address, size and compressed size."""
    h = struct.unpack_from("<HHIIIII", d, 0)
    data_start, nametable = h[3], h[4]
    filetable = h[4] + h[5]
    dirtable = h[4] + h[6]

    out = []
    at = filetable
    while at + 44 <= dirtable:
        name_addr, attr = struct.unpack_from("<II", d, at)
        data_addr, size, packed = struct.unpack_from("<III", d, at + 32)
        if not attr & 0x10:  # not a directory
            try:
                nm = name_at(d, nametable, name_addr)
            except ValueError:
                nm = ""
            if nm:
                out.append({
                    "name": nm,
                    "offset": data_start + data_addr,
                    "size": size,
                    "packed": None if packed == 0xFFFFFFFF else packed,
                })
        at += 44
    return out


def load_ost(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            out.append((row["track"], float(row["seconds"])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["streams", "names", "match", "files"])
    ap.add_argument("--pack", default=DEFAULT)
    ap.add_argument("--ost", default="ost.csv")
    ap.add_argument("--out", default="tracks.csv")
    ap.add_argument("--tol", type=float, default=0.30, help="seconds of slack")
    args = ap.parse_args()

    if not os.path.exists(args.pack):
        print("missing: %s" % args.pack, file=sys.stderr)
        return 1
    d = decrypt(args.pack)
    head = struct.unpack_from("<HHIIIII", d, 0)
    if d[:2] != b"DX":
        print("not a DXA archive after decrypt - the key may be wrong", file=sys.stderr)
        return 1

    if args.cmd == "names":
        found = sorted(
            set(m.group(0).decode("latin1")
                for m in re.finditer(rb"[A-Za-z0-9_\-./\\ ]{1,60}\.(ogg|png|wav)", d))
        )
        for n in found:
            print("  " + n)
        print("  %d name(s)" % len(found))
        return 0

    fl = files(d)

    if args.cmd == "files":
        print("%d file(s) in the archive" % len(fl))
        for e in sorted(fl, key=lambda e: e["offset"])[:80]:
            print("  %-28s %11d  %10d bytes%s" % (
                e["name"], e["offset"], e["size"],
                "" if e["packed"] is None else "  packed to %d" % e["packed"]))
        return 0

    st = streams(d)

    # A stream belongs to the file whose extent contains its first page.
    spans = sorted(((e["offset"], e["offset"] + e["size"], e["name"]) for e in fl))
    def owner(off):
        for lo, hi, nm in spans:
            if lo <= off < hi:
                return nm
        return ""

    print("DXA version %d, %d stream(s), %d file(s)\n" % (head[1], len(st), len(fl)))
    print("%-12s %-10s %2s %10s %9s %5s  %s" % (
        "offset", "serial", "ch", "samples", "seconds", "vend", "file"))

    rows = []
    for s in st:
        nm = owner(s["offset"])
        print("%-12d %-10d %2d %10d %9.3f %5d  %s" % (
            s["offset"], s["serial"], s["channels"],
            s["samples"], s["seconds"], s["vendor_bytes"], nm))
        rows.append((s, nm))

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("file,offset,serial,rate,channels,samples,seconds,vendor_bytes,vendor\n")
        for s, nm in rows:
            f.write('"%s",%d,%d,%d,%d,%d,%.3f,%d,"%s"\n' % (
                nm, s["offset"], s["serial"], s["rate"], s["channels"],
                s["samples"], s["seconds"], s["vendor_bytes"], s["vendor"]))
    named = sum(1 for _, n in rows if n)
    print("\n%d of %d stream(s) attached to a file name, written to %s"
          % (named, len(rows), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
