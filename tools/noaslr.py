#!/usr/bin/env python3
"""Clear DYNAMICBASE on our own wrapper DLLs.

Linux counterpart of tools/noaslr.ps1. linux-rabi.sh calls this after zig cc
sets ImageBase with -Wl,--image-base. Only the DllCharacteristics bit is
touched. ImageBase must NOT be rewritten here: the loader relocates by
(load address - header ImageBase), so editing that field afterwards without
rewriting the image would leave every absolute address pointing at where the
linker originally put it. The relocation table is left intact too, so if a
base is ever occupied the loader can still move the image rather than fail
to load it.

It only works because system-wide ForceRelocateImages is off. This script
cannot query that policy from Linux; noaslr.ps1 does on Windows.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

DYNAMICBASE = 0x40


def pin(path: Path) -> None:
    if not path.is_file():
        print(f"  {path} missing")
        return
    data = bytearray(path.read_bytes())
    if len(data) < 0x40 or data[0:2] != b"MZ":
        print(f"  {path} is not a PE")
        return
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 26 > len(data) or struct.unpack_from("<I", data, pe)[0] != 0x4550:
        print(f"  {path} is not a PE")
        return
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dc_off = opt + 70
    if dc_off + 2 > len(data):
        print(f"  {path} is not a PE")
        return
    dc = struct.unpack_from("<H", data, dc_off)[0]
    if magic == 0x10B:
        base = struct.unpack_from("<I", data, opt + 28)[0]
    else:
        base = struct.unpack_from("<Q", data, opt + 24)[0]
    name = path.name
    if not (dc & DYNAMICBASE):
        print(f"  {name:<22} already fixed at {base:08X}")
        return
    new = dc & ~DYNAMICBASE
    struct.pack_into("<H", data, dc_off, new)
    path.write_bytes(data)
    print(f"  {name:<22} {dc:04X} -> {new:04X}, pinned at {base:08X}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: noaslr.py <dll> [dll ...]", file=sys.stderr)
        return 2
    for arg in sys.argv[1:]:
        pin(Path(arg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
