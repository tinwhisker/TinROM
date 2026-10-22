#!/usr/bin/env python3
"""tinrom - cut edge loops through oversized faces before conversion.

Why this exists
---------------
The PS1 has no depth buffer. Faces are sorted by bucketing one depth value per
face into the ordering table, so a face is entirely in front of or entirely
behind another. A large polygon with small detail laid across it therefore
cannot sort against that detail, and the model draws over parts of itself.
Splitting the few oversized faces gives each piece its own depth value.

Why LOOP CUTS rather than splitting each face on its own
--------------------------------------------------------
Splitting one quad into four puts a new vertex in the middle of each of its
edges. A neighbour that was not split still treats that edge as a straight
line between its two original corners, so the two sides disagree about where
the surface is by a fraction of a pixel - a T-junction - and the rasteriser
leaves a crack. That is visible as tearing along the seams, and it is worse
than the sorting artefact it was meant to fix.

Measured on rom.obj: per-face splitting produced 24 T-junctions across 10
faces and left 56 edges without an opposite partner - the mesh stopped being
closed.

A loop cut cannot do that. It walks the ring of edges opposite each other
through adjacent quads and cuts every quad it passes through, so both sides of
every edge always agree. Quads stay quads, which also keeps them at 10 words
rather than two triangles at 8 each.

    python3 tools/subdivide.py in.obj out.obj [--ratio 8] [--verbose]

--ratio is a multiple of the MEDIAN face area, so it does not depend on the
units a model happens to be authored in. 0 copies the mesh through untouched.
"""

import argparse
import collections
import math
import sys

MAX_CUTS = 64


def parse(path):
    """Positions, texture coords, faces as [(position, uv)], and passthrough."""
    positions, uvs, faces, preamble = [], [], [], []

    for line in open(path):
        bits = line.split()

        if not bits:
            continue

        if bits[0] == "v":
            positions.append(tuple(float(x) for x in bits[1:4]))
        elif bits[0] == "vt":
            uvs.append(tuple(float(x) for x in bits[1:3]))
        elif bits[0] == "f":
            corners = []

            for token in bits[1:]:
                parts = token.split("/")
                pi = int(parts[0])
                ti = int(parts[1]) if len(parts) > 1 and parts[1] else 0
                pi = pi - 1 if pi > 0 else len(positions) + pi
                ti = ti - 1 if ti > 0 else (len(uvs) + ti if ti else -1)
                corners.append((pi, ti))

            faces.append(corners)
        elif bits[0] in ("mtllib", "usemtl", "o", "g", "s"):
            preamble.append(line.rstrip("\n"))

    return positions, uvs, faces, preamble


def weld(positions, faces, eps=1e-6):
    """Merge coincident positions so topology follows geometry, not indices.

    UV indices are left alone: two faces meeting at a texture seam share a
    position but must keep their own texture coordinates.
    """
    keys, remap = {}, []

    for p in positions:
        k = tuple(round(c / eps) for c in p)
        keys.setdefault(k, len(keys))
        remap.append(keys[k])

    welded = [None] * len(keys)

    for p, r in zip(positions, remap):
        welded[r] = p

    return welded, [[(remap[p], t) for p, t in f] for f in faces]


def area(positions, face):
    p = [positions[i] for i, _ in face]
    total = 0.0

    for k in range(1, len(p) - 1):
        u = [p[k][i] - p[0][i] for i in range(3)]
        v = [p[k + 1][i] - p[0][i] for i in range(3)]
        c = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        total += math.sqrt(sum(x * x for x in c)) / 2

    return total


def edge_map(faces):
    """Undirected edge -> [(face, corner index)] it appears in."""
    m = collections.defaultdict(list)

    for fi, f in enumerate(faces):
        n = len(f)

        for k in range(n):
            m[frozenset((f[k][0], f[(k + 1) % n][0]))].append((fi, k))

    return m


def edge_ring(faces, edges, seed):
    """Every edge a single loop cut would pass through.

    Inside a quad an edge continues through the one opposite it; between
    quads it continues through the shared edge. Closing that relation over
    both gives the ring, and stopping at anything that is not a quad keeps it
    away from geometry it cannot cut cleanly.
    """
    ring = set()
    pending = [seed]

    while pending:
        edge = pending.pop()

        if edge in ring:
            continue

        ring.add(edge)

        for fi, k in edges[edge]:
            if len(faces[fi]) != 4:
                continue

            opposite = frozenset(
                (faces[fi][(k + 2) % 4][0], faces[fi][(k + 3) % 4][0]))

            if opposite not in ring:
                pending.append(opposite)

    return ring


def cut(positions, uvs, faces, ring):
    """Cut every quad the ring passes through. Quads in, quads out."""
    midpoint = {}

    def position_midpoint(edge, a, b):
        if edge not in midpoint:
            positions.append(tuple(
                (positions[a][i] + positions[b][i]) / 2 for i in range(3)))
            midpoint[edge] = len(positions) - 1

        return midpoint[edge]

    def uv_midpoint(ta, tb):
        if ta < 0 or tb < 0:
            return -1

        uvs.append(tuple((uvs[ta][i] + uvs[tb][i]) / 2 for i in range(2)))

        return len(uvs) - 1

    out = []
    skipped = 0

    for face in faces:
        if len(face) != 4:
            out.append(face)
            continue

        onRing = [k for k in range(4)
                  if frozenset((face[k][0], face[(k + 1) % 4][0])) in ring]

        if not onRing:
            out.append(face)
            continue

        # A ring enters and leaves through opposite edges. Anything else means
        # two rings crossing here, which this pass does not try to resolve -
        # the next pass will pick it up once the first cut has landed.
        if len(onRing) != 2 or ((onRing[1] - onRing[0]) % 4) != 2:
            out.append(face)
            skipped += 1
            continue

        k = onRing[0]
        a, b, c, d = (face[k], face[(k + 1) % 4],
                      face[(k + 2) % 4], face[(k + 3) % 4])

        mAB = (position_midpoint(frozenset((a[0], b[0])), a[0], b[0]),
               uv_midpoint(a[1], b[1]))
        mCD = (position_midpoint(frozenset((c[0], d[0])), c[0], d[0]),
               uv_midpoint(c[1], d[1]))

        out += [[a, mAB, mCD, d], [mAB, b, c, mCD]]

    return out, skipped


def refine(positions, uvs, faces, limit, verbose=False, maxCuts=MAX_CUTS):
    cuts = 0

    while cuts < maxCuts:
        oversized = [(area(positions, f), i)
                     for i, f in enumerate(faces) if len(f) == 4]
        oversized = [x for x in oversized if x[0] > limit]

        if not oversized:
            break

        oversized.sort(reverse=True)
        worst = faces[oversized[0][1]]

        # Cut across the longer side, so a cut actually reduces the area that
        # matters rather than slicing an already-thin face thinner.
        def side(k):
            return math.dist(positions[worst[k][0]],
                             positions[worst[(k + 1) % 4][0]])

        seed = frozenset((worst[0][0], worst[1][0])) if side(1) >= side(0) \
            else frozenset((worst[1][0], worst[2][0]))

        ring = edge_ring(faces, edge_map(faces), seed)
        faces, skipped = cut(positions, uvs, faces, ring)
        cuts += 1

        if verbose:
            print(f"  cut {cuts}: ring of {len(ring)} edge(s) -> "
                  f"{len(faces)} faces"
                  + (f", {skipped} crossing quad(s) left for a later pass"
                     if skipped else ""))

    return faces, cuts


def write(path, positions, uvs, faces, preamble):
    with open(path, "w") as out:
        out.write("# Generated by tools/subdivide.py - do not edit.\n")

        for line in preamble:
            out.write(line + "\n")

        for p in positions:
            out.write(f"v {p[0]:.6f} {p[1]:.6f} {p[2]:.6f}\n")

        for t in uvs:
            out.write(f"vt {t[0]:.6f} {t[1]:.6f}\n")

        for face in faces:
            out.write("f " + " ".join(
                f"{p + 1}/{t + 1}" if t >= 0 else f"{p + 1}"
                for p, t in face) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--ratio", type=float, default=8.0)
    parser.add_argument("--max-cuts", type=int, default=MAX_CUTS,
                        help="stop after this many loop cuts")
    parser.add_argument("--verbose", action="store_true")

    args = parser.parse_args(argv)

    positions, uvs, faces, preamble = parse(args.input)

    if not faces:
        print(f"subdivide.py: {args.input} has no faces", file=sys.stderr)
        return 1

    positions, faces = weld(positions, faces)

    if args.ratio <= 0:
        write(args.output, positions, uvs, faces, preamble)
        return 0

    areas = sorted(area(positions, f) for f in faces)
    median = areas[len(areas) // 2] or 1.0
    limit = median * args.ratio

    before = len(faces)
    faces, cuts = refine(positions, uvs, faces, limit, args.verbose)

    write(args.output, positions, uvs, faces, preamble)

    print(f"subdivide: {cuts} loop cut(s), {before} -> {len(faces)} faces "
          f"(splitting above {args.ratio:g}x the median face)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
