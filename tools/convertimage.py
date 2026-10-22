#!/usr/bin/env python3
# tinrom - (C) 2026 TinWhisker - MIT licensed
"""PNG -> raw PS1 texture (+ palette), using only the standard library.

ps1-bare-metal ships tools/convertImage.py, which does the same job but needs
NumPy and Pillow. Those have no wheels for an MSYS2 Python, so on a devkitPro
or MSYS2 setup the build stops dead at asset conversion. tinrom converts one
small texture; that is not worth a scientific computing stack.

The conversion semantics are deliberately identical to upstream's so the
output is interchangeable - see NOTICE.md, this is a derivative of
ps1-bare-metal's convertImage.py (MIT, spicyjpeg).

PNG decoding is delegated to read_png() in mktim.py, alongside this file.

    python3 tools/convertimage.py -b 8 rom.png romTexture.dat romPalette.dat
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mktim import read_png  # noqa: E402

# A 16-bit pixel of 0x0000 is transparent to the GPU, so an opaque pixel that
# would round to black has to be nudged somewhere else or it becomes a hole.
TRANSPARENT_COLOR = 0x0000
BLACK_COLOR = 0x0421
STP_BLACK_COLOR = 0x8000

LOWER_ALPHA_BOUND = 32
UPPER_ALPHA_BOUND = 224


def to16bpp(rgba, force_stp=False, stp_black=False):
    """One (r,g,b,a) tuple -> one 16bpp GPU pixel."""
    r, g, b, a = rgba

    red = ((r * 31) + 127) // 255
    green = ((g * 31) + 127) // 255
    blue = ((b * 31) + 127) // 255

    solid = red | (green << 5) | (blue << 10)

    if a < LOWER_ALPHA_BOUND:
        return TRANSPARENT_COLOR
    if force_stp or a < UPPER_ALPHA_BOUND:
        return solid | (1 << 15)
    if solid == TRANSPARENT_COLOR:
        return STP_BLACK_COLOR if stp_black else BLACK_COLOR

    return solid


def quantize(pixels, max_colors):
    """Unique colours, ordered the same way upstream's np.unique orders them
    (lexicographically by r, g, b, a), plus the indexed image."""
    palette = sorted(set(pixels))

    if len(palette) > max_colors:
        raise ValueError(
            f"image has {len(palette)} unique colours, but {max_colors} is the "
            f"most a {max_colors.bit_length() - 1}bpp image can hold"
        )

    lookup = {color: index for index, color in enumerate(palette)}

    return palette, [lookup[pixel] for pixel in pixels]


def pack_indexed(indices, width, height, bpp):
    out = bytearray()

    for y in range(height):
        row = indices[y * width:(y + 1) * width]

        if bpp == 4:
            if len(row) % 2:
                row = row + [0]
            packed = bytes(
                (row[x] & 0x0f) | ((row[x + 1] & 0x0f) << 4)
                for x in range(0, len(row), 2)
            )
        else:
            packed = bytes(index & 0xff for index in row)

        # Rows are padded to an even byte count: VRAM is addressed in 16-bit
        # words, so an odd-width row would put the next row out of step.
        if len(packed) % 2:
            packed += b"\x00"

        out += packed

    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-b", "--bpp", type=int, choices=(4, 8, 16), default=16)
    parser.add_argument("-s", "--force-stp", action="store_true")
    parser.add_argument("-S", "--stp-black", action="store_true")
    parser.add_argument("input")
    parser.add_argument("imageOutput")
    parser.add_argument("clutOutput", nargs="?")

    args = parser.parse_args()

    width, height, pixels = read_png(args.input)

    if args.bpp == 16:
        image = b"".join(
            struct.pack("<H", to16bpp(pixel, args.force_stp))
            for pixel in pixels
        )
        clut = None
    else:
        if args.clutOutput is None:
            parser.error("a palette output path is required below 16bpp")

        try:
            palette, indices = quantize(pixels, 1 << args.bpp)
        except ValueError as error:
            parser.error(str(error))

        entries = [
            to16bpp(color, args.force_stp, args.stp_black) for color in palette
        ]
        # Pad to a full CLUT: the GPU always reads 16 or 256 entries.
        entries += [0] * ((16 if args.bpp == 4 else 256) - len(entries))

        clut = b"".join(struct.pack("<H", entry) for entry in entries)
        image = pack_indexed(indices, width, height, args.bpp)

    with open(args.imageOutput, "wb") as file:
        file.write(image)

    if clut is not None:
        with open(args.clutOutput, "wb") as file:
            file.write(clut)

    print(
        f"convertimage: {args.input} -> {width}x{height} @ {args.bpp}bpp, "
        f"{len(image)} bytes"
        + (f" + {len(clut)} byte palette" if clut else "")
    )


if __name__ == "__main__":
    main()
