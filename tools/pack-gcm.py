#!/usr/bin/env python3
"""Pack an RBO folder + DOL + stub apploader into a trimmed GameCube GCM.

The FST wraps files as prefix/relpath (default prefix RBO) so disc paths match
the SD layout: RBO/ASSETS/TITLE.TPL, RBO/SOUND/..., RBO/SCRIPTS/...

The DOL is boot.bin's main executable, not an FST file.
Does not pad to a full 1.4 GB image.
"""

from __future__ import print_function

import argparse
import os
import struct
import sys
from pathlib import Path


GC_MAGIC = 0xC2339F3D
APPLDR_OFF = 0x2440
BI2_OFF = 0x440
BI2_SIZE = 0x2000
BOOT_SIZE = 0x440

SKIP_NAMES = {
    "thumbs.db",
    "desktop.ini",
    ".ds_store",
    "save.dat",
    "boot.bin",
    "bi2.bin",
    "fst.bin",
    "apploader.img",
    "apploader.ldr",
    "apploader.lbl",
    "game.toc",
    "iso.hdr",
    "fsn.bin",
    "start.dol",
}
SKIP_SUFFIXES = {".dol", ".elf", ".gcm", ".iso", ".map"}
SKIP_DIRS = {".git", "&&systemdata"}


def align_up(n, a):
    return (n + (a - 1)) & ~(a - 1)


def be32(buf, off, val):
    struct.pack_into(">I", buf, off, val & 0xFFFFFFFF)


def should_skip(path, dol_resolved):
    name = path.name
    low = name.lower()
    if low in SKIP_NAMES:
        return True
    if path.suffix.lower() in SKIP_SUFFIXES:
        return True
    try:
        if dol_resolved and path.resolve() == dol_resolved:
            return True
    except OSError:
        pass
    return False


class Node(object):
    __slots__ = (
        "name",
        "is_dir",
        "children",
        "src",
        "size",
        "disc_off",
        "name_off",
        "index",
        "parent",
        "next_idx",
    )

    def __init__(self, name, is_dir):
        self.name = name
        self.is_dir = is_dir
        self.children = {}
        self.src = None
        self.size = 0
        self.disc_off = 0
        self.name_off = 0
        self.index = 0
        self.parent = 0
        self.next_idx = 0


def insert_file(root, parts, src, size):
    node = root
    for i, part in enumerate(parts):
        if not part or part in (".", ".."):
            continue
        last = i == len(parts) - 1
        child = node.children.get(part)
        if child is None:
            child = Node(part, not last)
            node.children[part] = child
        node = child
        if last:
            node.is_dir = False
            node.src = src
            node.size = size


def flatten(root):
    entries = []

    def walk(node, parent_idx):
        node.index = len(entries)
        node.parent = parent_idx
        entries.append(node)
        if node.is_dir:
            for key in sorted(node.children.keys(), key=lambda s: s.upper()):
                walk(node.children[key], node.index)
            node.next_idx = len(entries)
        return node.index

    walk(root, 0)
    return entries


def build_strings(entries):
    blob = bytearray(b"\x00")
    entries[0].name_off = 0
    for node in entries[1:]:
        node.name_off = len(blob)
        blob.extend(node.name.encode("ascii", "replace"))
        blob.append(0)
    return bytes(blob)


def build_fst(entries, strings):
    raw = bytearray(len(entries) * 12 + len(strings))
    for node in entries:
        flags = 1 if node.is_dir else 0
        w0 = (flags << 24) | (node.name_off & 0xFFFFFF)
        be32(raw, node.index * 12 + 0, w0)
        if node.is_dir:
            be32(raw, node.index * 12 + 4, node.parent)
            be32(raw, node.index * 12 + 8, node.next_idx)
        else:
            be32(raw, node.index * 12 + 4, node.disc_off)
            be32(raw, node.index * 12 + 8, node.size)
    raw[len(entries) * 12 :] = strings
    return bytes(raw)


def collect_files(root_dir, prefix, dol_path):
    root_dir = root_dir.resolve()
    dol_resolved = dol_path.resolve() if dol_path else None
    files = []
    for dirpath, dirnames, filenames in os.walk(str(root_dir)):
        dirnames[:] = [
            d
            for d in dirnames
            if d not in SKIP_DIRS and not d.startswith("&") and not d.startswith(".")
        ]
        dirnames.sort(key=lambda s: s.upper())
        for name in sorted(filenames, key=lambda s: s.upper()):
            p = Path(dirpath) / name
            if should_skip(p, dol_resolved):
                continue
            rel = p.relative_to(root_dir).as_posix()
            parts = []
            if prefix:
                parts.append(prefix)
            parts.extend([x for x in rel.split("/") if x])
            size = p.stat().st_size
            if size <= 0:
                continue
            files.append((parts, p, size))
    return files


def write_boot(buf, game_id, maker, name, dol_off, fst_off, fst_len, user_off, user_len):
    gid = (game_id + "XXXX")[:4].encode("ascii")
    mk = (maker + "XX")[:2].encode("ascii")
    buf[0:4] = gid
    buf[4:6] = mk
    be32(buf, 0x1C, GC_MAGIC)
    nb = name.encode("ascii", "replace")[:0x3DF]
    buf[0x20 : 0x20 + len(nb)] = nb
    be32(buf, 0x420, dol_off)
    be32(buf, 0x424, fst_off)
    be32(buf, 0x428, fst_len)
    be32(buf, 0x42C, fst_len)
    be32(buf, 0x430, 0x81700000)  # FST load address (unused by our stub)
    be32(buf, 0x434, user_off)
    be32(buf, 0x438, user_len)


def write_bi2(buf):
    # Simulated MEM1 24 MiB, country USA.
    be32(buf, 0x00, 0)
    be32(buf, 0x04, 0x01800000)
    be32(buf, 0x18, 1)


def pack(root_dir, dol_path, appldr_path, out_path, prefix, game_id, maker, disc_name):
    dol = Path(dol_path).read_bytes()
    appldr = Path(appldr_path).read_bytes()
    if len(dol) < 0x100:
        raise SystemExit("DOL is too small: %s" % dol_path)
    if len(appldr) < 16:
        raise SystemExit("apploader is too small: %s" % appldr_path)

    files = collect_files(Path(root_dir), prefix, Path(dol_path))
    tree = Node("", True)
    for parts, src, size in files:
        insert_file(tree, parts, src, size)
    entries = flatten(tree)
    strings = build_strings(entries)

    appldr_size = len(appldr)
    trailer = (32 - (appldr_size % 32)) % 32
    appldr_end = APPLDR_OFF + 0x20 + appldr_size + trailer
    dol_off = align_up(appldr_end, 256)
    dol_end = dol_off + len(dol)

    cursor = align_up(dol_end, 32)
    file_nodes = [e for e in entries if not e.is_dir]
    for node in file_nodes:
        node.disc_off = cursor
        cursor = align_up(cursor + node.size, 32)

    user_off = file_nodes[0].disc_off if file_nodes else cursor
    fst_off = align_up(cursor, 32)
    fst = build_fst(entries, strings)
    fst_len = len(fst)
    image_len = align_up(fst_off + fst_len, 32)
    user_len = image_len - user_off

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with out_path.open("wb") as fp:
        boot = bytearray(BOOT_SIZE)
        write_boot(
            boot, game_id, maker, disc_name, dol_off, fst_off, fst_len, user_off, user_len
        )
        fp.write(boot)

        bi2 = bytearray(BI2_SIZE)
        write_bi2(bi2)
        fp.write(bi2)

        hdr = bytearray(0x20)
        rev = b"2026/08/12"
        hdr[0:16] = rev + b"\x00" * (16 - len(rev))
        be32(hdr, 0x10, 0x81200000)
        be32(hdr, 0x14, appldr_size)
        be32(hdr, 0x18, trailer)
        fp.write(hdr)
        fp.write(appldr)
        if trailer:
            fp.write(b"\x00" * trailer)

        fp.write(b"\x00" * (dol_off - fp.tell()))
        fp.write(dol)

        for node in file_nodes:
            fp.write(b"\x00" * (node.disc_off - fp.tell()))
            with node.src.open("rb") as inf:
                while True:
                    chunk = inf.read(1024 * 1024)
                    if not chunk:
                        break
                    fp.write(chunk)

        fp.write(b"\x00" * (fst_off - fp.tell()))
        fp.write(fst)
        if image_len > fp.tell():
            fp.write(b"\x00" * (image_len - fp.tell()))

    print(
        "GCM %s  %d bytes  %d files  DOL@0x%X  FST@0x%X (%d)"
        % (out_path, image_len, len(file_nodes), dol_off, fst_off, fst_len)
    )


def main():
    ap = argparse.ArgumentParser(description="Pack RBO folder into a GameCube GCM")
    ap.add_argument("--root", required=True, help="Folder to pack (e.g. sd_pack/RBO)")
    ap.add_argument("--dol", required=True, help="Boot DOL (not stored in the FST)")
    ap.add_argument("--apploader", required=True, help="Stub apploader code (no 0x20 header)")
    ap.add_argument("-o", "--out", required=True, help="Output .gcm path")
    ap.add_argument("--prefix", default="RBO", help="FST top folder (empty to unpack at root)")
    ap.add_argument("--game-id", default="GRBE", help="4-character game code")
    ap.add_argument("--maker", default="HB", help="2-character maker code")
    ap.add_argument("--name", default="Ragnarok Battle Offline", help="Disc name")
    args = ap.parse_args()
    prefix = args.prefix.strip("/").replace("\\", "/")
    pack(
        args.root,
        args.dol,
        args.apploader,
        args.out,
        prefix,
        args.game_id,
        args.maker,
        args.name,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
