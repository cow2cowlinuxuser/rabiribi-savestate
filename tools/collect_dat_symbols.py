#!/usr/bin/env python3
"""List DAT_* / FUN_* symbols in Ghidra-lifted RBO_Ex3 C.

Used to pack BSS instead of mapping the PE's 33 MB .data hole onto MEM1.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "cdir",
        nargs="?",
        type=Path,
        default=Path(r"F:\Ragnarok Battle Offline + Ex1-3\EXTRACTION\re\RBO_Ex3\09_functions_c"),
    )
    ap.add_argument("-o", "--out", type=Path, default=None)
    args = ap.parse_args()

    dat: set[str] = set()
    fun: set[str] = set()
    n = 0
    for p in sorted(args.cdir.glob("*.c")):
        n += 1
        t = p.read_text(encoding="utf-8", errors="replace")
        dat.update(re.findall(r"\bDAT_[0-9a-fA-F]+\b", t))
        fun.update(re.findall(r"\bFUN_[0-9a-fA-F]+\b", t))

    lines = [
        f"# Ghidra C {args.cdir}",
        f"# files={n} DAT={len(dat)} FUN={len(fun)}",
        "",
        "## DAT",
        *sorted(dat),
        "",
        "## FUN",
        *sorted(fun),
        "",
    ]
    text = "\n".join(lines)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
        print(f"wrote {args.out} files={n} dat={len(dat)} fun={len(fun)}")
    else:
        print(f"files={n} dat={len(dat)} fun={len(fun)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
