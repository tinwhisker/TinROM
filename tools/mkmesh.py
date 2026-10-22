#!/usr/bin/env python3
"""Build a PS1M mesh from an OBJ plus a JSON sidecar.

    python3 tools/mkmesh.py model.obj model.json -o assets/model.msh

The OBJ supplies geometry and UVs. The sidecar supplies everything OBJ cannot
express -- the part hierarchy, animations and collision boxes:

    {
      "texture": [128, 128],
      "scale": 64,
      "parts": [
        {"name": "torso", "groups": ["torso"], "pivot": [0, 0, 0]},
        {"name": "head",  "groups": ["head"], "parent": "torso", "pivot": [0, -20, 0]}
      ],
      "boxes": [
        {"name": "head", "part": "head", "min": [-8,-8,-8], "max": [8,8,8]}
      ],
      "animations": {
        "walk": {"fps": 15, "loop": true, "frames": [
          {"head": {"rot": [0, 0, 128]}},
          {"head": {"rot": [0, 0, -128]}}
        ]}
      }
    }

"groups" names OBJ `o`/`g` groups that belong to the part. Rotations are in
angle units where 4096 is a full turn; translations are model units. Anything
a frame leaves out stays at rest.

See ps1port/mesh.h for the binary layout.
"""

import argparse
import json
import os
import struct
import sys

try:
    from tmd import load_tmd, TmdError
    from bbmodel import load_bbmodel, BbError
except ImportError:  # running from another directory
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from tmd import load_tmd, TmdError
    from bbmodel import load_bbmodel, BbError

MAGIC = b'PS1M'
VERSION = 1
NAME_LEN = 12
BOX_NAME_LEN = 14
TRIANGLE = 0xFFFF
ANGLE_TURN = 4096


def die(msg):
    print("mkmesh: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def warn(msg):
    print("mkmesh: warning: %s" % msg, file=sys.stderr)


def load_obj(path):
    """Returns (positions, uvs, groups) where groups maps name -> list of faces,
    each face a list of (position_index, uv_index)."""
    positions, uvs = [], []
    groups, current = {}, '__default__'
    groups[current] = []

    with open(path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            bits = line.split()
            tag = bits[0]

            if tag == 'v':
                positions.append(tuple(float(v) for v in bits[1:4]))
            elif tag == 'vt':
                uvs.append(tuple(float(v) for v in bits[1:3]))
            elif tag in ('o', 'g'):
                current = bits[1] if len(bits) > 1 else '__default__'
                groups.setdefault(current, [])
            elif tag == 'f':
                corners = []
                for token in bits[1:]:
                    parts = token.split('/')
                    vi = int(parts[0])
                    ti = int(parts[1]) if len(parts) > 1 and parts[1] else 0
                    # OBJ indices are 1-based, and negative means relative
                    vi = vi - 1 if vi > 0 else len(positions) + vi
                    ti = ti - 1 if ti > 0 else (len(uvs) + ti if ti < 0 else -1)
                    corners.append((vi, ti))
                if len(corners) < 3:
                    die("%s:%d: face with %d corners" % (path, lineno, len(corners)))
                # Fan-triangulate anything with more than four corners. Quads
                # are kept whole: the GPU draws them natively and it halves the
                # primitive count.
                if len(corners) == 4:
                    groups[current].append(corners)
                else:
                    for i in range(1, len(corners) - 1):
                        groups[current].append([corners[0], corners[i], corners[i + 1]])

    groups = {k: v for k, v in groups.items() if v}
    return positions, uvs, groups


def to_texel(uv, tex_w, tex_h):
    """OBJ UVs are 0..1 with v pointing up; the GPU wants texels with v down."""
    u = int(round(uv[0] * (tex_w - 1)))
    v = int(round((1.0 - uv[1]) * (tex_h - 1)))
    return max(0, min(255, u)), max(0, min(255, v))


def fix_winding(out_faces, out_verts, verbose=False):
    """Make every face wind the same way round as its neighbours.

    Backface culling asks one question of each face -- does it turn towards the
    camera -- and answers it from the order of its corners. A single face wound
    the other way is invisible from the side you want to see it from, which
    looks like a hole in the model rather than like a winding problem, so it is
    easy to chase for a long time.

    Exporters produce these routinely: mirrored geometry, a negatively-scaled
    object, a face built by hand. Blender's "Recalculate Outside" fixes it, but
    only if you know to look.

    The repair walks the mesh through its shared edges. Two faces agree if they
    traverse their shared edge in opposite directions -- go round one clockwise
    and the other anticlockwise and the edge between them runs each way once.
    Where they disagree, the neighbour is flipped. That propagates one
    consistent orientation across each connected piece.

    Which orientation that is, is then decided per piece by signed volume: a
    correctly wound closed surface encloses a positive one. An open piece gives
    a less reliable answer, hence the boundary-edge count in the report.

    Returns (faces, flipped, boundary_edges, pieces).
    """
    n = len(out_faces)
    if n == 0:
        return out_faces, 0, 0, 0

    # Weld by position for adjacency only. Two vertex records at the same point
    # are the same corner as far as the surface is concerned, even though the
    # output keeps them separate.
    weld = {}
    wid = []
    for v in out_verts:
        wid.append(weld.setdefault(v, len(weld)))

    def perimeter(face):
        idx, tex = face
        if idx[3] == TRIANGLE:
            return [0, 1, 2]
        return [0, 1, 3, 2]     # stored Z order -> round the outside

    def corners(fi):
        idx, tex = out_faces[fi]
        return [wid[idx[k]] for k in perimeter(out_faces[fi])]

    # edge -> faces touching it
    edges = {}
    for fi in range(n):
        c = corners(fi)
        for k in range(len(c)):
            a, b = c[k], c[(k + 1) % len(c)]
            edges.setdefault((min(a, b), max(a, b)), []).append(fi)

    boundary = sum(1 for f in edges.values() if len(f) == 1)
    nonmanifold = sum(1 for f in edges.values() if len(f) > 2)


    # Only propagate across edges with exactly two faces. Welding by position
    # can make separate shells that merely touch look joined -- two boxes
    # stacked face to face share an edge between four faces, and their
    # coplanar inner faces genuinely do wind opposite ways. Flipping one to
    # "agree" would turn a correct model inside out. An edge like that carries
    # no orientation information, so it is skipped.
    adjacent = {e: f for e, f in edges.items() if len(f) == 2}

    def traverses(fi, a, b):
        """True if face fi walks its perimeter from a to b (not b to a)."""
        c = corners(fi)
        for k in range(len(c)):
            if c[k] == a and c[(k + 1) % len(c)] == b:
                return True
        return False

    def flip(fi):
        idx, tex = out_faces[fi]
        order = perimeter(out_faces[fi])
        ridx = [idx[k] for k in order][::-1]
        rtex = [tex[k] for k in order][::-1]
        if len(ridx) == 3:
            out_faces[fi] = (list(ridx) + [TRIANGLE], list(rtex) + [(0, 0)])
        else:
            # back from perimeter to Z order is the same swap
            out_faces[fi] = ([ridx[0], ridx[1], ridx[3], ridx[2]],
                             [rtex[0], rtex[1], rtex[3], rtex[2]])

    seen = [False] * n
    flipped = 0
    pieces = 0
    for start in range(n):
        if seen[start]:
            continue
        pieces += 1
        seen[start] = True
        group = [start]
        seen_group = {start}
        stack = [start]
        while stack:
            fi = stack.pop()
            c = corners(fi)
            for k in range(len(c)):
                a, b = c[k], c[(k + 1) % len(c)]
                for fj in adjacent.get((min(a, b), max(a, b)), ()):
                    if fj == fi or seen[fj]:
                        continue
                    # agreeing neighbours traverse the shared edge the other way
                    if traverses(fj, a, b):
                        flip(fj)
                        flipped += 1
                    seen[fj] = True
                    group.append(fj)
                    seen_group.add(fj)
                    stack.append(fj)

        # Now the piece is internally consistent. Whether it points outwards is
        # a separate question, and signed volume can only answer it for a
        # closed surface -- an open shell, or a single face stranded by
        # non-manifold junctions, gives a number with no meaning. Where the
        # test does not apply, keep whatever orientation the model file had.
        touched = set()
        for fi in group:
            c = corners(fi)
            for k in range(len(c)):
                a, b = c[k], c[(k + 1) % len(c)]
                touched.add((min(a, b), max(a, b)))
        closed = all(len(edges[e]) == 2 and all(f in seen_group for f in edges[e])
                     for e in touched)
        if not closed:
            if verbose:
                print("mkmesh:   piece of %d faces is not a closed surface; its facing was "
                      "left as the file had it" % len(group))
            continue

        vol = 0.0
        for fi in group:
            idx, _tex = out_faces[fi]
            order = perimeter(out_faces[fi])
            p = [out_verts[idx[k]] for k in order]
            for t in range(1, len(p) - 1):
                a, b, c3 = p[0], p[t], p[t + 1]
                vol += (a[0] * (b[1] * c3[2] - b[2] * c3[1])
                        - a[1] * (b[0] * c3[2] - b[2] * c3[0])
                        + a[2] * (b[0] * c3[1] - b[1] * c3[0]))
        if vol < 0:
            for fi in group:
                flip(fi)
            flipped += len(group)
            if verbose:
                print("mkmesh:   piece of %d faces was inside-out, flipped whole" % len(group))

    return out_faces, flipped, boundary, pieces, nonmanifold


def find_overlaps(out_faces, out_verts, part_of_vert):
    """Faces sitting exactly on top of another face, split by what that means.

    Within a single part, two coincident faces are pure waste: they can never
    move apart, so one is always hidden behind the other. Worth reporting.

    Across two parts, they are the opposite of waste. A limb modelled as its
    own capped tube meets its neighbour at coincident rings, and each cap is
    what closes its own segment once the joint bends -- delete them and the
    model opens into a hollow pipe the moment it animates. So these are
    expected, and saying anything alarming about them would send you deleting
    the geometry that holds the model together.

    Returns (same_part, cross_part).
    """
    groups = {}
    for fi, (idx, _tex) in enumerate(out_faces):
        key = frozenset(out_verts[i] for i in idx if i != TRIANGLE)
        groups.setdefault(key, []).append(fi)

    same = cross = 0
    for fis in groups.values():
        if len(fis) < 2:
            continue
        parts = set()
        for fi in fis:
            for i in out_faces[fi][0]:
                if i != TRIANGLE:
                    parts.add(part_of_vert[i])
        if len(parts) == 1:
            same += len(fis) - 1
        else:
            cross += len(fis) - 1
    return same, cross


def build(obj_path, cfg_path, out_path, verbose=False, scale=None, texture=None, axis=None,
          fix_wind=True, fps=15):
    # The sidecar is optional. Without one you get a single rigid part holding
    # the whole model, no animations and no collision boxes -- which is all a
    # static prop needs.
    cfg = json.load(open(cfg_path)) if cfg_path else {}
    tex_w, tex_h = texture or cfg.get('texture', [256, 256])
    is_tmd = obj_path.lower().endswith('.tmd')
    is_bb = obj_path.lower().endswith('.bbmodel')

    # Axis convention.
    #
    # The engine's world is the PlayStation's: +x right, +y DOWN, +z away from
    # the camera. TMDs are authored in exactly that space and pass through.
    # OBJ is +y up and +z towards the viewer, so it gets rotated 180 degrees
    # about x -- negate y and z. That is a rigid rotation, so it leaves winding
    # and handedness alone; the model simply ends up the right way up with its
    # front facing the camera.
    #
    # The same rotation has to reach anything else authored in OBJ space: part
    # pivots, collision boxes and keyframe translations from the sidecar.
    if axis is None:
        axis = cfg.get('axis', 'tmd' if is_tmd else 'obj')
    if axis not in ('obj', 'raw', 'tmd'):
        die("axis must be 'obj' or 'raw', not '%s'" % axis)
    flip = (axis == 'obj')

    def conv(v):
        return (v[0], -v[1], -v[2]) if flip else tuple(v)

    if is_bb:
        # Blockbench brings its own hierarchy, weights and animations, so it
        # skips the group/sidecar machinery entirely and assembles the arrays
        # directly. The one thing it must respect is the engine's rule that a
        # part owns a contiguous run of vertices -- bbmodel.py orders them that
        # way -- while faces stay free to reference vertices in other parts, so
        # a polygon across a joint stretches instead of tearing.
        scale = scale if scale is not None else cfg.get('scale', 4)
        try:
            bb = load_bbmodel(obj_path, fps=fps, report=verbose)
        except BbError as e:
            die(str(e))
        st = bb['stats']
        if texture is None and 'texture' not in cfg:
            tex_w, tex_h = bb['texture']

        names = [p['name'] for p in bb['parts']]
        index_of = {n: i for i, n in enumerate(names)}
        part_of_vert = bb['vert_part']
        out_verts = [tuple(int(round(c * scale)) for c in conv(v)) for v in bb['verts']]

        out_faces = []
        for corners in bb['faces']:
            idx = [c[0] for c in corners]
            texels = [(max(0, min(255, int(round(u)))), max(0, min(255, int(round(t)))))
                      for _i, (u, t) in corners]
            if len(idx) == 3:
                idx.append(TRIANGLE)
                texels.append((0, 0))
            else:
                # perimeter order to the GPU's Z order
                idx[2], idx[3] = idx[3], idx[2]
                texels[2], texels[3] = texels[3], texels[2]
            out_faces.append((idx, texels))

        # Faces belong to the part of their first corner, purely so each part
        # can name a contiguous range; the renderer walks every face anyway.
        out_faces.sort(key=lambda f: part_of_vert[f[0][0]])

        vcount = [0] * len(names)
        for pi in part_of_vert:
            vcount[pi] += 1
        fcount = [0] * len(names)
        for idx, _t in out_faces:
            fcount[part_of_vert[idx[0]]] += 1
        out_parts, vacc, facc = [], 0, 0
        for i, p in enumerate(bb['parts']):
            out_parts.append({
                'parent': -1 if p['parent'] is None else p['parent'],
                'pivot': [int(round(c * scale)) for c in conv(p['pivot'])],
                'firstVert': vacc, 'vertCount': vcount[i],
                'firstFace': facc, 'faceCount': fcount[i],
            })
            vacc += vcount[i]
            facc += fcount[i]

        if st['unbound']:
            warn("%d of %d vertices are not weighted to any bone; they were bound to '%s' "
                 "and will not move with the rig" % (st['unbound'], len(out_verts), st['root']))
        if st['worst_blend'] > 0.25:
            warn("the most blended vertex gives %.0f%% of its weight to bones other than the "
                 "one it was bound to. Rigid binding will show at that joint."
                 % (100 * st['worst_blend']))
        if st['skipped_faces']:
            warn("%d faces were skipped: Blockbench keeps loose edges and points in the mesh, "
                 "and n-gons above four corners are not supported" % st['skipped_faces'])
        if st['molang']:
            warn("%d keyframe values are MoLang expressions rather than numbers; they were "
                 "read as zero" % st['molang'])
    else:
        if is_tmd:
            # TMD coordinates are already int16 model units and its UVs are
            # already texels, so neither gets scaled or remapped by default.
            scale = scale if scale is not None else cfg.get('scale', 1)
            try:
                positions, uvs, groups, stats = load_tmd(obj_path, report=verbose)
            except TmdError as e:
                die(str(e))
            total_skipped = sum(stats['skipped'].values())
            if total_skipped:
                warn("%d of %d primitives were skipped because their packet layout did not match "
                     "any known one -- re-run with -v to see the mode bytes, and send them on so "
                     "the layout can be added" % (total_skipped, sum(stats['seen'].values())))
            if not groups:
                die("%s produced no usable faces" % obj_path)
        else:
            scale = scale if scale is not None else cfg.get('scale', 64)
            positions, uvs, groups = load_obj(obj_path)
            positions = [conv(p) for p in positions]

        lo = [min(p[i] for p in positions) for i in range(3)] if positions else [0, 0, 0]
        hi = [max(p[i] for p in positions) for i in range(3)] if positions else [0, 0, 0]
        span = max(hi[i] - lo[i] for i in range(3))
        if verbose:
            print("mkmesh: model bounds %s .. %s (largest span %.3f)"
                  % (["%.3f" % v for v in lo], ["%.3f" % v for v in hi], span))
        if span > 0:
            fits = int(32767 / max(abs(v) for v in lo + hi)) if any(lo + hi) else 0
            if fits and scale > fits:
                warn("scale %d overflows int16 for this model; the largest that fits is %d" % (scale, fits))
            elif span * scale < 16:
                warn("at scale %d the whole model spans %d units -- it will be a few pixels across"
                     % (scale, int(span * scale)))
        if not uvs:
            warn("%s has no texture coordinates, so every face will sample texel (0,0) and the "
                 "model will render as one flat colour. Re-export from Blender with 'Include UVs' "
                 "after unwrapping." % obj_path)

        if not positions:
            die("%s has no vertices" % obj_path)
        if tex_w > 256 or tex_h > 256:
            die("texture %dx%d exceeds one 256x256 texture page" % (tex_w, tex_h))

        part_defs = cfg.get('parts')
        if not part_defs:
            # No hierarchy declared: one rigid part holding everything.
            part_defs = [{'name': 'root', 'groups': list(groups)}]

        names = [p['name'] for p in part_defs]
        if len(set(names)) != len(names):
            die("duplicate part names")
        index_of = {n: i for i, n in enumerate(names)}

        for p in part_defs:
            parent = p.get('parent')
            if parent is not None:
                if parent not in index_of:
                    die("part '%s' has unknown parent '%s'" % (p['name'], parent))
                if index_of[parent] >= index_of[p['name']]:
                    die("part '%s' must be listed after its parent '%s' -- the engine "
                        "composes the hierarchy in one forward pass" % (p['name'], parent))

        claimed = set()
        for p in part_defs:
            for g in p.get('groups', []):
                if g not in groups:
                    die("part '%s' claims OBJ group '%s', which is not in %s (have: %s)"
                        % (p['name'], g, obj_path, ', '.join(sorted(groups)) or 'none'))
                if g in claimed:
                    die("OBJ group '%s' is claimed by more than one part" % g)
                claimed.add(g)
        for g in groups:
            if g not in claimed:
                warn("OBJ group '%s' is not claimed by any part, its faces are dropped" % g)

        # Vertices are de-duplicated per part, because a part's vertex range has to
        # be contiguous and a shared vertex may be pulled by two different parts.
        out_parts, out_verts, out_faces = [], [], []
        for p in part_defs:
            first_vert, first_face = len(out_verts), len(out_faces)
            remap = {}
            for g in p.get('groups', []):
                for corners in groups[g]:
                    idx, texels = [], []
                    for vi, ti in corners:
                        if vi < 0 or vi >= len(positions):
                            die("face references vertex %d of %d" % (vi + 1, len(positions)))
                        if vi not in remap:
                            x, y, z = positions[vi]
                            remap[vi] = len(out_verts)
                            out_verts.append((int(round(x * scale)),
                                              int(round(y * scale)),
                                              int(round(z * scale))))
                        idx.append(remap[vi])
                        if not (0 <= ti < len(uvs)):
                            texels.append((0, 0))
                        elif is_tmd:
                            u, v = uvs[ti]
                            texels.append((max(0, min(255, int(u))), max(0, min(255, int(v)))))
                        else:
                            texels.append(to_texel(uvs[ti], tex_w, tex_h))
                    if len(idx) == 3:
                        idx.append(TRIANGLE)
                        texels.append((0, 0))
                    elif not is_tmd:
                        # OBJ winds quads around the perimeter; the GPU wants
                        # Z order, so the last two corners swap. TMD quads are
                        # stored in Z order already.
                        idx[2], idx[3] = idx[3], idx[2]
                        texels[2], texels[3] = texels[3], texels[2]
                    out_faces.append((idx, texels))
            out_parts.append({
                'parent': index_of[p['parent']] if p.get('parent') else -1,
                'pivot': [int(round(v * scale)) for v in conv(p.get('pivot', [0, 0, 0]))],
                'firstVert': first_vert, 'vertCount': len(out_verts) - first_vert,
                'firstFace': first_face, 'faceCount': len(out_faces) - first_face,
            })

    if fix_wind:
        (out_faces, n_flipped, n_boundary, n_pieces,
         n_nonman) = fix_winding(out_faces, out_verts, verbose)
        if n_flipped:
            print("mkmesh: winding: flipped %d of %d faces to match their neighbours"
                  % (n_flipped, len(out_faces)))
        elif verbose:
            print("mkmesh: winding: already consistent across %d piece(s)" % n_pieces)
        if n_nonman and verbose:
            print("mkmesh: winding: %d edges join more than two faces; those junctions were "
                  "left alone" % n_nonman)
        if n_boundary:
            warn("%d edges belong to only one face, so this mesh is not closed. Backface "
                 "culling will let you see straight through those openings into the inside "
                 "of the model. Cap them in your modeller, or call scene.cull(CULL_NONE) and "
                 "pay double the fill rate." % n_boundary)

    part_of_vert = [0] * len(out_verts)
    for pi, p in enumerate(out_parts):
        for i in range(p['firstVert'], p['firstVert'] + p['vertCount']):
            part_of_vert[i] = pi
    n_same, n_cross = find_overlaps(out_faces, out_verts, part_of_vert)
    if n_same:
        warn("%d faces sit exactly on top of another face in the same part. Nothing can ever "
             "move them apart, so one of each pair is always hidden and only costs you "
             "transform time." % n_same)
    if n_cross and verbose:
        print("mkmesh: %d coincident faces sit across a joint -- segment caps, which is what "
              "keeps the model closed when the joint bends. Left alone." % n_cross)

    for v in out_verts:
        for c in v:
            if not -32768 <= c <= 32767:
                die("vertex coordinate %d overflows int16 -- lower \"scale\" (currently %d)" % (c, scale))

    # Collision boxes
    out_boxes = []
    for b in cfg.get('boxes', []):
        if b['part'] not in index_of:
            die("box '%s' is attached to unknown part '%s'" % (b['name'], b['part']))
        if len(b['name'].encode()) >= BOX_NAME_LEN:
            die("box name '%s' is longer than %d bytes" % (b['name'], BOX_NAME_LEN - 1))
        raw_lo = [int(round(v * scale)) for v in conv(b['min'])]
        raw_hi = [int(round(v * scale)) for v in conv(b['max'])]
        # conv negates y and z, which turns those mins into maxes.
        lo = [min(raw_lo[k], raw_hi[k]) for k in range(3)]
        hi = [max(raw_lo[k], raw_hi[k]) for k in range(3)]
        for k in range(3):
            if b['min'][k] > b['max'][k]:
                die("box '%s' has min > max on axis %d" % (b['name'], k))
        out_boxes.append({'name': b['name'], 'part': index_of[b['part']], 'min': lo, 'max': hi})

    # Animations
    out_anims, out_frames = [], []
    if is_bb:
        for a in bb['anims']:
            name = a['name']
            if len(name.encode()) >= NAME_LEN:
                warn("animation '%s' is longer than %d bytes and was truncated"
                     % (name, NAME_LEN - 1))
                name = name.encode()[:NAME_LEN - 1].decode('utf-8', 'ignore')
            out_anims.append({'name': name, 'firstFrame': len(out_frames),
                              'frameCount': len(a['frames']), 'fps': a['fps'],
                              'flags': 1 if a['loop'] else 0})
            for pose in a['frames']:
                row = []
                for rot, trans in pose:
                    # The axis conversion is a half turn about x, which negates
                    # the sense of rotation about the other two axes.
                    r = [rot[0], -rot[1], -rot[2]] if flip else list(rot)
                    t = conv(trans)
                    row.append([int(round(v * ANGLE_TURN / 360.0)) % ANGLE_TURN for v in r]
                               + [int(round(v * scale)) for v in t])
                out_frames.append(row)

    for name, a in cfg.get('animations', {}).items():
        if len(name.encode()) >= NAME_LEN:
            die("animation name '%s' is longer than %d bytes" % (name, NAME_LEN - 1))
        frames = a.get('frames', [])
        if not frames:
            die("animation '%s' has no frames" % name)
        out_anims.append({'name': name, 'firstFrame': len(out_frames),
                          'frameCount': len(frames), 'fps': a.get('fps', 15),
                          'flags': 1 if a.get('loop', True) else 0})
        for fi, frame in enumerate(frames):
            for pname in frame:
                if pname not in index_of:
                    die("animation '%s' frame %d poses unknown part '%s'" % (name, fi, pname))
            poses = []
            for pname in names:
                pose = frame.get(pname, {})
                # Rotating the model 180 degrees about x also reverses the
                # sense of rotation about the other two axes.
                r = pose.get('rot', [0, 0, 0])
                rot = [r[0], -r[1], -r[2]] if flip else list(r)
                trans = [v * scale for v in conv(pose.get('trans', [0, 0, 0]))]
                poses.append([int(round(v)) % ANGLE_TURN for v in rot] + [int(round(v)) for v in trans])
            out_frames.append(poses)

    # ---- serialise ----
    blob = bytearray()
    blob += MAGIC
    blob += struct.pack('<HH', VERSION, 0)
    blob += struct.pack('<HHHHHH', len(out_parts), len(out_verts), len(out_faces),
                        len(out_boxes), len(out_anims), len(out_frames))
    blob += struct.pack('<HH', tex_w, tex_h)
    for p in out_parts:
        blob += struct.pack('<hhhhHHHHH', p['parent'], p['pivot'][0], p['pivot'][1], p['pivot'][2],
                            p['firstVert'], p['vertCount'], p['firstFace'], p['faceCount'], 0)
    for v in out_verts:
        blob += struct.pack('<hhhh', v[0], v[1], v[2], 0)
    for idx, texels in out_faces:
        blob += struct.pack('<HHHH', *idx)
        for u, v in texels:
            blob += struct.pack('<BB', u, v)
    for b in out_boxes:
        blob += b['name'].encode().ljust(BOX_NAME_LEN, b'\0')
        blob += struct.pack('<BB', b['part'], 0)
        blob += struct.pack('<hhhhhh', *(b['min'] + b['max']))
    for a in out_anims:
        blob += a['name'].encode().ljust(NAME_LEN, b'\0')
        blob += struct.pack('<HHHH', a['firstFrame'], a['frameCount'], a['fps'], a['flags'])
    for poses in out_frames:
        for pose in poses:
            blob += struct.pack('<hhhhhh', *pose)

    open(out_path, 'wb').write(blob)
    tris = sum(1 for idx, _ in out_faces if idx[3] == TRIANGLE)
    if len(out_faces) > 600:
        warn("%d faces is a lot for one PS1 object; typical in-game models are 200-500. "
             "Expect this to cost real frame time." % len(out_faces))
    print("mkmesh: %s -> %s%s" % (obj_path, out_path, "  (axes converted from OBJ space)" if flip else ""))
    print("  %d parts, %d verts, %d faces (%d tris + %d quads), %d boxes, %d anims, %d keyframes"
          % (len(out_parts), len(out_verts), len(out_faces), tris, len(out_faces) - tris,
             len(out_boxes), len(out_anims), len(out_frames)))
    print("  %d bytes" % len(blob))
    if verbose:
        for i, p in enumerate(out_parts):
            print("    part %d %-10s parent=%-3d verts %d..%d faces %d..%d"
                  % (i, names[i], p['parent'], p['firstVert'], p['firstVert'] + p['vertCount'] - 1,
                     p['firstFace'], p['firstFace'] + p['faceCount'] - 1))
    return blob


def main():
    ap = argparse.ArgumentParser(description="Build a PS1M mesh from an OBJ or a Sony TMD, plus an optional JSON sidecar.")
    ap.add_argument('obj', metavar='MODEL', help="an .obj, a Sony .tmd, or a Blockbench .bbmodel")
    ap.add_argument('config', nargs='?',
                    help="optional JSON sidecar with parts, animations and collision boxes; "
                         "without it the model becomes one static part")
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('-s', '--scale', type=int, help="model units per world unit (overrides the sidecar)")
    ap.add_argument('-t', '--texture', type=int, nargs=2, metavar=('W', 'H'),
                    help="texture size the UVs were authored against (overrides the sidecar)")
    ap.add_argument('--fps', type=int, default=15,
                    help="frame rate to resample Blockbench animations onto (default 15). "
                         "The engine stores uniform frames, so arbitrary keyframe times are "
                         "sampled at this rate; higher is smoother and larger")
    ap.add_argument('--no-fix-winding', dest='fix_winding', action='store_false',
                    help="leave face winding exactly as the model file has it. By default "
                         "mkmesh makes it consistent, because a single reversed face is "
                         "invisible under backface culling and reads as a hole in the model")
    ap.add_argument('--axis', choices=('obj', 'raw'),
                    help="'obj' rotates the model 180 degrees about x, converting OBJ's y-up "
                         "z-towards-viewer space into the engine's y-down z-into-screen space "
                         "(the default for .obj input). 'raw' passes coordinates through "
                         "unchanged (the default for .tmd, which is already in that space)")
    args = ap.parse_args()
    build(args.obj, args.config, args.output, args.verbose, args.scale, args.texture, args.axis,
          args.fix_winding, args.fps)


if __name__ == '__main__':
    main()
