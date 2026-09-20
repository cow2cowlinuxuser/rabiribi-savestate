#!/usr/bin/env python3
"""Clear DYNAMICBASE on wrapper DLLs. Same job as tools/noaslr.ps1.

Only DllCharacteristics is edited. ImageBase stays the linker value
(-Wl,--image-base); rewriting it here would leave absolute addresses pointing
at the wrong place. The reloc table is left intact.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DYNAMICBASE = 0x40


def pin(path: Path) -> None:
    data = bytearray(path.read_bytes())
    if data[:2] != b"MZ":
        print(f"  {path.name} is not a PE")
        return
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if struct.unpack_from("<I", data, pe)[0] != 0x4550:
        print(f"  {path.name} is not a PE")
        return
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dc_off = opt + 70
    dc = struct.unpack_from("<H", data, dc_off)[0]
    if magic == 0x10B:
        base = struct.unpack_from("<I", data, opt + 28)[0]
    else:
        base = struct.unpack_from("<Q", data, opt + 24)[0]
    if not (dc & DYNAMICBASE):
        print(f"  {path.name:<22} already fixed at {base:08X}")
        return
    new = dc & ~DYNAMICBASE
    struct.pack_into("<H", data, dc_off, new)
    path.write_bytes(data)
    print(f"  {path.name:<22} {dc:04X} -> {new:04X}, pinned at {base:08X}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: noaslr.py FILE.dll [FILE.dll ...]", file=sys.stderr)
        return 2
    for a in sys.argv[1:]:
        p = Path(a)
        if not p.is_file():
            print(f"  {p} missing")
            continue
        pin(p)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
