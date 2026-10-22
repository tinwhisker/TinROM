#!/usr/bin/env python3
"""tinrom - measure how badly a mesh sorts under the PS1's painter's algorithm.

Transforms a mesh the way src/ui/ui.c does, buckets each face the way
src/ui/model.c does, then rasterises it twice: once in ordering-table order,
and once against a true depth buffer. Pixels where the two disagree are the
"model draws over itself" artefact.

    python3 tools/sortsim.py mesh.obj [mesh2.obj ...]

CAUTION, and the reason this docstring exists: the ground truth has to be the
depth of the face's PLANE AT THAT PIXEL, not the face's average depth. An
earlier version of this tool compared average depths - which is the very
quantity the painter's algorithm buckets - so it could only ever detect bucket
quantisation, never an actual occlusion error. It reported a change as a large
improvement when the change made the artefact worse on hardware.
"""

import argparse
import collections
import math
import sys

# Matching src/ui/ui.c and src/ui/ui.h.
RADIUS_TARGET = 40
DISTANCE = 100
TILT_X = 300
Z_NEAR, Z_FAR = 128, 238
H, OFX, OFY = 120, 242, 170


def load(path):
    positions, faces = [], []

    for line in open(path):
        bits = line.split()

        if not bits:
            continue

        if bits[0] == "v":
            positions.append([float(x) for x in bits[1:4]])
        elif bits[0] == "f":
            idx = [int(t.split("/")[0]) for t in bits[1:]]
            faces.append([i - 1 if i > 0 else len(positions) + i for i in idx])

    scale = RADIUS_TARGET / max(math.dist((0, 0, 0), p) for p in positions)

    return [[c * scale for c in p] for p in positions], faces


def transform(positions, ax, ay):
    ca, sa = math.cos(ax), math.sin(ax)
    cb, sb = math.cos(ay), math.sin(ay)
    out = []

    # model.c composes identity, then X, then Y, then Z - and
    # multiplyRotationMatrixByVectors() post-multiplies, so the matrix is
    # Rx.Ry.Rz and a vertex meets Y FIRST, then X. Doing it the other way
    # round tilts the spin axis instead of spinning under a fixed tilt, which
    # is a different set of views and therefore a different answer.
    for x, y, z in positions:
        x, z = x * cb + z * sb, -x * sb + z * cb
        y, z = y * ca - z * sa, y * sa + z * ca
        out.append((x, y, z + DISTANCE))

    return out


def project(v):
    x, y, z = v
    z = max(z, 1.0)

    return (OFX + x * H / z, OFY + y * H / z, z)


def signed_area(p):
    return ((p[1][0] - p[0][0]) * (p[2][1] - p[0][1])
          - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]))


def plane(view):
    """Plane of a face in view space, as (normal, offset)."""
    a, b, c = view[0], view[1], view[2]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1],
         u[2] * v[0] - u[0] * v[2],
         u[0] * v[1] - u[1] * v[0])

    return n, sum(n[i] * a[i] for i in range(3))


def depth_at(pl, sx, sy):
    """View-space Z where the ray through screen pixel (sx, sy) hits the plane.

    This - not the face's average Z - is what a depth buffer would store, and
    comparing against anything else measures the wrong thing.
    """
    n, d = pl
    denom = n[0] * (sx - OFX) / H + n[1] * (sy - OFY) / H + n[2]

    if abs(denom) < 1e-9:
        return float("inf")

    z = d / denom

    return z if z > 0 else float("inf")


def contains(poly, x, y):
    n = len(poly)
    s = 0

    for k in range(n):
        a, b = poly[k], poly[(k + 1) % n]
        cross = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])

        if cross > 0:
            s += 1
        elif cross < 0:
            s -= 1

    return abs(s) == n


def bucket(zs, key):
    depth = {"avg": sum(zs) / len(zs),
             "min": min(zs),
             "max": max(zs)}[key]
    depth = max(0, min(2 * RADIUS_TARGET, depth - (DISTANCE - RADIUS_TARGET)))

    return Z_NEAR + int(depth * (Z_FAR - Z_NEAR) / (2 * RADIUS_TARGET))


def components(positions, faces):
    """Group faces into connected pieces, by welded position.

    A mesh of separate convex solids - which is what a box-and-legs model is -
    can be sorted per piece instead of per face, and that is a different
    algorithm with different failure modes, so it is worth measuring.
    """
    key, remap = {}, []

    for p in positions:
        k = tuple(round(c, 5) for c in p)
        key.setdefault(k, len(key))
        remap.append(key[k])

    parent = list(range(len(key)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for f in faces:
        w = [remap[i] for i in f]
        for b in w[1:]:
            ra, rb = find(w[0]), find(b)
            if ra != rb:
                parent[rb] = ra

    labels = {}
    out = []

    for f in faces:
        r = find(remap[f[0]])
        labels.setdefault(r, len(labels))
        out.append(labels[r])

    return out, len(labels)


def run(positions, faces, key="avg", angles=8, grid=110, group=False):
    blame = collections.Counter()
    wrong = shaded = 0
    worst = 0.0
    peak = 0
    parts = components(positions, faces)[0] if group else None

    for step in range(angles):
        ay = 2 * math.pi * step / angles
        view = transform(positions, TILT_X * 2 * math.pi / 4096, ay)
        proj = [project(v) for v in view]

        visible = []

        if group:
            # One bucket per piece, pieces ordered by their centroid depth and
            # given consecutive buckets so two can never collide. Inside a
            # convex piece the front faces do not overlap each other, so their
            # order within the bucket does not matter.
            depth = {}

            for fi, f in enumerate(faces):
                depth.setdefault(parts[fi], []).extend(view[i][2] for i in f)

            if group == "near":
                ranked = sorted(depth, key=lambda c: -min(depth[c]))
            else:
                ranked = sorted(depth,
                                key=lambda c: -sum(depth[c]) / len(depth[c]))
            slot = {c: Z_NEAR + i for i, c in enumerate(reversed(ranked))}

        for fi, f in enumerate(faces):
            p = [proj[i] for i in f]

            if signed_area(p) >= 0:      # MODEL_FRONT_FACE_AREA_POSITIVE = 0
                continue

            xs = [q[0] for q in p]
            ys = [q[1] for q in p]
            z = slot[parts[fi]] if group else bucket([q[2] for q in p], key)
            visible.append((z, p,
                            plane([view[i] for i in f]), fi,
                            min(xs), max(xs), min(ys), max(ys)))

        peak = max(peak, len(visible))

        if not visible:
            continue

        # Back to front: a higher bucket is sent first. Within a bucket the
        # chain is LIFO, so the later allocation is drawn first.
        order = sorted(range(len(visible)),
                       key=lambda i: (-visible[i][0], -i))

        x0 = min(e[4] for e in visible); x1 = max(e[5] for e in visible)
        y0 = min(e[6] for e in visible); y1 = max(e[7] for e in visible)

        bad = px = 0

        for gy in range(grid):
            y = y0 + (y1 - y0) * (gy + 0.5) / grid
            row = [(i, visible[i]) for i in order
                   if visible[i][6] <= y <= visible[i][7]]

            for gx in range(grid):
                x = x0 + (x1 - x0) * (gx + 0.5) / grid
                painter = nearest = None
                best = float("inf")

                for i, e in row:
                    if x < e[4] or x > e[5]:
                        continue
                    if not contains(e[1], x, y):
                        continue

                    painter = i                      # last drawn wins
                    z = depth_at(e[2], x, y)

                    if z < best:
                        best, nearest = z, i

                if painter is None:
                    continue

                px += 1

                if painter != nearest:
                    bad += 1
                    blame[visible[painter][3]] += 1

        wrong += bad
        shaded += px
        worst = max(worst, 100 * bad / px if px else 0)

    return {"wrong": 100 * wrong / shaded if shaded else 0,
            "worst": worst, "peak": peak, "blame": blame}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("meshes", nargs="+")
    parser.add_argument("--keys", default="avg,min,max")
    parser.add_argument("--angles", type=int, default=8)
    parser.add_argument("--grid", type=int, default=110)
    parser.add_argument("--blame", type=int, default=6)

    args = parser.parse_args(argv)

    for path in args.meshes:
        positions, faces = load(path)
        print(f"{path}: {len(positions)} vertices, {len(faces)} faces")

        pieces = components(positions, faces)[1]
        print(f"  {pieces} connected piece(s)")

        for key in args.keys.split(","):
            r = run(positions, faces, key, args.angles, args.grid)
            print(f"  per face, sort by {key:4s}  wrong {r['wrong']:6.2f}%   "
                  f"worst frame {r['worst']:6.2f}%   "
                  f"peak visible {r['peak']:3d} ({r['peak'] * 10} words)")

        for mode, label in (("centre", "per piece, centroid"),
                            ("near", "per piece, nearest  ")):
            r = run(positions, faces, "avg", args.angles, args.grid, group=mode)
            print(f"  {label}  wrong {r['wrong']:6.2f}%   "
                  f"worst frame {r['worst']:6.2f}%   "
                  f"peak visible {r['peak']:3d} ({r['peak'] * 10} words)")

        r = run(positions, faces, "avg", args.angles, args.grid)
        total = sum(r["blame"].values()) or 1
        print(f"  faces ever drawn wrongly: {len(r['blame'])}")

        for fi, n in r["blame"].most_common(args.blame):
            print(f"    face {fi:4d}  {100 * n / total:5.1f}%")

        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
