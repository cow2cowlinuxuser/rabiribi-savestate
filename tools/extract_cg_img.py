#!/usr/bin/env python3
"""Extract named IMGs from CG.PAC using EXTRACTION/dumps/CG_toc.csv.

  python tools/extract_cg_img.py LOBBY.IMG RBO/DATA/CG/CharaSel/Lobby.Img
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

REPO = Path(r"F:\Ragnarok Battle Offline + Ex1-3")
PAC = REPO / "INSTALL" / "DATA" / "CG.PAC"
TOC = REPO / "EXTRACTION" / "dumps" / "CG_toc.csv"
PROTO = Path(__file__).resolve().parents[1]


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: extract_cg_img.py PAC_NAME dest_rel_under_sd_pack", file=sys.stderr)
        return 2
    want = sys.argv[1].upper()
    dest = PROTO / "sd_pack" / sys.argv[2]
    start = size = None
    with TOC.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if (row.get("Name") or "").upper() == want:
                start = int(row["Start"])
                size = int(row["Size"])
                break
    if start is None:
        print(f"error: {want} not in {TOC}", file=sys.stderr)
        return 1
    if not PAC.is_file():
        print(f"error: no PAC at {PAC}", file=sys.stderr)
        return 1
    with PAC.open("rb") as fp:
        fp.seek(start)
        blob = fp.read(size)
    if len(blob) != size:
        print(f"error: short read {len(blob)}/{size}", file=sys.stderr)
        return 1
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(blob)
    print(f"wrote {dest} ({size} bytes) from {want} @{start}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
