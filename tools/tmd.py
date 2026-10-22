"""Read Sony TMD models (the Psy-Q native PlayStation format).

Returns the same shape as mkmesh's OBJ loader, so the rest of the pipeline
does not care which one produced the data. Each TMD "object" becomes a group
named obj0, obj1, ... which the JSON sidecar can claim as a part.

On trusting this parser
-----------------------
TMD primitive packets are variable-length and their field layout depends on
the mode and flag bytes. The layout is derived from those bits and then
checked against the packet's own declared ilen: if no layout of the right
size exists, the primitive is skipped and its mode byte reported rather than
decoded into scrambled geometry. Padding does make some layouts the same
size, so preference order decides between them and ilen only confirms.

This has been round-tripped against synthesised files but not, so far,
against a TMD produced by Sony's own tools -- so treat a high skip count as
a layout gap here rather than a broken model, and send the report along.

Run with --report to see a breakdown of every mode byte in a file, including
the ones that were skipped.
"""

import struct
from collections import Counter

CODE_POLYGON = 1

# mode byte: CCC I Q T A G
#   CCC code, I gouraud, Q quad, T textured, A semi-transparent, G brightness
MODE_CODE = 0xE0
MODE_IIP = 0x10   # gouraud
MODE_QUAD = 0x08
MODE_TME = 0x04   # textured
MODE_ABE = 0x02
MODE_TGE = 0x01

FLAG_LGT = 0x01   # 1 = no light source calculation


class TmdError(Exception):
    pass


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _pad4(n):
    return (n + 3) & ~3


def _layout_options(nvert, textured, gouraud, lit):
    """Plausible body layouts, most likely first.

    The building blocks are fixed: a textured corner costs 4 bytes (u, v, then
    cba / tsb / padding), an index costs 2, a colour costs 4. What varies is
    how many normal indices are present, and padding makes several of those
    the same size -- so preference order decides, and ilen only confirms.
    """
    colour_bytes = 0 if textured else (4 * (nvert if gouraud else 1))
    uv_bytes = nvert * 4 if textured else 0
    idx_off = colour_bytes + uv_bytes

    # Deliberately narrow. Offering a per-vertex-normal layout for a flat
    # polygon would make almost any body size match something, which defeats
    # the point of checking against ilen. An unlisted layout gets skipped and
    # reported, which is recoverable; a wrongly accepted one is not.
    if gouraud:
        order = [nvert, 0]        # gouraud means a normal per vertex
    elif lit:
        order = [1, 0]            # flat and lit means a single face normal
    else:
        order = [0, 1]

    seen, opts = set(), []
    for normals in order:
        if normals in seen:
            continue
        seen.add(normals)
        # with one normal per vertex the two interleave: n0 v0 n1 v1 ...
        idx_bytes = nvert * 4 if normals == nvert and nvert > 1 else normals * 2 + nvert * 2
        opts.append((_pad4(idx_off + idx_bytes), normals, colour_bytes, idx_off))
    return opts


def _read_polygon(data, off, ilen, flag, mode, verts_base, positions_len):
    """Decode one polygon packet body. Returns (indices, uvs) or None."""
    nvert = 4 if (mode & MODE_QUAD) else 3
    textured = bool(mode & MODE_TME)
    gouraud = bool(mode & MODE_IIP)
    body_len = ilen * 4

    lit = not (flag & FLAG_LGT)
    match = None
    for size, normals, uv_off, idx_off in _layout_options(nvert, textured, gouraud, lit):
        if size == body_len:
            match = (normals, uv_off, idx_off)
            break
    if match is None:
        return None              # no layout of the right size: refuse to guess
    normals, uv_off, idx_off = match

    uvs = []
    if textured:
        for i in range(nvert):
            u, v = struct.unpack_from('<BB', data, off + uv_off + i * 4)
            uvs.append((u, v))
    else:
        uvs = [(0, 0)] * nvert

    idx = []
    p = off + idx_off
    if normals == nvert:
        for _ in range(nvert):
            p += 2                                   # skip the normal index
            idx.append(struct.unpack_from('<H', data, p)[0])
            p += 2
    else:
        p += normals * 2
        for _ in range(nvert):
            idx.append(struct.unpack_from('<H', data, p)[0])
            p += 2

    for v in idx:
        if verts_base + v >= positions_len:
            return None
    return [verts_base + v for v in idx], uvs


def load_tmd(path, report=False):
    """Returns (positions, uvs, groups, stats).

    positions are int16 model units straight from the file, uvs are texels
    (0..255), and groups maps 'objN' -> list of faces, each face a list of
    (position_index, uv_index).
    """
    data = open(path, 'rb').read()
    if len(data) < 12:
        raise TmdError("%s is too short to be a TMD" % path)

    version, flags, nobj = struct.unpack_from('<III', data, 0)
    if version != 0x41:
        raise TmdError("%s: version 0x%x, expected 0x41 -- is this really a TMD?" % (path, version))

    # With flags bit 0 clear the offsets are relative to the end of the header;
    # with it set they are already absolute file offsets.
    relative = (flags & 1) == 0
    base = 12 + nobj * 28 if relative else 0

    positions, all_uvs, groups = [], [], {}
    seen, skipped = Counter(), Counter()

    for o in range(nobj):
        vtop, vn, ntop, nn, ptop, pn, scale = struct.unpack_from('<IIIIIIi', data, 12 + o * 28)
        vstart = base + vtop if relative else vtop
        pstart = base + ptop if relative else ptop

        if vstart + vn * 8 > len(data):
            raise TmdError("%s: object %d vertex data runs past the end of the file" % (path, o))

        verts_base = len(positions)
        for i in range(vn):
            x, y, z = struct.unpack_from('<hhh', data, vstart + i * 8)
            positions.append((x, y, z))

        name = 'obj%d' % o
        faces = []
        p = pstart
        for _ in range(pn):
            if p + 4 > len(data):
                raise TmdError("%s: object %d primitive list runs past the end of the file" % (path, o))
            olen, ilen, flag, mode = struct.unpack_from('<BBBB', data, p)
            body = p + 4
            if body + ilen * 4 > len(data):
                raise TmdError("%s: object %d has a primitive claiming %d words at offset %d"
                               % (path, o, ilen, body))
            code = (mode & MODE_CODE) >> 5
            seen[(code, mode, ilen)] += 1

            if code == CODE_POLYGON:
                got = _read_polygon(data, body, ilen, flag, mode, verts_base, len(positions))
                if got is None:
                    skipped[(mode, ilen)] += 1
                else:
                    idx, uvs = got
                    corners = []
                    for k in range(len(idx)):
                        corners.append((idx[k], len(all_uvs)))
                        all_uvs.append(uvs[k])
                    # TMD quads are already in the GPU's Z order, unlike OBJ's
                    # perimeter winding, so they pass straight through.
                    faces.append(corners)
            else:
                skipped[(mode, ilen)] += 1

            p = body + ilen * 4      # step to the next packet

        if faces:
            groups[name] = faces

    stats = {'objects': nobj, 'seen': seen, 'skipped': skipped,
             'faces': sum(len(f) for f in groups.values()), 'verts': len(positions)}
    if report:
        print("tmd: %s -- %d objects, %d vertices" % (path, nobj, len(positions)))
        print("tmd: primitives by (code, mode, ilen):")
        for (code, mode, ilen), n in sorted(seen.items()):
            mark = ' SKIPPED' if skipped.get((mode, ilen)) else ''
            bits = []
            if mode & MODE_IIP: bits.append('gouraud')
            if mode & MODE_QUAD: bits.append('quad')
            else: bits.append('tri')
            if mode & MODE_TME: bits.append('textured')
            if mode & MODE_ABE: bits.append('trans')
            if mode & MODE_TGE: bits.append('nolight')
            print("     code=%d mode=0x%02x ilen=%-2d n=%-5d %-34s%s"
                  % (code, mode, ilen, n, ','.join(bits), mark))
    return positions, all_uvs, groups, stats
