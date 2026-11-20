"""Build the flash-resident LVGL glyph pack. Pass the directory of font inputs."""
import argparse
import re
import struct
import zipfile
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import ImageFont


ROOT = Path(__file__).resolve().parents[1]
SIZES = (13, 14, 16, 17, 18, 20, 34, 56, 40)


def common_han(inputs):
    with zipfile.ZipFile(inputs / "Unihan.zip") as archive:
        text = archive.read("Unihan_OtherMappings.txt").decode()
    rows = re.findall(r"^U\+([0-9A-F]+)\tkTGH\t2013:(\d+)$", text, re.M)
    codes = {int(cp, 16) for cp, rank in rows if int(rank) <= 3500}
    assert len(codes) == 3500
    return codes


def make_font(path, size, weight):
    font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    axes = font.get_variation_axes()
    font.set_variation_by_axes([weight if axis["name"] == b"Weight" else axis["default"] for axis in axes])
    return font


def pack(inputs):
    han = common_han(inputs)
    cmap = TTFont(inputs / "notosanssc.ttf").getBestCmap()
    assert not han.difference(cmap)
    han.update(c for c in range(0x3000, 0x3040) if c in cmap)
    han.update(c for c in range(0xff01, 0xff61) if c in cmap)
    han.update(map(ord, "□↑…“”‘’·—☀"))
    # Include any uncommon ideographs used by the reference UI and its fixtures.
    for filename in [ROOT.parent / "prototype/index.html", ROOT.parent / "prototype/api/v1/lockscreen.json", ROOT / "src/main.cpp"]:
        han.update(ord(c) for c in filename.read_text(encoding="utf8") if ord(c) >= 0x3000 and ord(c) in cmap)
    data = bytearray(len(SIZES) * 8)
    for face, size in enumerate(SIZES):
        latin = make_font(inputs / ("jetbrainsmono.ttf" if size == 56 else "inter.ttf"), size, 600 if size >= 34 else 500)
        chinese = make_font(inputs / "notosanssc.ttf", size, 650 if size == 13 else 500)
        codes = set(range(32, 127)) | {183, 8230, 9633}
        if size in (13, 14, 17, 18, 20):
            codes |= han
        if size >= 34:
            codes = set(map(ord, "0123456789:-"))
        if size == 40:
            codes.add(ord("%"))
        index = len(data)
        struct.pack_into("<II", data, face * 8, index, len(codes))
        data.extend(bytes(len(codes) * 12))
        for n, code in enumerate(sorted(codes)):
            font = chinese if code in han and code != 183 else latin
            char = chr(code)
            mask, (left, top) = font.getmask2(char, mode="L", anchor="ls")
            width, height = mask.size
            pixels = bytes(mask)
            if len(pixels) % 2:
                pixels += b"\0"
            offset = len(data)
            data.extend((a & 0xf0) | (b >> 4) for a, b in zip(pixels[::2], pixels[1::2]))
            struct.pack_into("<HIBBBbbx", data, index + 12*n, code, offset,
                             round(font.getlength(char)), width, height, left, -top-height)
        print(size, len(codes), len(data)-index, flush=True)
    (ROOT / "assets/fonts.bin").write_bytes(data)
    print("Total bytes", len(data))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", type=Path)
    pack(parser.parse_args().inputs)
