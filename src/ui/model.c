/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PS1M model loading and drawing. The GTE transform/cull/sort loop follows
 * ps1-bare-metal's examples 08 and 10 (MIT, spicyjpeg); see NOTICE.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/gpu.h"
#include "common/gte.h"
#include "core/log.h"
#include "core/result.h"
#include "hal/video.h"
#include "ps1/gpucmd.h"
#include "libc/string.h"
#include "ps1/gte.h"
#include "ui/model.h"

void tinromGteScreenMatrix(void) {
	/*
	 * The rotations that follow multiply on the right, so this doubling
	 * applies after them, in screen space. 2.0 is 8192 in the GTE's 4.12,
	 * well inside a matrix entry's 16 bits.
	 */
	gte_setRotationMatrix(
		GTE_UNIT * TINROM_SCREEN_SCALE_X, 0,        0,
		                               0, GTE_UNIT, 0,
		                               0, 0,        GTE_UNIT
	);
}

/* On-disk header, exactly as tools/mkmesh.py packs it. */
typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint16_t partCount, vertexCount, faceCount;
	uint16_t boxCount, animCount, frameCount;
	uint16_t textureWidth, textureHeight;
} ModelHeader;

#define MODEL_HEADER_SIZE 24
#define MODEL_PART_SIZE   18
#define MODEL_VERT_SIZE    8
#define MODEL_FACE_SIZE   16

/*
 * The offsets below are computed by walking these sizes, so if the compiler
 * ever pads one of these structs the parser silently reads the wrong bytes.
 * ps1port/mesh.h asserts the same sizes on its side; they and the
 * struct.pack formats in tools/mkmesh.py have to stay in step.
 */
_Static_assert(sizeof(ModelHeader)       == MODEL_HEADER_SIZE, "PS1M header is 24 bytes");
_Static_assert(sizeof(TinromModelVertex) == MODEL_VERT_SIZE,   "PS1M vertex is 8 bytes");
_Static_assert(sizeof(TinromModelFace)   == MODEL_FACE_SIZE,   "PS1M face is 16 bytes");

/*
 * Which sign of the GTE's shoelace result is a front face.
 *
 * mkmesh.py writes +x right, +y down, +z away from the camera, and under that
 * convention a face pointing at the camera comes out NEGATIVE. The ps1-python
 * renderer these meshes were authored for culls on exactly that
 * (scene.c: `if (s_cull == SCENE_CULL_BACK && cross >= 0) continue;`), and
 * its cross product is algebraically identical to what GTE NCLIP leaves in
 * MAC0 - so tinrom keeps negative areas to match.
 *
 * Note this is the opposite of ps1-bare-metal's example 08, whose cube is
 * wound the other way. Neither is wrong; the exporter decides, and ours is
 * mkmesh.py.
 */
#define MODEL_FRONT_FACE_AREA_POSITIVE 0

/* Neutral modulation: the texture's own colours pass through unchanged. */
#define MODEL_NEUTRAL_COLOR gp0_rgb(128, 128, 128)

/*
 * setupGTE() scales the ordering table for a whole scene: ZSF4 is
 * GPU_ORDERING_TABLE_SIZE/4, which spreads depths of roughly 0..4096 across
 * the table. A single decorative model sits at a couple of hundred units, so
 * every one of its faces would land in the first handful of buckets - below
 * the range reserved for it, and therefore dropped entirely.
 *
 * Setting ZSF4 to 4096/4 makes OTZ the plain average of the four projected Z
 * values, in model units. tinrom then maps that onto the bucket range itself,
 * which gives the model the full spread regardless of how far away it sits.
 */
#define MODEL_ZSF3 (4096 / 3)
#define MODEL_ZSF4 (4096 / 4)

/*
 * Vertices are handed to the GTE with lwc2, which faults on anything that is
 * not 4-byte aligned - and a PS1M blob does not guarantee that. The header is
 * 24 bytes and a part is 18, so an odd part count leaves the vertex array on
 * a 2-byte boundary: a one-part mesh puts it at offset 42, and the GTE load
 * takes a LoadAddressError on the first vertex.
 *
 * The format is shared with the ps1-python engine, so tinrom does not change
 * it. Instead the vertex array is realigned here, once, at parse time - and
 * only when it actually needs it, so a blob that happens to be aligned is
 * still used in place with no copy.
 */
static TinromModelVertex alignedVertices[TINROM_MODEL_MAX_VERTICES]
	__attribute__((aligned(4)));

/*
 * Which connected piece each face belongs to, worked out once at parse time.
 * A mesh of separate convex solids sorts far better one bucket per piece than
 * one bucket per face: inside a convex solid the visible faces never overlap
 * each other, so their order does not matter at all, and what is left is the
 * much easier problem of ordering a handful of disjoint solids.
 */
static uint8_t  facePart[TINROM_MODEL_MAX_FACES];
static uint16_t hullFaces[TINROM_MODEL_MAX_HULL_FACES];
static uint8_t  partSeparator[TINROM_MODEL_MAX_PARTS];
static int32_t  partBox[TINROM_MODEL_MAX_PARTS][6];
static unsigned int separatorHull, separatorPlanes;
static uint16_t vertexParent[TINROM_MODEL_MAX_VERTICES];

static uint16_t findRoot(uint16_t index) {
	while (vertexParent[index] != index) {
		vertexParent[index] = vertexParent[vertexParent[index]];
		index = vertexParent[index];
	}

	return index;
}

/*
 * Union-find over "these two vertices are corners of the same face". Vertices
 * are already shared between faces that meet, so this finds exactly the
 * separate shells without any geometric comparison.
 *
 * Returns the number of pieces, or 0 if the mesh is too big to group or comes
 * out as a single piece - in which case the caller falls back to sorting
 * faces one at a time.
 */
static unsigned int findParts(TinromModel *model) {
	if ((model->faceCount > TINROM_MODEL_MAX_FACES)
		|| (model->vertexCount > TINROM_MODEL_MAX_VERTICES))
		return 0;

	for (unsigned int i = 0; i < model->vertexCount; i++)
		vertexParent[i] = (uint16_t) i;

	for (unsigned int i = 0; i < model->faceCount; i++) {
		const TinromModelFace *face = &model->faces[i];
		uint16_t               root = findRoot(face->v[0]);

		for (int corner = 1; corner < 4; corner++) {
			uint16_t index = face->v[corner];

			if ((corner == 3) && (index == TINROM_MODEL_TRIANGLE))
				continue;

			uint16_t other = findRoot(index);

			if (other != root)
				vertexParent[other] = root;
		}
	}

	/* Number the roots in the order they are first seen. */
	unsigned int count = 0;
	uint16_t     label[TINROM_MODEL_MAX_PARTS];

	for (unsigned int i = 0; i < model->faceCount; i++) {
		uint16_t     root  = findRoot(model->faces[i].v[0]);
		unsigned int part  = 0;
		bool         found = false;

		for (part = 0; part < count; part++) {
			if (label[part] == root) {
				found = true;
				break;
			}
		}

		if (!found) {
			if (count >= TINROM_MODEL_MAX_PARTS)
				return 0;

			label[count] = root;
			part = count++;
		}

		facePart[i] = (uint8_t) part;
	}

	return (count > 1) ? count : 0;
}

/*
 * Give every piece a plane that separates it from the hull.
 *
 * Sorting by one depth value per piece is only as good as that value, and a
 * long hull has no single honest depth: a satellite at the near END of it can
 * sit nearer than the hull's centre while still being behind the hull's near
 * face. That is the "legs showing through the body" artefact, and no scalar
 * fixes it.
 *
 * Two convex solids that do not interpenetrate always have a separating
 * plane, and for a hull with satellites stuck to it the hull's own faces are
 * the candidates. Once a piece is known to lie outside one of them, its order
 * against the hull is decided by which side of that plane the camera is on -
 * exactly what NCLIP reports about that face - and no depth is involved.
 *
 * A piece's bounding box stands in for the piece: a box outside the plane
 * means the piece is too. That is conservative, never wrong, and exact for
 * the boxes these meshes are actually made of.
 *
 * Returns how many pieces were separated; 0 means the caller keeps sorting
 * by centroid depth alone.
 */
/*
 * Limits that keep every product below in 32 bits.
 *
 * The obvious way to write the plane maths is in int64_t, and the first
 * version did. On this toolchain that cost THREE KILOBYTES of inlined 64-bit
 * helper code for a function that runs once at parse time - enough, on its
 * own, to push tinrom past the fifteen blocks a memory card image gets. So
 * the vectors are shifted down until int32 is provably enough instead.
 *
 * An edge of 1024 gives a normal under 2*1024*1024, and a normal of 2048
 * against a coordinate difference of 131070 gives 8.0e8, comfortably inside
 * a signed 32-bit int. The shifting costs precision near zero, which does not
 * matter: a piece sitting flush against a plane gives exactly zero either way,
 * and that is the case the test is really about.
 */
#define MODEL_EDGE_LIMIT   1024
#define MODEL_NORMAL_LIMIT 2048

/* Halve until every component fits. Arithmetic shift on a negative value is
 * what every compiler tinrom targets does, and the sign has to survive. */
static void reduceVector(int32_t *v, int32_t limit) {
	for (;;) {
		int32_t peak = 0;

		for (int k = 0; k < 3; k++) {
			int32_t magnitude = (v[k] < 0) ? -v[k] : v[k];

			if (magnitude > peak)
				peak = magnitude;
		}

		if (peak <= limit)
			return;

		for (int k = 0; k < 3; k++)
			v[k] >>= 1;
	}
}

static unsigned int findSeparators(TinromModel *model) {
	unsigned int parts = model->partCount;

	separatorHull = separatorPlanes = 0;

	if ((parts < 2) || (parts > TINROM_MODEL_MAX_PARTS))
		return 0;

	for (unsigned int p = 0; p < parts; p++) {
		partBox[p][0] = partBox[p][1] = partBox[p][2] = INT32_MAX;
		partBox[p][3] = partBox[p][4] = partBox[p][5] = INT32_MIN;
		partSeparator[p] = TINROM_MODEL_NO_SEPARATOR;
	}

	for (unsigned int i = 0; i < model->faceCount; i++) {
		unsigned int p = facePart[i];

		for (int corner = 0; corner < 4; corner++) {
			uint16_t index = model->faces[i].v[corner];

			if ((corner == 3) && (index == TINROM_MODEL_TRIANGLE))
				continue;

			const TinromModelVertex *vertex = &model->vertices[index];
			int32_t                  c[3]   = { vertex->x, vertex->y, vertex->z };

			for (int k = 0; k < 3; k++) {
				if (c[k] < partBox[p][k])
					partBox[p][k] = c[k];
				if (c[k] > partBox[p][3 + k])
					partBox[p][3 + k] = c[k];
			}
		}
	}

	/*
	 * The hull is the piece with the largest bounding box, measured by the
	 * sum of its squared dimensions rather than its volume: it separates a
	 * body from its legs just as well, and unlike a volume it cannot overflow
	 * 32 bits.
	 */
	uint32_t largest = 0;

	for (unsigned int p = 0; p < parts; p++) {
		uint32_t extent = 0;

		for (int k = 0; k < 3; k++) {
			int32_t side = partBox[p][3 + k] - partBox[p][k];

			if (side > 32767)
				side = 32767;

			extent += (uint32_t) (side * side);
		}

		if (extent > largest) {
			largest       = extent;
			separatorHull = p;
		}
	}

	for (unsigned int i = 0; (i < model->faceCount)
		&& (separatorPlanes < TINROM_MODEL_MAX_HULL_FACES); i++) {
		if (facePart[i] == separatorHull)
			hullFaces[separatorPlanes++] = (uint16_t) i;
	}

	/* The hull's centre, used only to point each plane normal outwards. */
	int32_t centre[3] = { 0, 0, 0 };
	int32_t corners   = 0;

	for (unsigned int k = 0; k < separatorPlanes; k++) {
		const TinromModelFace *face = &model->faces[hullFaces[k]];

		for (int corner = 0; corner < 4; corner++) {
			uint16_t index = face->v[corner];

			if ((corner == 3) && (index == TINROM_MODEL_TRIANGLE))
				continue;

			centre[0] += model->vertices[index].x;
			centre[1] += model->vertices[index].y;
			centre[2] += model->vertices[index].z;
			corners++;
		}
	}

	if (!corners)
		return 0;

	for (int k = 0; k < 3; k++)
		centre[k] /= corners;

	unsigned int separated = 0;

	for (unsigned int p = 0; p < parts; p++) {
		int32_t clearest = 0;
		bool    found    = false;

		if (p == separatorHull)
			continue;

		for (unsigned int k = 0; k < separatorPlanes; k++) {
			const TinromModelFace   *face = &model->faces[hullFaces[k]];
			const TinromModelVertex *a    = &model->vertices[face->v[0]];
			const TinromModelVertex *b    = &model->vertices[face->v[1]];
			const TinromModelVertex *c    = &model->vertices[face->v[2]];

			int32_t u[3] = { b->x - a->x, b->y - a->y, b->z - a->z };
			int32_t w[3] = { c->x - a->x, c->y - a->y, c->z - a->z };

			reduceVector(u, MODEL_EDGE_LIMIT);
			reduceVector(w, MODEL_EDGE_LIMIT);

			int32_t n[3] = {
				(u[1] * w[2]) - (u[2] * w[1]),
				(u[2] * w[0]) - (u[0] * w[2]),
				(u[0] * w[1]) - (u[1] * w[0])
			};

			reduceVector(n, MODEL_NORMAL_LIMIT);

			int32_t inward = n[0] * (centre[0] - a->x)
				+ n[1] * (centre[1] - a->y)
				+ n[2] * (centre[2] - a->z);

			if (inward > 0) {
				n[0] = -n[0];
				n[1] = -n[1];
				n[2] = -n[2];
			}

			/* The bounding-box corner that sits deepest inside the plane. */
			int32_t worst = 0;

			for (int corner = 0; corner < 8; corner++) {
				int32_t v[3] = {
					partBox[p][(corner & 1) ? 3 : 0],
					partBox[p][(corner & 2) ? 4 : 1],
					partBox[p][(corner & 4) ? 5 : 2]
				};
				int32_t d = n[0] * (v[0] - a->x)
					+ n[1] * (v[1] - a->y)
					+ n[2] * (v[2] - a->z);

				if (!corner || (d < worst))
					worst = d;
			}

			if ((worst >= 0) && (!found || (worst > clearest))) {
				clearest         = worst;
				partSeparator[p] = (uint8_t) k;
				found            = true;
			}
		}

		if (found)
			separated++;
	}

	return separated;
}

/* Exact integer square root, bit by bit. Runs once, at parse time. */
static unsigned int integerSqrt(uint32_t value) {
	uint32_t result = 0;
	uint32_t bit    = 1u << 30;

	while (bit > value)
		bit >>= 2;

	while (bit) {
		if (value >= (result + bit)) {
			value  -= result + bit;
			result  = (result >> 1) + bit;
		} else {
			result >>= 1;
		}

		bit >>= 2;
	}

	return result;
}

TinromResult tinromModelParse(
	const void *data, size_t length, TinromModel *model
) {
	const uint8_t *bytes = (const uint8_t *) data;

	model->valid = false;

	if (!data || length < MODEL_HEADER_SIZE)
		return TINROM_ERR_BAD_ARG;

	const ModelHeader *header = (const ModelHeader *) data;

	if (header->magic != TINROM_MODEL_MAGIC)
		return TINROM_ERR_CORRUPT;
	if ((header->version < TINROM_MODEL_VERSION_MIN)
		|| (header->version > TINROM_MODEL_VERSION_MAX)) {
		TINROM_LOG("model: version %d, tinrom reads %d..%d",
			(int) header->version,
			TINROM_MODEL_VERSION_MIN, TINROM_MODEL_VERSION_MAX);

		return TINROM_ERR_UNSUPPORTED;
	}

	/*
	 * Walk the sections in order rather than trusting any single offset. Parts
	 * are skipped: tinrom draws the whole mesh as one rigid object.
	 */
	size_t offset = MODEL_HEADER_SIZE;

	offset += (size_t) header->partCount * MODEL_PART_SIZE;

	/*
	 * PS1M v2 pads here so the vertex array is 4-byte aligned. A part is 18
	 * bytes, so an odd part count would otherwise leave the vertices on a
	 * 2-byte boundary - and lwc2, which is how they reach the GTE, faults on
	 * that. Every later section is a multiple of 4 bytes, so this one pad
	 * aligns all of them.
	 *
	 * v1 has no pad. Rather than refuse it, the realignment below copies the
	 * vertices somewhere aligned, so a blob from either exporter works.
	 */
	if (header->version >= 2)
		offset = (offset + 3) & ~(size_t) 3;

	size_t vertexOffset = offset;

	offset += (size_t) header->vertexCount * MODEL_VERT_SIZE;

	size_t faceOffset = offset;

	offset += (size_t) header->faceCount * MODEL_FACE_SIZE;

	if (offset > length) {
		TINROM_LOG("model: blob is %d bytes, needs %d",
			(int) length, (int) offset);

		return TINROM_ERR_CORRUPT;
	}

	const uint8_t *vertexData = bytes + vertexOffset;

	/*
	 * v2 blobs are aligned by construction, so this is a belt-and-braces
	 * path: it also covers a blob that was embedded at an odd address.
	 */
	if (!((uintptr_t) vertexData & 3)) {
		model->vertices = (const TinromModelVertex *) vertexData;
	} else {
		if (header->vertexCount > TINROM_MODEL_MAX_VERTICES) {
			TINROM_LOG("model: %d vertices need realigning, cap is %d",
				(int) header->vertexCount, TINROM_MODEL_MAX_VERTICES);

			return TINROM_ERR_NO_SPACE;
		}

		memcpy(
			alignedVertices, vertexData,
			(size_t) header->vertexCount * MODEL_VERT_SIZE
		);
		model->vertices = alignedVertices;

		TINROM_LOG("model: vertices at +%d are 4-byte misaligned, realigned",
			(int) vertexOffset);
	}

	/* Faces are only ever read as uint16 and uint8, so 2-byte alignment -
	 * which the layout does guarantee - is enough for them. */
	model->faces       = (const TinromModelFace *) (bytes + faceOffset);
	model->vertexCount = header->vertexCount;
	model->faceCount   = header->faceCount;

	model->textureWidth  = header->textureWidth;
	model->textureHeight = header->textureHeight;

	/*
	 * Bounding radius, used to spread the model's faces across its slice of
	 * the ordering table. It is exactly the bound we want: however the model
	 * is rotated, no vertex is further than this from the centre, so the
	 * depth slab is centre +/- radius and the full bucket range gets used.
	 */
	model->radius = 1;

	/*
	 * Every index is dereferenced once per frame forever after, so it is
	 * worth paying for the check once here rather than reading rubbish
	 * vertices for the life of the program.
	 */
	for (unsigned int i = 0; i < model->faceCount; i++) {
		const TinromModelFace *face = &model->faces[i];

		for (int corner = 0; corner < 4; corner++) {
			uint16_t index = face->v[corner];

			if ((corner == 3) && (index == TINROM_MODEL_TRIANGLE))
				continue;
			if (index >= model->vertexCount) {
				TINROM_LOG("model: face %d corner %d indexes vertex %d of %d",
					(int) i, corner, (int) index, (int) model->vertexCount);

				return TINROM_ERR_CORRUPT;
			}
		}
	}

	uint32_t furthest = 0;

	for (unsigned int i = 0; i < model->vertexCount; i++) {
		const TinromModelVertex *vertex = &model->vertices[i];
		/* int16 components, so the sum of three squares needs 32 unsigned
		 * bits at the extreme and cannot overflow one. */
		uint32_t squared = (uint32_t) ((int32_t) vertex->x * vertex->x)
			+ (uint32_t) ((int32_t) vertex->y * vertex->y)
			+ (uint32_t) ((int32_t) vertex->z * vertex->z);

		if (squared > furthest)
			furthest = squared;
	}

	if (furthest)
		model->radius = (int) integerSqrt(furthest);

	model->partCount = findParts(model);
	model->facePart  = model->partCount ? facePart : NULL;

	unsigned int separated = model->partCount ? findSeparators(model) : 0;

	model->hullPart      = separatorHull;
	model->hullFaceCount = separated ? separatorPlanes : 0;
	model->hullFaces     = separated ? hullFaces : NULL;
	model->partSeparator = separated ? partSeparator : NULL;

	if (model->partCount)
		TINROM_LOG("model: %u face(s) in %u piece(s), %u separated by hull planes",
			model->faceCount, model->partCount, separated);

	model->valid = true;

	return TINROM_OK;
}

void tinromModelUploadTexture(
	TinromModel  *model,
	const void   *pixels,
	const void   *palette,
	unsigned int  imageX,
	unsigned int  imageY,
	unsigned int  paletteX,
	unsigned int  paletteY,
	GP0ColorDepth colorDepth
) {
	uploadIndexedTexture(
		&model->texture,
		pixels,
		palette,
		imageX,
		imageY,
		paletteX,
		paletteY,
		model->textureWidth,
		model->textureHeight,
		colorDepth
	);
}

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
) {
	if (!model || !model->valid)
		return 0;

	/*
	 * Faces are skipped rather than drawn partially once the frame's packet
	 * buffer runs low - the UI in front of the model matters more than the
	 * last few polygons behind it.
	 */
	unsigned int used  = (unsigned int) (chain->nextPacket - chain->data);
	unsigned int limit = used + wordBudget;

	if (limit > GPU_CHAIN_BUFFER_SIZE)
		limit = GPU_CHAIN_BUFFER_SIZE;

	/*
	 * Rebuild the rotation from scratch each frame: composing in place
	 * would accumulate rounding error over minutes on a menu screen.
	 */
	tinromGteScreenMatrix();
	rotateCurrentMatrixX(angleX);
	rotateCurrentMatrixY(angleY);
	rotateCurrentMatrixZ(angleZ);

	/*
	 * Position by moving the projection centre rather than translating the
	 * model. OFX/OFY are where the camera axis lands on screen, in 16.16
	 * fixed-point pixels, so this places the origin exactly - and unlike a
	 * TRX/TRY translation it does not interact with distance, which leaves
	 * "where it is" and "how big it is" as two independent knobs.
	 *
	 * setupGTE() put these at the screen centre; nothing else in tinrom uses
	 * the GTE, so overriding them per draw is free.
	 */
	gte_setControlReg(GTE_OFX, screenX << 16);
	gte_setControlReg(GTE_OFY, screenY << 16);

	gte_setControlReg(GTE_TRX, 0);
	gte_setControlReg(GTE_TRY, 0);
	gte_setControlReg(GTE_TRZ, distance);

	gte_setControlReg(GTE_ZSF3, MODEL_ZSF3);
	gte_setControlReg(GTE_ZSF4, MODEL_ZSF4);

	/*
	 * The slab of depth the model can occupy at any rotation. Anything that
	 * still falls outside clamps to the ends rather than vanishing.
	 */
	int          halfSpan   = model->radius;
	int          nearPlane  = distance - halfSpan;
	int          spanUnits  = halfSpan * 2;
	unsigned int spanBuckets = zFar - zNear;

	/*
	 * If the mesh came apart into separate pieces, sort those instead of
	 * sorting faces. Each piece gets its own bucket, assigned in depth order,
	 * so two pieces can never collide and their relative order is exact. The
	 * faces inside a piece then need no ordering at all, which is the whole
	 * point: a convex solid's visible faces do not overlap one another.
	 */
	unsigned int partBucket[TINROM_MODEL_MAX_PARTS];
	int          partDepth[TINROM_MODEL_MAX_PARTS];
	uint8_t      partClass[TINROM_MODEL_MAX_PARTS];
	bool         byPart = (model->partCount > 1)
		&& (model->partCount <= (zFar - zNear));
	bool         bySeparator = byPart && model->partSeparator
		&& model->hullFaceCount;

	if (byPart) {
		int sum[TINROM_MODEL_MAX_PARTS];
		int corners[TINROM_MODEL_MAX_PARTS];

		for (unsigned int p = 0; p < model->partCount; p++) {
			sum[p]     = 0;
			corners[p] = 0;
		}

		/*
		 * Each piece's centre, in view space. Transforming one point per
		 * piece would be cheaper, but the vertices are already going through
		 * the GTE below and the rotation matrix is linear - so the mean of
		 * the rotated corners is the rotated mean, and this needs no extra
		 * GTE traffic at all.
		 */
		for (unsigned int i = 0; i < model->faceCount; i++) {
			const TinromModelFace *face = &model->faces[i];
			unsigned int           p    = model->facePart[i];

			for (int corner = 0; corner < 4; corner++) {
				uint16_t index = face->v[corner];

				if ((corner == 3) && (index == TINROM_MODEL_TRIANGLE))
					continue;

				gte_loadV0((const GTEVector16 *) &model->vertices[index]);
				gte_command(GTE_CMD_RTPS | GTE_SF);

				sum[p] += (int) gte_getDataReg(GTE_SZ3);
				corners[p]++;
			}
		}

		for (unsigned int p = 0; p < model->partCount; p++)
			partDepth[p] = corners[p] ? (sum[p] / corners[p]) : distance;

		/*
		 * Where a separating plane exists, it - not the depth - decides the
		 * order against the hull. A hull face that is front-facing has the
		 * camera on its outside, so everything separated by it is in front of
		 * the hull; everything separated by a back-facing one is behind it.
		 * Depth then only orders pieces within those two groups, where they
		 * are small enough for it to be honest.
		 */
		for (unsigned int p = 0; p < model->partCount; p++)
			partClass[p] = 1;

		if (bySeparator) {
			bool front[TINROM_MODEL_MAX_HULL_FACES];

			for (unsigned int k = 0; k < model->hullFaceCount; k++) {
				const TinromModelFace *face =
					&model->faces[model->hullFaces[k]];

				gte_loadV0((const GTEVector16 *) &model->vertices[face->v[0]]);
				gte_loadV1((const GTEVector16 *) &model->vertices[face->v[1]]);
				gte_loadV2((const GTEVector16 *) &model->vertices[face->v[2]]);
				gte_command(GTE_CMD_RTPT | GTE_SF);
				gte_command(GTE_CMD_NCLIP);

				int area = (int) gte_getDataReg(GTE_MAC0);

#if MODEL_FRONT_FACE_AREA_POSITIVE
				front[k] = (area > 0);
#else
				front[k] = (area < 0);
#endif
			}

			unsigned int hull = model->hullPart;

			for (unsigned int p = 0; p < model->partCount; p++) {
				uint8_t k = model->partSeparator[p];

				if (p == hull)
					partClass[p] = 1;
				else if (k == TINROM_MODEL_NO_SEPARATOR)
					partClass[p] =
						(partDepth[p] > partDepth[hull]) ? 0 : 2;
				else
					partClass[p] = front[k] ? 2 : 0;
			}
		}

		/*
		 * Furthest piece to the nearest, taking the highest buckets first -
		 * a higher bucket is sent earlier and so ends up further back.
		 * Insertion sort: there are at most a few dozen pieces and this runs
		 * once a frame.
		 */
		unsigned int rank[TINROM_MODEL_MAX_PARTS];

		for (unsigned int p = 0; p < model->partCount; p++) {
			unsigned int at = p;

			while (at) {
				unsigned int q = rank[at - 1];
				/* Behind the hull, then the hull, then in front of it; and
				 * within a group, furthest first. */
				bool         before = (partClass[p] != partClass[q])
					? (partClass[p] < partClass[q])
					: (partDepth[p] > partDepth[q]);

				if (!before)
					break;

				rank[at] = q;
				at--;
			}

			rank[at] = p;
		}

		for (unsigned int p = 0; p < model->partCount; p++)
			partBucket[rank[p]] = zFar - p;
	}

	unsigned int drawn = 0;

	for (unsigned int i = 0; i < model->faceCount; i++) {
		const TinromModelFace *face = &model->faces[i];

		/*
		 * The GTE transforms three vertices at a time, so a quad takes two
		 * passes: RTPT for the first three, then RTPS for the fourth.
		 */
		gte_loadV0((const GTEVector16 *) &model->vertices[face->v[0]]);
		gte_loadV1((const GTEVector16 *) &model->vertices[face->v[1]]);
		gte_loadV2((const GTEVector16 *) &model->vertices[face->v[2]]);
		gte_command(GTE_CMD_RTPT | GTE_SF);

		/* Shoelace area of the projected triangle: its sign is the winding,
		 * which is how a back face is recognised without a normal. */
		gte_command(GTE_CMD_NCLIP);

		int area = (int) gte_getDataReg(GTE_MAC0);

#if MODEL_FRONT_FACE_AREA_POSITIVE
		if (area <= 0)
			continue;
#else
		/* Zero area is a face edge-on to the camera: nothing to draw. */
		if (area >= 0)
			continue;
#endif

		bool isTriangle = (face->v[3] == TINROM_MODEL_TRIANGLE);

		/* Projecting the fourth vertex pushes the first out of the GTE's
		 * three-entry queue, so save it first. */
		uint32_t xy0 = gte_getDataReg(GTE_SXY0);

		if (!isTriangle) {
			gte_loadV0((const GTEVector16 *) &model->vertices[face->v[3]]);
			gte_command(GTE_CMD_RTPS | GTE_SF);
			gte_command(GTE_CMD_AVSZ4 | GTE_SF);
		} else {
			gte_command(GTE_CMD_AVSZ3 | GTE_SF);
		}

		/* With ZSF4 as set above this is the average Z in model units. */
		int depth = (int) gte_getDataReg(GTE_OTZ) - nearPlane;

		if (depth < 0)
			depth = 0;
		if (depth > spanUnits)
			depth = spanUnits;

		unsigned int zIndex;

		if (byPart) {
			zIndex = partBucket[model->facePart[i]];
		} else {
			zIndex = zNear
				+ ((unsigned int) depth * spanBuckets)
					/ (unsigned int) spanUnits;

			if (zIndex > zFar)
				zIndex = zFar;
		}

		unsigned int cost = isTriangle ? 8 : 10;

		if ((used + cost + 2) >= limit)
			break;

		used += cost;

		if (isTriangle) {
			uint32_t *ptr = allocateOrderedGP0Packet(chain, zIndex, 7);

			ptr[0] = MODEL_NEUTRAL_COLOR | gp0_triangle(true, false);
			ptr[1] = xy0;
			ptr[2] = gp0_uv(
				model->texture.u + face->uv[0],
				model->texture.v + face->uv[1],
				model->texture.clut
			);
			gte_storeDataReg(GTE_SXY1, &ptr[3]);
			ptr[4] = gp0_uv(
				model->texture.u + face->uv[2],
				model->texture.v + face->uv[3],
				model->texture.page
			);
			gte_storeDataReg(GTE_SXY2, &ptr[5]);
			ptr[6] = gp0_uv(
				model->texture.u + face->uv[4],
				model->texture.v + face->uv[5],
				0
			);
		} else {
			uint32_t *ptr = allocateOrderedGP0Packet(chain, zIndex, 9);

			ptr[0] = MODEL_NEUTRAL_COLOR | gp0_quad(true, false);
			ptr[1] = xy0;
			ptr[2] = gp0_uv(
				model->texture.u + face->uv[0],
				model->texture.v + face->uv[1],
				model->texture.clut
			);
			gte_storeDataReg(GTE_SXY0, &ptr[3]);
			ptr[4] = gp0_uv(
				model->texture.u + face->uv[2],
				model->texture.v + face->uv[3],
				model->texture.page
			);
			gte_storeDataReg(GTE_SXY1, &ptr[5]);
			ptr[6] = gp0_uv(
				model->texture.u + face->uv[4],
				model->texture.v + face->uv[5],
				0
			);
			gte_storeDataReg(GTE_SXY2, &ptr[7]);
			ptr[8] = gp0_uv(
				model->texture.u + face->uv[6],
				model->texture.v + face->uv[7],
				0
			);
		}

		drawn++;
	}

	return drawn;
}
