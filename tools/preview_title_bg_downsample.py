"""Decode title_bg*.img and bake 640x480 comparison strips.

IMG: ver u32@4, fmt u32@8 (2=RGBA), w@12, h@16, pixels@20.
Cube already stretches 1024x512 onto 640x480. This shows that vs a
baked 640x480 vs a baked 512x256 upscaled, all quantized to RGB565.
"""
from __future__ import annotations

import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SRC_DIR = Path(r"F:\rbo_fabre_proto\title_bg_src1024")
OUT_DIR = Path(r"F:\rbo_fabre_proto\textures\title\downsample_preview")


def load_img(path: Path) -> Image.Image:
    data = path.read_bytes()
    if len(data) < 20:
        raise SystemExit(f"short {path}")
    _magic, ver, fmt, w, h = struct.unpack_from("<5I", data, 0)
    if ver not in (6, 7) or w == 0 or h == 0:
        raise SystemExit(f"bad header {path} ver={ver} fmt={fmt} {w}x{h}")
    px = data[20:]
    if fmt == 2:
        need = w * h * 4
        if len(px) < need:
            raise SystemExit(f"short rgba {path}")
        im = Image.frombytes("RGBA", (w, h), px[:need])
        return im.convert("RGB")
    raise SystemExit(f"unhandled fmt {fmt} in {path}")


def to_rgb565(im: Image.Image) -> Image.Image:
    im = im.convert("RGB")
    px = bytearray(im.tobytes())
    for i in range(0, len(px), 3):
        px[i] &= 0xF8
        px[i + 1] &= 0xFC
        px[i + 2] &= 0xF8
    return Image.frombytes("RGB", im.size, bytes(px))


def stretch(im: Image.Image, size: tuple[int, int], how: Image.Resampling) -> Image.Image:
    return im.resize(size, how)


def label_bar(im: Image.Image, text: str) -> Image.Image:
    out = Image.new("RGB", (im.width, im.height + 22), (20, 20, 24))
    out.paste(im, (0, 22))
    d = ImageDraw.Draw(out)
    d.text((8, 4), text, fill=(230, 230, 230))
    return out


def hstack(imgs: list[Image.Image], gap: int = 8) -> Image.Image:
    w = sum(i.width for i in imgs) + gap * (len(imgs) - 1)
    h = max(i.height for i in imgs)
    out = Image.new("RGB", (w, h), (12, 12, 16))
    x = 0
    for i in imgs:
        out.paste(i, (x, 0))
        x += i.width + gap
    return out


def crop4x(im: Image.Image, box: tuple[int, int, int, int]) -> Image.Image:
    c = im.crop(box)
    return c.resize((c.width * 4, c.height * 4), Image.Resampling.NEAREST)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    sheets = []
    crop_box = (360, 140, 360 + 80, 140 + 60)
    sizes = {
        "1024x512 RGB565": 1024 * 512 * 2,
        "640x480 RGB565": 640 * 480 * 2,
        "512x256 RGB565": 512 * 256 * 2,
    }
    print("per still bytes:", sizes)
    print("x8:", {k: v * 8 for k, v in sizes.items()})

    for n in range(1, 9):
        src_path = SRC_DIR / f"title_bg{n:03d}.img"
        src = load_img(src_path)
        src.save(OUT_DIR / f"bg{n:03d}_1024x512.png")

        now = to_rgb565(stretch(src, (640, 480), Image.Resampling.NEAREST))
        bake640 = to_rgb565(stretch(src, (640, 480), Image.Resampling.LANCZOS))
        small = stretch(src, (512, 256), Image.Resampling.LANCZOS)
        from512 = to_rgb565(stretch(small, (640, 480), Image.Resampling.NEAREST))
        small.save(OUT_DIR / f"bg{n:03d}_512x256.png")
        now.save(OUT_DIR / f"bg{n:03d}_now_640.png")
        bake640.save(OUT_DIR / f"bg{n:03d}_bake_640.png")
        from512.save(OUT_DIR / f"bg{n:03d}_from512_640.png")

        row = hstack(
            [
                label_bar(now, f"BG{n:03d} NOW nearest 1024 to 640 565"),
                label_bar(bake640, f"BG{n:03d} BAKE 640 Lanczos 565"),
                label_bar(from512, f"BG{n:03d} 512x256 then nearest 640 565"),
            ]
        )
        row.save(OUT_DIR / f"bg{n:03d}_compare.png")
        sheets.append(stretch(bake640, (320, 240), Image.Resampling.BOX))

        crops = hstack(
            [
                label_bar(crop4x(now, crop_box), "NOW 4x crop"),
                label_bar(crop4x(bake640, crop_box), "BAKE640 4x crop"),
                label_bar(crop4x(from512, crop_box), "FROM512 4x crop"),
            ]
        )
        crops.save(OUT_DIR / f"bg{n:03d}_crop4x.png")

        if n in (1, 4, 8):
            print(f"wrote bg{n:03d} {src.size}")

    contact = hstack(sheets[:4], 4)
    contact2 = hstack(sheets[4:], 4)
    Image.new("RGB", (contact.width, contact.height + contact2.height + 4), (12, 12, 16)).save(
        OUT_DIR / "contact_640_half.png"
    )
    c = Image.new("RGB", (contact.width, contact.height + contact2.height + 4), (12, 12, 16))
    c.paste(contact, (0, 0))
    c.paste(contact2, (0, contact.height + 4))
    c.save(OUT_DIR / "contact_640_half.png")
    print("out", OUT_DIR)


if __name__ == "__main__":
    main()
