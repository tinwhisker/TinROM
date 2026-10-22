/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/gpu.h"
#include "core/result.h"
#include "ps1/gpucmd.h"

/*
 * PS1M models.
 *
 * The format is the one tools/mkmesh.py writes; ps1port/mesh.h in the
 * ps1-python project is its specification, and this is a reader for the
 * subset tinrom needs. See NOTICE.md.
 *
 * tinrom draws one static textured model and nothing else, so the part
 * hierarchy, animations and collision boxes in the format are parsed past and
 * ignored. If tinrom ever wants an animated model, that is where to start.
 *
 * Nothing is copied: the model points into the blob, which is embedded in the
 * executable and therefore outlives everything.
 */

/*
 * Cap on vertices tinrom will realign; see the note on alignment in model.c.
 * Only spent when the blob's vertex array is not already 4-byte aligned.
 */
#define TINROM_MODEL_MAX_VERTICES 512
#define TINROM_MODEL_MAX_FACES    512

/*
 * Above this, grouping is not worth it: the ordering table only has so many
 * buckets to give the model, and a mesh in hundreds of pieces is not the kind
 * this helps.
 */
#define TINROM_MODEL_MAX_PARTS     48

/*
 * Faces of the hull piece that are offered as separating planes, and the
 * marker for a piece that no plane fully separates.
 */
#define TINROM_MODEL_MAX_HULL_FACES 16
#define TINROM_MODEL_NO_SEPARATOR   0xff

#define TINROM_MODEL_MAGIC    0x4d315350u /* 'PS1M', little-endian */
/*
 * Both versions are read. v2 pads the parts section so the vertex array is
 * 4-byte aligned; v1 does not, and tinrom realigns it on load instead. See
 * the notes in model.c.
 */
#define TINROM_MODEL_VERSION_MIN 1
#define TINROM_MODEL_VERSION_MAX 2
#define TINROM_MODEL_TRIANGLE 0xffffu     /* face.v[3] on a three-sided face */

/*
 * Laid out to match GTEVector16 exactly - {int16 x, y, z, pad}, 4-byte
 * aligned - so vertices are handed to the GTE without copying or converting.
 */
typedef struct __attribute__((aligned(4))) {
	int16_t x, y, z, pad;
} TinromModelVertex;

typedef struct {
	/*
	 * Already in the GPU's "Z" order rather than perimeter order - mkmesh.py
	 * does that swap at export time - so these four map straight onto the
	 * four corners of a GP0 quad.
	 */
	uint16_t v[4];
	uint8_t  uv[8]; /* u, v per corner, in texels */
} TinromModelFace;

typedef struct {
	const TinromModelVertex *vertices;
	const TinromModelFace   *faces;
	unsigned int             vertexCount;
	unsigned int             faceCount;
	uint16_t                 textureWidth, textureHeight;
	/* Bounding-sphere radius; the depth mapping is scaled by it. */
	int                      radius;
	/*
	 * Connected pieces, found at parse time. A mesh of separate convex
	 * solids - a chip body with its legs, say - sorts far better one bucket
	 * per piece than one bucket per face, because inside a convex solid the
	 * front faces never overlap each other and so never need sorting at all.
	 * 0 or 1 means there is nothing to group and faces are sorted
	 * individually. See docs/model-sorting.md.
	 */
	unsigned int             partCount;
	const uint8_t           *facePart;
	/*
	 * Exact ordering for a hull-and-satellites mesh. A convex satellite that
	 * lies wholly outside one of the hull's own face planes is separated from
	 * it by that plane: it is in front of the hull exactly when the camera is
	 * on its side of that plane, which is the question face culling already
	 * answers. No depth value is compared, so a hull too long to be described
	 * by one cannot fool it. hullFaceCount is 0 when nothing was separable.
	 */
	unsigned int             hullPart;
	unsigned int             hullFaceCount;
	const uint16_t          *hullFaces;
	const uint8_t           *partSeparator;
	TextureInfo              texture;
	bool                     valid;
} TinromModel;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Point `model` at the contents of `data`. Validates the magic, the version
 * and every offset and vertex index against `length`, so a truncated or
 * mismatched blob is refused rather than read off the end.
 */
TinromResult tinromModelParse(
	const void *data, size_t length, TinromModel *model
);

/** Upload the model's texture and palette to VRAM. */
void tinromModelUploadTexture(
	TinromModel  *model,
	const void   *pixels,
	const void   *palette,
	unsigned int  imageX,
	unsigned int  imageY,
	unsigned int  paletteX,
	unsigned int  paletteY,
	GP0ColorDepth colorDepth
);

/**
 * Loads the GTE's rotation with the display's x-doubling and nothing else:
 * its pixels are half as wide as they are tall (hal/video.h), so anything
 * drawn in 3D needs x doubled or comes out squashed to half its width.
 * Start every frame's rotation here, then turn it with ps1-bare-metal's
 * rotateCurrentMatrixX/Y/Z(), which multiply on the right and so leave the
 * doubling applied last. The model and the CD player's visualiser both do.
 */
void tinromGteScreenMatrix(void);

/**
 * Draw the model into `chain`.
 *
 * `screenX` and `screenY` are where the model's origin lands, in display
 * pixels - 640 across, not layout units; the model is stretched across to
 * match (hal/video.h). They move the projection centre rather than the
 * model, so position and size are independent: changing `distance` scales
 * the model in place instead of sliding it about.
 *
 * `distance` is how far away it sits, in model units, and is therefore what
 * controls apparent size. Keep it comfortably above the model's radius - the
 * GTE's perspective divide overflows once a vertex's Z reaches H/2, where H
 * is half the smaller screen dimension (120 at 640x240).
 *
 * Angles are in ps1-bare-metal's units, where 4096 is a full turn.
 *
 * Faces are bucketed by their average Z between zNear and zFar, so anything
 * already in the chain outside that range stays in front of or behind the
 * whole model.
 *
 * `wordBudget` caps how much of the frame's packet buffer the model may take.
 * It draws first, so without a cap a busy model would starve the menu drawn
 * on top of it - and a missing menu entry matters more than a missing polygon.
 *
 * Returns the number of faces actually drawn - the rest were back-facing,
 * fell outside the depth range, or ran into the budget.
 */
unsigned int tinromModelDraw(
	GPUOrderedDMAChain *chain,
	const TinromModel  *model,
	int                 angleX,
	int                 angleY,
	int                 angleZ,
	int                 screenX,
	int                 screenY,
	int                 distance,
	unsigned int        zNear,
	unsigned int        zFar,
	unsigned int        wordBudget
);

#ifdef __cplusplus
}
#endif
