# Why the model drew over itself

The PS1 has no depth buffer. Faces are sorted by putting **one depth value per
face** into an ordering table bucket and drawing back to front. A face is
therefore entirely in front of, or entirely behind, every other face - there
is no way to interleave two that overlap in screen space.

So a large polygon with small detail arranged across it cannot sort correctly
against that detail. One of them wins the whole overlap.

## Measuring it rather than guessing - and getting the measurement wrong

`tools/sortsim.py` transforms a mesh exactly the way `src/ui/ui.c` does,
buckets each face the way `src/ui/model.c` does, then rasterises it twice:
once in ordering-table order and once against a true depth buffer. Pixels
where the two disagree are the artefact.

**Two bugs in that tool sent this document, in its first two revisions, to the
wrong conclusion. They are recorded here because the numbers they produced
were quoted as fact.**

1. The z-buffer ground truth used the face's *average* depth - the very
   quantity the painter's algorithm buckets. Comparing a thing against itself
   can only detect bucket quantisation, never an actual occlusion error. The
   ground truth has to be the depth of the face's **plane at that pixel**:
   `Z = d / (n_x*u/H + n_y*v/H + n_z)`.
2. The rotation order was inverted. `model.c` composes identity -> X -> Y -> Z
   and post-multiplies, so a vertex meets **Y first, then X**. Applying X
   first spins about a tilted axis instead of spinning under a fixed tilt, and
   never visits the angles where the artefact is worst.

With both bugs present the tool reported subdivision as an eleven-fold
improvement (0.11% wrong, 0.32% worst frame). On hardware the same change made
the artefact visibly *worse*. The corrected tool agrees with the hardware. The
docstring of `sortsim.py` now carries this warning at the top.

## What the mesh actually is

`src/ui/objects/rom.obj` is a crude microchip, and connected-component
analysis says so plainly: **15 disjoint pieces**, each convex. One body
(62.5 x 7.8 x 31.2) and fourteen legs (3.9 x 7.8 x 3.9).

That single fact decides everything below. A convex solid can never sort
wrongly against *itself* - back-face culling already removes the half that
would - so every wrong pixel in this model is one piece drawn against another
piece. Splitting faces cannot help with that, because the pieces were never
the problem within themselves.

## Measured, with the corrected tool

Unmodified mesh, 90 faces, 45 visible at peak (450 words):

| sorting key | wrong pixels | worst frame |
| --- | --- | --- |
| per face, average Z (the old behaviour) | 5.35% | 8.33% |
| per face, nearest Z | 10.95% | 21.73% |
| per face, farthest Z | 4.90% | 7.62% |
| **per piece, centroid** | **4.56%** | **6.30%** |
| per piece, nearest Z | 10.95% | 21.73% |

And with `tools/subdivide.py --ratio 8` applied (136 faces, 680 words):

| sorting key | wrong pixels | worst frame |
| --- | --- | --- |
| per face, average Z | 6.54% | 11.40% |
| per piece, centroid | 4.56% | 6.30% |

**Subdivision makes per-face sorting worse**, not better: 5.35% -> 6.54%, and
8.33% -> 11.40% in the worst frame. More faces means more near-coplanar
neighbours landing in the same bucket, where the drawing order is whatever the
allocation order happened to be. It costs 230 extra words to be worse.

Per-piece sorting is unaffected by subdivision, which is the tell: the pieces
are what is being ordered, and how finely each one is chopped up is beside the
point.

## First attempt: sort per piece

`tinromModelParse` runs a union-find over the mesh's edges and labels every
face with the connected component it belongs to (`findParts()`), and
`tinromModelDraw` gives each piece one bucket, assigned in depth order from a
GTE-transformed centroid.

That is a real improvement - 5.35% to 4.51% wrong pixels - and on screen it
changed **nothing anyone could see**. The reason is worth recording.

## Why one depth value per piece is still not enough

A single depth for a piece is only as honest as the piece is small. The body
is 48 units long; its centroid is the middle of that. A leg at the NEAR END of
the body has a centroid nearer than the body's centroid, so it sorts in front
of the body - while in fact the body's near end is standing in front of it.
The leg draws through the body.

That is exactly what a screenshot of the running menu showed: two legs from the
far side, drawn on top of the body's end face. The simulation's blame table
said the same thing in numbers - essentially every wrong pixel was a leg drawn
over piece 0, the body.

## And underneath that, a modelling bug

The first version of `rom.obj` had each leg **sunk half-way into the body**:

```
body        x -31.23..31.23   y 11.71..19.52   z -15.61..15.61
leg         3.90 x 7.81 x 3.90, overlapping the body by 3.90, 3.90, 1.95
```

Interpenetrating solids cannot be ordered by a painter's algorithm at all. A
leg that passes through the body's surface is simultaneously in front of it and
behind it, and no bucket assignment is right. The test that proves it: of the
body's six face planes, **zero** fully separated any of the fourteen legs.

The legs were pulled out flush. That alone improved matters (4.51% to 1.85%)
but again looked identical on screen, because the *kind* of failure was
unchanged. What it did do was make the problem solvable.

## The fix: separating planes, not depth

Two convex solids that do not interpenetrate always have a separating plane,
and where a satellite is stuck to a hull, the hull's own faces are the
candidates. Once a piece is known to lie wholly outside one of them, its order
against the hull is decided by **which side of that plane the camera is on** -
which is precisely what NCLIP already reports about that face. No depth value
takes part, so a hull too long to be described by one cannot mislead it.

At parse time, `findSeparators()`:

1. takes the piece with the largest bounding box as the hull;
2. offers the hull's own faces as candidate planes (up to
   `TINROM_MODEL_MAX_HULL_FACES`), each normal turned outward using the hull's
   centre;
3. for every other piece, keeps the plane it clears by the widest margin, using
   the piece's **bounding-box corners** rather than its vertices - conservative,
   never wrong, and exact for the boxes these meshes are made of;
4. stores one plane ordinal per piece, or `TINROM_MODEL_NO_SEPARATOR`.

At draw time, `tinromModelDraw()` runs RTPT + NCLIP once per candidate plane -
six extra GTE ops for this mesh - and sorts the pieces by:

```
class ascending, then depth descending
  class 0  separated by a back-facing hull plane   -> behind the hull
  class 1  the hull itself
  class 2  separated by a front-facing hull plane  -> in front of the hull
```

Depth only orders pieces *within* a group, where they are small enough for it
to be honest. A piece with no separating plane falls back to comparing its
centroid against the hull's, so a mesh that does not fit this shape degrades to
the previous behaviour rather than breaking.

Buckets are then handed out consecutively - `partBucket[rank[p]] = zFar - p` -
so two pieces at nearly equal depth can never tie on quantisation.

## Measured

Original mesh (legs embedded), 90 faces, 45 visible at peak, 450 words:

| ordering | wrong pixels | worst frame |
| --- | --- | --- |
| per face, average Z | 5.35% | 8.33% |
| per face, nearest Z | 10.95% | 21.73% |
| per face, farthest Z | 4.90% | 7.62% |
| per piece, centroid | 4.51% | 7.08% |
| per piece, separating plane | 4.51% | 7.08% |

Nothing helps while the legs interpenetrate; there is no separating plane to
find, so the last row falls back to the row above it.

Legs pulled out flush:

| ordering | separable | wrong pixels | worst frame |
| --- | --- | --- | --- |
| per face, average Z | - | 2.46% | 5.21% |
| per piece, centroid | - | 1.85% | 4.66% |
| **per piece, separating plane** | **14/14** | **0.00%** | **0.00%** |

Zero, at every angle tested, for six extra GTE operations and no extra packet
words. The C implementation was checked separately against the `.msh` the
console actually loads: 15 pieces, hull correctly identified as piece 0, six
candidate planes, 14 of 14 satellites separated with a clearance of exactly 0.

## What subdivision is still for

`tools/subdivide.py` is kept, but **not wired into the build**:

```cmake
tinromConvertMesh(src/ui/objects/rom.obj src/ui/objects/rom.png assets/rom.msh)
```

That call takes the texture **image** rather than its dimensions, and reads the
size out of the PNG's IHDR at configure time. It used to take two numbers, and
mkmesh scales every UV by `width - 1`, so shrinking `rom.png` from 128x128 to
64x64 without changing them scaled every UV for a texture twice the size - and
separately made `tinromModelUploadTexture` read 16 KB out of a 4 KB array,
because the size it uploads comes from the mesh header. One fact, one place.
A trailing ratio argument still turns subdivision on.

On this mesh it made things worse - 5.35% to 6.54% - for 230 extra words, since
more faces means more near-coplanar neighbours tying in one bucket. It cuts
**edge loops** rather than splitting faces individually, which is worth keeping
for the one case it is right for: a single concave shell, where there are no
pieces to separate. The per-face variant it replaced left **24 T-junctions
across 10 faces and 56 edges without an opposite partner** - the mesh stopped
being closed and the rasteriser left cracks along every seam. A loop cut walks
the ring of opposite edges through adjacent quads and cuts every quad it passes
through, so both sides of every edge always agree, and quads stay quads (10
words, rather than two triangles at 8 each).

## The packet buffer, and why the model draws last

`GPU_CHAIN_BUFFER_SIZE` was 2048 when this was written - 4096 since
ps1-bare-metal 2af848a - and ps1-bare-metal defines it unconditionally, so
tinrom cannot size it without patching ps1-bare-metal - which NOTICE.md
commits to not doing. The main menu's
own worst case is about 1500 words by `uiTextCost`'s reckoning - the same
conservative number `uiHasRoom` uses to decide what to drop.

So the model is drawn **last**, from `drawModel()` in `uiEndFrame` (when no
custom background replaces it), using whatever words the UI did not use -
which `backgroundBudget()` works out:

```c
unsigned int used  = (unsigned int) (ui->chain->nextPacket - ui->chain->data);
unsigned int spare = (used < GPU_CHAIN_BUFFER_SIZE)
	? (GPU_CHAIN_BUFFER_SIZE - used - MODEL_TAIL_MARGIN) : 0;

if (spare > MODEL_WORD_BUDGET)
	spare = MODEL_WORD_BUDGET;
```

The ordering table decides what is in front; the packet buffer decides what gets
drawn at all, and it is first come first served. Those are separate things, and
the model only ever needed the first. Drawing it after the UI means a busy
screen costs polygons instead of menu rows - and a missing menu entry is worse
than a missing polygon. `MODEL_WORD_BUDGET` (500) is a ceiling on GPU time
rather than a reservation.

`uiTextCost` was also tightened to count only glyphs that actually draw,
skipping spaces, tabs and newlines, which recovered 165 words on the main menu.

## Rules this left behind

- **A mesh of separate solids must not interpenetrate.** Where pieces overlap,
  no sorting is correct, and the artefact is not a bug in the renderer.
- **Do not compare a long piece to a short one by depth.** Use a separating
  plane where one exists.
- **Measure against the plane depth at the pixel**, never a face average.
- **Trust a screenshot over the simulation** when they disagree. They have
  disagreed twice, and the simulation was wrong both times.

**None of this has run on real hardware yet** - it is the corrected simulation
plus emulator screenshots.
