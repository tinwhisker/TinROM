"""Read Blockbench models (.bbmodel), armature and all.

Blockbench's newer "free" format carries a real skeleton: bones with a rest
pose, per-vertex weights, and keyframed animations. That is very nearly what
this engine wants, with two differences that this module resolves.

Skinning vs rigid parts
-----------------------
Blockbench blends a vertex between several bones. The engine gives each part
one matrix and no per-vertex blending -- the PS1 has no budget for anything
else. So each vertex is bound to its heaviest bone. That is lossy in
principle; in practice character rigs of this kind are almost entirely
rigid already, and the module reports how much blending it actually threw
away so you can see whether it mattered.

Faces are allowed to reference vertices in several parts. A polygon across a
joint then stretches between two bones instead of tearing, which is what makes
rigid binding look acceptable at all. Roughly a quarter of the faces on a
typical rig are in that position, so it is not a detail.

Rest poses
----------
A Blockbench bone has a rest rotation, and its keyframes are rotations in
*bone* space on top of that. Engine parts have a pivot and no rest rotation,
and their pose rotation acts in model space. The two are reconciled by
conjugation: a bone-space rotation A on a bone whose rest orientation in the
world is Q is the model-space rotation Q * A * Q^-1 about the bone's rest
world position. That is exactly what the engine's "rotate about the pivot"
composition then reproduces down the chain.

Child bone origins are in the parent's *rotated* space, which was established
by checking computed bone positions against the centroids of the vertices
weighted to them -- the alternative reading put them four times further away.

Sampling
--------
Keyframes land at arbitrary times and may be linear, stepped or smooth. The
engine's format is uniform frames at a fixed rate with linear interpolation
between them, so animations are resampled: Blockbench's own curve is evaluated
at each frame time. Smooth keyframes survive as well as the frame rate allows,
which is the same trade the console would make anyway.
"""

import json
import math


class BbError(Exception):
    pass


# ---------------------------------------------------------------------------
# Small matrix helpers. Kept local and explicit -- the whole file does a few
# hundred 3x3 operations at build time, so clarity beats speed.
# ---------------------------------------------------------------------------

def _mm(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def _mv(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def _transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def _identity():
    return [[1.0, 0, 0], [0, 1.0, 0], [0, 0, 1.0]]


def _euler_to_matrix(deg):
    """Blockbench applies X, then Y, then Z, so the matrix is Rz * Ry * Rx."""
    rx, ry, rz = [math.radians(v) for v in deg]
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    mx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    my = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    mz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    return _mm(mz, _mm(my, mx))


def matrix_to_engine_euler(r):
    """Decompose into the engine's order, Ry * Rx * Rz, in radians.

    From that product:
        r[1][2] = -sin(x)
        r[1][0], r[1][1] = cos(x)sin(z), cos(x)cos(z)
        r[0][2], r[2][2] = sin(y)cos(x), cos(y)cos(x)
    When cos(x) is zero the y and z terms both vanish -- gimbal lock -- and only
    their sum is recoverable, so z is pinned to zero and y takes all of it.
    """
    sx = max(-1.0, min(1.0, -r[1][2]))
    x = math.asin(sx)
    if abs(r[1][2]) < 0.999999:
        z = math.atan2(r[1][0], r[1][1])
        y = math.atan2(r[0][2], r[2][2])
    else:
        z = 0.0
        y = math.atan2(-r[2][0], r[0][0])
    return x, y, z


# ---------------------------------------------------------------------------
# Keyframe evaluation
# ---------------------------------------------------------------------------

def _point(kf):
    dp = (kf.get('data_points') or [{}])[0]
    out = []
    for axis in ('x', 'y', 'z'):
        v = dp.get(axis, 0)
        try:
            out.append(float(v))
        except (TypeError, ValueError):
            # Blockbench allows MoLang expressions in a data point. Evaluating
            # them is out of scope; treat as zero and let the caller report it.
            out.append(0.0)
    return out


def _sample(keyframes, t):
    """Value of one channel at time t, honouring the keyframe interpolation."""
    if not keyframes:
        return [0.0, 0.0, 0.0]
    ks = sorted(keyframes, key=lambda k: float(k.get('time', 0)))
    if t <= float(ks[0].get('time', 0)):
        return _point(ks[0])
    if t >= float(ks[-1].get('time', 0)):
        return _point(ks[-1])

    i = 0
    while i < len(ks) - 1 and float(ks[i + 1].get('time', 0)) <= t:
        i += 1
    k0, k1 = ks[i], ks[i + 1]
    t0, t1 = float(k0.get('time', 0)), float(k1.get('time', 0))
    span = t1 - t0
    f = 0.0 if span <= 0 else (t - t0) / span
    p0, p1 = _point(k0), _point(k1)

    mode = k0.get('interpolation', 'linear')
    if mode == 'step':
        return p0
    if mode == 'catmullrom':
        pm = _point(ks[i - 1]) if i > 0 else p0
        pn = _point(ks[i + 2]) if i + 2 < len(ks) else p1
        out = []
        for a in range(3):
            m0, m1, m2, m3 = pm[a], p0[a], p1[a], pn[a]
            out.append(0.5 * ((2 * m1)
                              + (-m0 + m2) * f
                              + (2 * m0 - 5 * m1 + 4 * m2 - m3) * f * f
                              + (-m0 + 3 * m1 - 3 * m2 + m3) * f * f * f))
        return out
    return [p0[a] + (p1[a] - p0[a]) * f for a in range(3)]


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_bbmodel(path, fps=15, report=False):
    """Returns a dict the mkmesh builder consumes directly.

        verts       [(x, y, z)]        model space, Blockbench units,
                                       already ordered so each part owns a
                                       contiguous run
        vert_part   [part index]       parallel to verts
        parts       [{name, parent, pivot}]   parents always earlier
        faces       [[(vert index, (u, v)), ...]]   perimeter order
        anims       [{name, fps, loop, frames}]
                    frames[f][p] = (rot3 degrees, trans3 units) in model space
        texture     (w, h)
        stats       {...} for the caller to report
    """
    with open(path) as fh:
        doc = json.load(fh)

    elements = doc.get('elements', [])
    meshes = [e for e in elements if e.get('type') == 'mesh']
    bones = [e for e in elements if e.get('type') == 'armature_bone']
    if not meshes:
        raise BbError("%s has no mesh element. Blockbench formats that build "
                      "from cubes rather than a free mesh are not supported; "
                      "convert the project to the 'free' format first." % path)
    if len(meshes) > 1:
        raise BbError("%s has %d mesh elements; only one is supported. Join "
                      "them in Blockbench." % (path, len(meshes)))
    mesh = meshes[0]
    if not bones:
        raise BbError("%s has no armature bones -- nothing to import that the "
                      "OBJ would not already give you." % path)

    by_id = {e['uuid']: e for e in elements}

    # ---- bone hierarchy, from the outliner ----
    parent_of = {}
    order = []

    def walk(nodes, parent):
        for n in nodes:
            uid = n if isinstance(n, str) else n.get('uuid')
            e = by_id.get(uid)
            if e is None:
                if isinstance(n, dict):
                    walk(n.get('children', []), parent)
                continue
            if e.get('type') == 'armature_bone':
                parent_of[e['name']] = parent
                order.append(e['name'])
                walk(e.get('children', []), e['name'])
            else:
                kids = e.get('children')
                walk(kids if isinstance(kids, list) else [], parent)

    walk(doc.get('outliner', []), None)
    bone_by_name = {b['name']: b for b in bones}
    for b in bones:
        if b['name'] not in parent_of:
            # not reachable from the outliner; treat as a root
            parent_of[b['name']] = None
            order.append(b['name'])
    if len(set(order)) != len(order):
        raise BbError("%s has two bones with the same name; names must be "
                      "unique" % path)

    # Depth order guarantees parents come first, which the engine requires.
    def depth(name):
        d, p = 0, parent_of[name]
        while p is not None:
            d += 1
            p = parent_of[p]
        return d

    order.sort(key=depth)
    index_of = {n: i for i, n in enumerate(order)}

    # ---- rest pose in model space ----
    # A child's origin is expressed in its parent's rotated frame, so the
    # parent's orientation has to be applied before adding the offset.
    rest_pos, rest_rot = {}, {}
    for name in order:
        b = bone_by_name[name]
        p = parent_of[name]
        local = _euler_to_matrix(b.get('rotation', [0, 0, 0]))
        origin = [float(v) for v in b.get('origin', [0, 0, 0])]
        if p is None:
            rest_pos[name] = origin
            rest_rot[name] = local
        else:
            off = _mv(rest_rot[p], origin)
            rest_pos[name] = [rest_pos[p][i] + off[i] for i in range(3)]
            rest_rot[name] = _mm(rest_rot[p], local)

    # ---- weights -> one bone per vertex ----
    weights = {}
    for b in bones:
        for key, val in (b.get('vertex_weights') or {}).items():
            vkey = key.split(':', 1)[1] if ':' in key else key
            try:
                val = float(val)
            except (TypeError, ValueError):
                continue
            if val > 0:
                weights.setdefault(vkey, {})[b['name']] = val

    root = order[0]
    for name in order:
        if parent_of[name] is None:
            root = name
            break

    mesh_origin = [float(v) for v in (mesh.get('origin') or [0, 0, 0])]
    raw = mesh.get('vertices') or {}

    bound, unbound, discarded = {}, 0, 0.0
    for vkey in raw:
        w = weights.get(vkey)
        if not w:
            bound[vkey] = root
            unbound += 1
            continue
        best = max(w, key=w.get)
        if best not in index_of:
            bound[vkey] = root
            unbound += 1
            continue
        bound[vkey] = best
        total = sum(w.values())
        if total > 0:
            discarded = max(discarded, 1.0 - w[best] / total)

    # ---- vertices, grouped so each part owns a contiguous run ----
    verts, vert_part, vindex = [], [], {}
    for pi, name in enumerate(order):
        for vkey, v in raw.items():
            if bound[vkey] != name:
                continue
            vindex[vkey] = len(verts)
            verts.append(tuple(mesh_origin[i] + float(v[i]) for i in range(3)))
            vert_part.append(pi)

    # ---- faces ----
    faces, skipped_faces = [], 0
    for f in (mesh.get('faces') or {}).values():
        vs = f.get('vertices') or []
        if len(vs) < 3:
            skipped_faces += 1       # Blockbench keeps stray edges and points
            continue
        if len(vs) > 4:
            skipped_faces += 1
            continue
        uvmap = f.get('uv') or {}
        corners = []
        ok = True
        for vkey in vs:
            if vkey not in vindex:
                ok = False
                break
            uv = uvmap.get(vkey, [0, 0])
            corners.append((vindex[vkey], (float(uv[0]), float(uv[1]))))
        if not ok:
            skipped_faces += 1
            continue
        faces.append(corners)

    parts = [{'name': n,
              'parent': None if parent_of[n] is None else index_of[parent_of[n]],
              'pivot': rest_pos[n]}
             for n in order]

    # ---- animations, resampled onto uniform frames ----
    anims = []
    molang = 0
    for a in doc.get('animations', []):
        length = float(a.get('length') or 0)
        animators = a.get('animators') or {}
        by_bone = {}
        for an in animators.values():
            if an.get('type') != 'armature_bone':
                continue
            name = an.get('name')
            if name not in index_of:
                continue
            chans = {}
            for kf in an.get('keyframes') or []:
                chans.setdefault(kf.get('channel'), []).append(kf)
                for dp in kf.get('data_points') or []:
                    for axis in ('x', 'y', 'z'):
                        v = dp.get(axis)
                        if isinstance(v, str):
                            try:
                                float(v)
                            except ValueError:
                                molang += 1
            by_bone[name] = chans
        if not by_bone:
            continue

        if length <= 0:
            # a single pose: one frame, nothing to interpolate
            times = [0.0]
        else:
            n = max(2, int(round(length * fps)) + 1)
            times = [length * i / (n - 1) for i in range(n)]

        frames = []
        for t in times:
            pose = []
            for name in order:
                chans = by_bone.get(name)
                if not chans:
                    pose.append(([0.0, 0.0, 0.0], [0.0, 0.0, 0.0]))
                    continue
                local = _euler_to_matrix(_sample(chans.get('rotation', []), t))
                q = rest_rot[name]
                # bone-space rotation expressed in model space
                world = _mm(q, _mm(local, _transpose(q)))
                x, y, z = matrix_to_engine_euler(world)
                trans = _sample(chans.get('position', []), t)
                trans = _mv(q, trans) if any(trans) else [0.0, 0.0, 0.0]
                pose.append(([math.degrees(x), math.degrees(y), math.degrees(z)], trans))
            frames.append(pose)

        anims.append({'name': a.get('name') or 'anim',
                      'fps': fps if length > 0 else 1,
                      'loop': a.get('loop') == 'loop',
                      'frames': frames})

    res = doc.get('resolution') or {}
    texture = (int(res.get('width') or 64), int(res.get('height') or 64))

    stats = {
        'bones': len(order),
        'vertices': len(verts),
        'faces': len(faces),
        'skipped_faces': skipped_faces,
        'unbound': unbound,
        'worst_blend': discarded,
        'molang': molang,
        'animations': len(anims),
        'root': root,
    }

    if report:
        print("bbmodel: %s" % path)
        print("  %d bones, parents first: %s" % (len(order), ', '.join(order)))
        for n in order:
            p = parent_of[n]
            print("    %-10s parent=%-10s pivot=%s"
                  % (n, p or '-', ['%.2f' % v for v in rest_pos[n]]))
        print("  %d vertices, %d faces, texture %dx%d" % (len(verts), len(faces), texture[0], texture[1]))
        for a in anims:
            print("  animation %-12r %d frames at %dfps%s"
                  % (a['name'], len(a['frames']), a['fps'], ', looping' if a['loop'] else ''))

    return {'verts': verts, 'vert_part': vert_part, 'parts': parts, 'faces': faces,
            'anims': anims, 'texture': texture, 'stats': stats}
