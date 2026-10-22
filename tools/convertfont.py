#!/usr/bin/env python3
# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Bitmap font sheet (PNG or TIM) -> C source for tinrom's text renderer.

    python3 tools/convertfont.py font.png --cell 20x20 --size 10 -o font.c

The input is a grid of fixed-size cells in character order, the way a sprite
font is usually drawn: cell 0 is `--first` (a space, by default), then left to
right and top to bottom.

A PNG must be 8 bits per channel and not interlaced; a pixel less than half
opaque is transparent. A TIM can be any depth the GPU textures from, and
texel 0000h is transparent.

`--size` resamples every cell to that many pixels tall (and in proportion
wide, unless `--width` says otherwise) before anything else. tinrom's screen
is 640x240, so its pixels are half as wide as they are tall, and its font is
made 20 wide by 10 tall from a 20x20 source: each glyph keeps every column it
was drawn with and is averaged only down. Drawing a font larger than it will be shown and
scaling it down here is what lets one source serve any size: each output
pixel is the average of the source pixels it covers, so an edge that falls
between two output pixels comes out as a blend of the colours either side of
it. An output pixel less than half covered by the glyph is left transparent.
The PS1 cannot scale a sprite itself, so this is the only place it can
happen.

The output is what src/ui/font.c draws from:

  uiFontClut    the sheet's 16-entry palette, index 0 transparent
  uiFontGlyphs  where each glyph sits in the sheet, how wide it is, and how
                far the pen moves after it
  uiFontSheet   the glyphs at 4bpp, each cropped to its columns and packed
                into rows of a 256-texel sheet, padded so that it can be sent
                to VRAM in one DMA

A source with more than 15 colours is reduced to 15 by a k-means pass that is
seeded the same way every run, so the output does not change from build to
build. The text renderer tints glyphs by modulating them, so what matters is
the shading, not the exact colours.

Glyphs are proportional: each is cropped to the columns it uses, and the pen
moves on by that width plus `--tracking` (minus one by default, so that two
outlined glyphs share an outline column rather than doubling it). A glyph with
nothing in it - a space - moves the pen by `--space`. `--alias` borrows one
glyph for another that the sheet leaves empty.

The sizes are compared against src/ui/font.h at compile time; the output says
what to change there if a different font does not match.

Standard library only, like the rest of the build.
"""

import argparse
import os
import struct
import sys


def die(message):
    print(f"convertfont: error: {message}", file=sys.stderr)
    sys.exit(1)


# --- TIM ------------------------------------------------------------------- #

def read_tim(path):
    """Returns (width, height, pixels) with pixels a flat list of 16bpp
    values, whatever the TIM's own depth."""
    with open(path, "rb") as file:
        data = file.read()

    if len(data) < 8:
        die(f"{path} is too short to be a TIM")

    magic, flags = struct.unpack_from("<II", data, 0)

    if magic != 0x10:
        die(f"{path} is not a TIM (magic {magic:#x})")

    depth  = flags & 3
    offset = 8
    clut   = None

    if flags & 8:
        length, _x, _y, width, height = struct.unpack_from(
            "<IHHHH", data, offset
        )
        clut    = struct.unpack_from(f"<{width * height}H", data, offset + 12)
        offset += length

    length, _x, _y, words, height = struct.unpack_from("<IHHHH", data, offset)
    offset += 12

    raw = struct.unpack_from(f"<{words * height}H", data, offset)

    if depth == 2:
        return words, height, list(raw)

    if depth == 3:
        die(f"{path} is 24bpp, which the GPU cannot texture from")
    if clut is None:
        die(f"{path} is indexed but has no CLUT")

    pixels = []

    for word in raw:
        if depth == 0:
            pixels += [clut[(word >> shift) & 15] for shift in (0, 4, 8, 12)]
        else:
            pixels += [clut[word & 255], clut[word >> 8]]

    return words * (4 if depth == 0 else 2), height, pixels


# --- PNG ------------------------------------------------------------------- #

def read_png_rgba(path):
    """Returns (width, height, pixels) with pixels a flat list of (r,g,b,a).
    The decoder is mktim's, alongside this file."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

    from mktim import read_png  # noqa: E402

    return read_png(path)


def to16(rgb):
    """8-bit (r, g, b) -> an opaque 16bpp pixel, rounded as convertimage.py
    does. Black is 8000h, since 0000h is the transparent texel."""
    red, green, blue = (((c * 31) + 127) // 255 for c in rgb)
    value = red | (green << 5) | (blue << 10)

    return value if value else 0x8000


def weights(source, target):
    """For each of `target` pixels spanning `source` pixels, the
    [(source index, fraction of it covered)] that make it up."""
    scale = source / target
    out   = []

    for t in range(target):
        start, end = t * scale, (t + 1) * scale
        parts      = []

        for s in range(int(start), min(source, int(end) + 1)):
            cover = min(end, s + 1) - max(start, s)

            if cover > 1e-9:
                parts.append((s, cover))

        out.append(parts)

    return out


def resample(width, rgba, columns, rows, cell_w, cell_h, out_w, out_h,
             threshold):
    """Every cell scaled from cell_w x cell_h to out_w x out_h by averaging,
    each cell on its own so that no glyph bleeds into its neighbour. Returns
    the new sheet's (width, height, 16bpp pixels)."""
    across, down = weights(cell_w, out_w), weights(cell_h, out_h)
    sheet_w      = columns * out_w
    pixels       = [0] * (sheet_w * rows * out_h)

    for row in range(rows):
        for column in range(columns):
            cx, cy = column * cell_w, row * cell_h

            for ty, ys in enumerate(down):
                for tx, xs in enumerate(across):
                    area = cover = 0.0
                    total = [0.0, 0.0, 0.0]

                    for sy, wy in ys:
                        for sx, wx in xs:
                            r, g, b, a = rgba[(cy + sy) * width + cx + sx]
                            weight     = wx * wy
                            area      += weight

                            if a >= 128:
                                cover += weight
                                total[0] += r * weight
                                total[1] += g * weight
                                total[2] += b * weight

                    if cover < area * threshold:
                        continue

                    x = column * out_w + tx
                    y = row * out_h + ty
                    pixels[y * sheet_w + x] = to16(
                        round(c / cover) for c in total
                    )

    return sheet_w, rows * out_h, pixels


# --- colours --------------------------------------------------------------- #

def split(color):
    return (color & 31, (color >> 5) & 31, (color >> 10) & 31)


def join(rgb):
    red, green, blue = (max(0, min(31, round(c))) for c in rgb)

    return red | (green << 5) | (blue << 10)


def distance(a, b):
    return sum((x - y) * (x - y) for x, y in zip(a, b))


def reduce_colors(counts, limit):
    """{16bpp colour: pixel count} -> {colour: replacement}, at most `limit`
    distinct replacements. Deterministic: seeded with the most used colour,
    then repeatedly the one furthest from every seed so far."""
    colors = sorted(counts, key=lambda c: (-counts[c], c))

    if len(colors) <= limit:
        return {color: color for color in colors}

    points  = {color: split(color) for color in colors}
    centres = [points[colors[0]]]

    while len(centres) < limit:
        furthest = max(
            colors,
            key=lambda c: (min(distance(points[c], m) for m in centres), -c)
        )
        centres.append(points[furthest])

    for _ in range(50):
        groups = [[] for _ in centres]

        for color in colors:
            nearest = min(
                range(len(centres)),
                key=lambda i: (distance(points[color], centres[i]), i)
            )
            groups[nearest].append(color)

        moved = []

        for centre, group in zip(centres, groups):
            if not group:
                moved.append(centre)
                continue

            total = sum(counts[c] for c in group)
            moved.append(tuple(
                sum(points[c][axis] * counts[c] for c in group) / total
                for axis in range(3)
            ))

        if moved == centres:
            break

        centres = moved

    mapping = {}

    for color in colors:
        nearest = min(
            range(len(centres)),
            key=lambda i: (distance(points[color], centres[i]), i)
        )
        mapping[color] = join(centres[nearest])

    return mapping


def opaque(color):
    """0000h is the transparent texel, so an opaque black has to be spelled
    differently. 8000h is solid black with semi-transparency off."""
    return color if color else 0x8000


# --- output ---------------------------------------------------------------- #

def c_char(code):
    char = chr(code)

    if char == "\\":
        return "backslash"
    if char in "*/":
        return f"'{char}' "

    return f"'{char}'"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--cell", default="10x10",
        help="cell size in the source, WxH (default 10x10)")
    parser.add_argument("--size", type=int, default=0,
        help="resample cells to this many pixels tall (default: as drawn)")
    parser.add_argument("--width", type=int, default=0,
        help="resample cells to this many pixels wide (default: in "
             "proportion to --size)")
    parser.add_argument("--coverage", type=float, default=0.5,
        help="how much of a resampled pixel the glyph must cover for it to "
             "be drawn (default 0.5)")
    parser.add_argument("--first", type=int, default=32,
        help="character code of the first cell (default 32)")
    parser.add_argument("--count", type=int, default=95,
        help="glyphs to take (default 95: printable ASCII)")
    parser.add_argument("--space", type=int, default=4,
        help="pen advance for an empty glyph (default 4)")
    parser.add_argument("--tracking", type=int, default=-1,
        help="added to each glyph's width to give its advance (default -1)")
    parser.add_argument("--alias", action="append", default=[],
        metavar="TO=FROM",
        help="draw character TO with FROM's glyph; either may be a "
             "character or its code, e.g. 96=39 for ` as '")

    args = parser.parse_args()

    try:
        cell_w, cell_h = (int(v) for v in args.cell.lower().split("x"))
    except ValueError:
        parser.error("--cell is WxH, e.g. 10x10")

    with open(args.input, "rb") as file:
        is_png = file.read(8) == b"\x89PNG\r\n\x1a\n"

    if is_png:
        width, height, rgba = read_png_rgba(args.input)
    else:
        width, height, pixels = read_tim(args.input)

    columns = width // cell_w

    if not columns:
        die(f"the sheet is {width} wide, narrower than one {cell_w} cell")

    rows = (args.count + columns - 1) // columns

    if rows * cell_h > height:
        die(f"{args.count} glyphs need {rows} rows of cells, "
            f"the sheet has {height // cell_h}")

    if is_png or args.size or args.width:
        if not is_png:
            rgba = [
                (((p & 31) * 255) // 31, (((p >> 5) & 31) * 255) // 31,
                 (((p >> 10) & 31) * 255) // 31, 255 if p else 0)
                for p in pixels
            ]

        out_h = args.size or cell_h
        out_w = args.width or max(1, round(cell_w * out_h / cell_h))
        width, height, pixels = resample(
            width, rgba, columns, rows, cell_w, cell_h, out_w, out_h,
            args.coverage
        )
        cell_w, cell_h = out_w, out_h

    used_height = rows * cell_h

    def texel(x, y):
        return pixels[y * width + x]

    # Which rows any glyph uses: every glyph is drawn that tall.
    top, bottom = cell_h, -1

    for index in range(args.count):
        cx, cy = (index % columns) * cell_w, (index // columns) * cell_h

        for y in range(cell_h):
            if any(texel(cx + x, cy + y) for x in range(cell_w)):
                top, bottom = min(top, y), max(bottom, y)

    if bottom < 0:
        die("every glyph is empty")

    glyph_h = bottom - top + 1

    # Each glyph's columns.
    glyphs = []

    for index in range(args.count):
        cx, cy = (index % columns) * cell_w, (index // columns) * cell_h
        used = [
            x for x in range(cell_w)
            if any(texel(cx + x, cy + y) for y in range(top, bottom + 1))
        ]

        if used:
            left, glyph_w = used[0], used[-1] - used[0] + 1
            advance = max(1, glyph_w + args.tracking)
        else:
            left, glyph_w, advance = 0, 0, args.space

        glyphs.append([cx + left, cy + top, glyph_w, advance])

    for alias in args.alias:
        try:
            to, source = (
                (int(part) if part.isdigit() else ord(part)) - args.first
                for part in alias.split("=")
            )
        except (ValueError, TypeError):
            parser.error(f"--alias {alias!r} is not TO=FROM")

        if not (0 <= to < args.count and 0 <= source < args.count):
            parser.error(f"--alias {alias!r} names a character not in the font")

        glyphs[to] = list(glyphs[source])

    # Palette.
    counts = {}

    for y in range(used_height):
        for x in range(columns * cell_w):
            color = texel(x, y) & 0x7fff

            if texel(x, y):
                counts[color] = counts.get(color, 0) + 1

    mapping = reduce_colors(counts, 15)
    palette = sorted(
        set(mapping.values()),
        key=lambda c: (sum(split(c)), c)
    )
    index_of = {color: i + 1 for i, color in enumerate(palette)}

    # Sheet: the glyphs packed left to right in rows of their own height,
    # each cropped to its columns, in a sheet one texture page wide (a
    # sprite's u is eight bits). Then tall enough that the DMA is a whole
    # number of 16-word blocks.
    sheet_w = 256
    placed  = {}
    pen_x, pen_y = 0, 0

    for index, (sx, sy, glyph_w, advance) in enumerate(glyphs):
        if not glyph_w:
            glyphs[index] = [0, 0, 0, advance]
            continue

        key = (sx, sy, glyph_w)

        if key not in placed:
            if pen_x + glyph_w > sheet_w:
                pen_x, pen_y = 0, pen_y + glyph_h

            placed[key] = (pen_x, pen_y)
            pen_x += glyph_w

        u, v = placed[key]
        glyphs[index] = [u, v, glyph_w, advance]

    sheet_h = pen_y + glyph_h

    while (sheet_w * sheet_h) % 128:
        sheet_h += 1

    sheet = bytearray(sheet_w * sheet_h // 2)

    for (sx, sy, glyph_w), (u, v) in placed.items():
        for y in range(glyph_h):
            for x in range(glyph_w):
                value = texel(sx + x, sy + y)

                if not value:
                    continue

                nibble = index_of[mapping[value & 0x7fff]]
                offset = ((v + y) * sheet_w + u + x) // 2
                sheet[offset] |= nibble << (4 if (u + x) & 1 else 0)

    clut = [0] + [opaque(c) for c in palette]
    clut += [0] * (16 - len(clut))

    name = os.path.basename(args.input)
    out  = []

    out.append(f"/* Generated by tools/convertfont.py from {name}. Do not "
               "edit: change the\n * font or the converter, and rebuild. */\n")
    out.append("#include <stdint.h>\n#include \"ui/font.h\"\n")

    checks = [
        ("UI_FONT_FIRST",        args.first),
        ("UI_FONT_GLYPHS",       args.count),
        ("UI_FONT_HEIGHT",       glyph_h),
        ("UI_FONT_SHEET_WIDTH",  sheet_w),
        ("UI_FONT_SHEET_HEIGHT", sheet_h),
    ]

    for macro, value in checks:
        out.append(f"#if {macro} != {value}\n"
                   f"#error \"{name}: set {macro} to {value} in "
                   "src/ui/font.h\"\n#endif")

    out.append("")
    out.append("const uint16_t uiFontClut[16] __attribute__((aligned(4))) = {")

    for row in range(0, 16, 8):
        out.append("\t" + ", ".join(
            f"0x{c:04x}" for c in clut[row:row + 8]) + ",")

    out.append("};\n")
    out.append("const UIFontGlyph uiFontGlyphs[UI_FONT_GLYPHS] = {")

    for index, (u, v, glyph_w, advance) in enumerate(glyphs):
        out.append(f"\t{{ {u:3d}, {v:2d}, {glyph_w:2d}, {advance:2d} }}, "
                   f"/* {c_char(args.first + index)} */")

    out.append("};\n")
    out.append("const uint8_t uiFontSheet[UI_FONT_SHEET_BYTES] "
               "__attribute__((aligned(4))) = {")

    for row in range(0, len(sheet), 16):
        out.append("\t" + ", ".join(
            f"0x{b:02x}" for b in sheet[row:row + 16]) + ",")

    out.append("};")

    with open(args.output, "w", newline="\n") as file:
        file.write("\n".join(out) + "\n")

    print(f"convertfont: {name} -> {args.count} glyphs, {glyph_h}px tall, "
          f"{len(counts)} colours to {len(palette)}, "
          f"{sheet_w}x{sheet_h} sheet")


if __name__ == "__main__":
    main()
