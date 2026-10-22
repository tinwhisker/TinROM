#!/usr/bin/env python3
"""Pack PNGs into a single 16bpp TIM for the PS1.

    python3 tools/mktim.py skin.png -o skin.tim
    python3 tools/mktim.py a.png b.png c.png -o atlas.tim --layout atlas.json

The PS1 binds one texture page at a time, so a model with several materials
needs them atlased into one image. With more than one input the images are
packed side by side and --layout writes a JSON map that mkmesh.py uses to
remap each material's UVs into its sub-rectangle.

Transparency follows the PS1 convention: a 16-bit pixel of 0x0000 is
transparent. Fully transparent source pixels become 0x0000, and any opaque
pixel that would round to black is nudged to the darkest opaque colour so it
does not turn into a hole.

PNG decoding is pure standard library -- no Pillow needed.
"""

import argparse
import json
import struct
import sys
import zlib


def die(msg):
    print("mktim: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def warn(msg):
    print("mktim: warning: %s" % msg, file=sys.stderr)


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """Returns (width, height, pixels) with pixels a flat list of (r,g,b,a)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        die("%s is not a PNG" % path)

    pos, idat, plte, trns = 8, [], None, None
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        ln, typ = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, depth, ctype, _comp, _filt, interlace = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break

    if interlace:
        die("%s is interlaced; re-save it without Adam7 interlacing" % path)
    if depth != 8:
        die("%s is %d-bit; only 8 bits per channel are supported" % (path, depth))

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        die("%s has an unsupported colour type (%d)" % (path, ctype))

    raw = zlib.decompress(b''.join(idat))
    stride = w * channels
    out, prev = [], bytearray(stride)
    p = 0
    for _y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                ul = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], ul)) & 0xff
        elif f != 0:
            die("%s uses unknown PNG filter %d" % (path, f))
        prev = line

        for x in range(w):
            o = x * channels
            if ctype == 0:
                g = line[o]; out.append((g, g, g, 255))
            elif ctype == 2:
                out.append((line[o], line[o + 1], line[o + 2], 255))
            elif ctype == 3:
                i = line[o]
                r, g, b = plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2]
                a = trns[i] if trns and i < len(trns) else 255
                out.append((r, g, b, a))
            elif ctype == 4:
                g = line[o]; out.append((g, g, g, line[o + 1]))
            else:
                out.append((line[o], line[o + 1], line[o + 2], line[o + 3]))
    return w, h, out


# ---------------------------------------------------------------------------
# TIM
# ---------------------------------------------------------------------------

TRANSPARENT = 0x0000
DARKEST_OPAQUE = 0x0421   # r=g=b=1 in 5:5:5 -- black without being a hole


def to15(r, g, b, a):
    if a < 128:
        return TRANSPARENT
    v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
    return DARKEST_OPAQUE if v == TRANSPARENT else v


def pack_atlas(images, max_size=256):
    """Shelf-pack (name, w, h, pixels) into one image. Returns (W, H, pixels, rects)."""
    order = sorted(images, key=lambda i: -i[2])      # tallest first
    shelves, placed = [], {}
    width = max(i[1] for i in images)
    width = min(max_size, max(width, 1))
    # grow the atlas width in powers of two until everything fits
    while width <= max_size:
        shelves, placed, ok = [], {}, True
        x = y = shelf_h = 0
        for name, w, h, _px in order:
            if w > width:
                ok = False; break
            if x + w > width:
                y += shelf_h; x = 0; shelf_h = 0
            if y + h > max_size:
                ok = False; break
            placed[name] = (x, y, w, h)
            x += w
            shelf_h = max(shelf_h, h)
        if ok:
            height = y + shelf_h
            break
        width *= 2
    else:
        die("images do not fit in a %dx%d texture page" % (max_size, max_size))

    # round up to keep the GPU happy and make the numbers readable
    def up2(n):
        p = 1
        while p < n:
            p *= 2
        return p
    W, H = up2(width), up2(height)
    if W > max_size or H > max_size:
        die("atlas would be %dx%d, larger than one %dx%d texture page" % (W, H, max_size, max_size))

    buf = [TRANSPARENT] * (W * H)
    for name, w, h, px in images:
        ox, oy, _, _ = placed[name]
        for yy in range(h):
            row = yy * w
            dst = (oy + yy) * W + ox
            for xx in range(w):
                buf[dst + xx] = to15(*px[row + xx])
    return W, H, buf, placed


def write_tim(path, W, H, pixels, vram=(0, 0)):
    body = struct.pack('<HHHH', vram[0], vram[1], W, H)
    body += b''.join(struct.pack('<H', p) for p in pixels)
    blob = struct.pack('<II', 0x10, 2)                 # magic, 16bpp no CLUT
    blob += struct.pack('<I', len(body) + 4) + body    # block length includes itself
    open(path, 'wb').write(blob)
    return len(blob)


def main():
    ap = argparse.ArgumentParser(description="Pack PNGs into a 16bpp PS1 TIM.")
    ap.add_argument('png', nargs='+')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--layout', help="write a JSON map of image name -> [x,y,w,h] in the atlas")
    ap.add_argument('--max', type=int, default=256, help="largest atlas edge (default 256)")
    args = ap.parse_args()

    images = []
    for p in args.png:
        w, h, px = read_png(p)
        images.append((p, w, h, px))
        print("mktim: %-16s %dx%d" % (p, w, h))

    W, H, buf, rects = pack_atlas(images, args.max)
    size = write_tim(args.output, W, H, buf)
    print("mktim: -> %s  %dx%d 16bpp, %d bytes" % (args.output, W, H, size))
    for name, (x, y, w, h) in sorted(rects.items()):
        print("         %-16s at (%d,%d) %dx%d" % (name, x, y, w, h))

    if args.layout:
        json.dump({'texture': [W, H],
                   'rects': {k: list(v) for k, v in rects.items()}},
                  open(args.layout, 'w'), indent=2)
        print("mktim: layout -> %s" % args.layout)


if __name__ == '__main__':
    main()
