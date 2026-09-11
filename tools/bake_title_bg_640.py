"""Bake title_bg*.img to 640x480 RGBA (ver 7, fmt 2).

1024x512 sources live outside the SD pack so a card copy of RBO/ cannot
pick them up:

  F:\\rbo_fabre_proto\\title_bg_src1024\\title_bg001.img .. 008

Writes 640x480 into sd_pack/RBO/DATA/CG/Title/Title_Bg/.
The Cube loader already reads w/h from the header.
"""
from __future__ import annotations

import struct
from pathlib import Path

from PIL import Image

DST = Path(r"F:\rbo_fabre_proto\sd_pack\RBO\DATA\CG\Title\Title_Bg")
SRC = Path(r"F:\rbo_fabre_proto\title_bg_src1024")


def load_img(path: Path) -> tuple[Image.Image, int, int]:
    data = path.read_bytes()
    magic, ver, fmt, w, h = struct.unpack_from("<5I", data, 0)
    if ver not in (6, 7) or fmt != 2:
        raise SystemExit(f"bad {path} ver={ver} fmt={fmt} {w}x{h}")
    need = w * h * 4
    im = Image.frombytes("RGBA", (w, h), data[20 : 20 + need])
    return im, magic, ver


def write_img(path: Path, im: Image.Image, magic: int, ver: int) -> None:
    im = im.convert("RGBA")
    w, h = im.size
    payload = im.tobytes()
    hdr = struct.pack("<5I", magic, ver, 2, w, h)
    path.write_bytes(hdr + payload)


def main() -> None:
    if not SRC.is_dir():
        raise SystemExit(f"missing 1024 sources: {SRC}")
    DST.mkdir(parents=True, exist_ok=True)
    for n in range(1, 9):
        name = f"title_bg{n:03d}.img"
        bak = SRC / name
        live = DST / name
        if not bak.exists():
            raise SystemExit(f"missing {bak}")
        src, magic, ver = load_img(bak)
        if src.size != (1024, 512):
            raise SystemExit(f"{bak} is {src.size}, expected 1024x512")
        baked = src.resize((640, 480), Image.Resampling.LANCZOS)
        write_img(live, baked, magic, ver)
        print(f"{name} {src.size} -> {baked.size} {live.stat().st_size} bytes")


if __name__ == "__main__":
    main()
