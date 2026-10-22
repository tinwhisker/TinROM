/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The visualiser in 3D. See scene.h.
 *
 * Everything is a box: a bar of the spectrum, a block of the level meter, a
 * column of the waveform. A box is projected by the GTE a face at a time,
 * the way ui/model.c draws the spinning chip - transform, cull on the
 * winding, sort by depth into the background's buckets - and drawn as flat
 * quads, darker on the sides than on top so it reads as solid. No mesh, no
 * texture.
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "common/gte.h"
#include "common/trig.h"
#include "hal/video.h"
#include "modules/cdplay/scene.h"
#include "ps1/gpucmd.h"
#include "ps1/gte.h"

/*
 * Where it sits and how big it is. The scene sets its own projection plane,
 * further out than setupGTE()'s 120, and stands further back to match: the
 * same size on screen with a quarter of the perspective, so the front of
 * the ring does not loom over the back. A model unit at DISTANCE is then
 * 320/1200 of a layout unit, and the ring about 130 units across. No vertex
 * comes nearer than about 930, far clear of the GTE's overflow at H/2.
 * The plane is put back afterwards: the model (ui/model.c) relies on it.
 */
#define PROJECTION   320
#define SCREEN_X     160 /* units: centred */
#define SCREEN_Y_BIAS 42 /* below the middle, under a short track grid */
#define DISTANCE     1200
#define RING_RADIUS  240

/* Angles in 4096ths of a turn, as everywhere in the GTE code. */
#define RING_TILT    350
#define LEVEL_TILT   150
#define RING_SPIN      6 /* per frame */

#define FACE_WORDS 6 /* a flat quad, five words, and its tag */

/* The depth slab sorted into the background's buckets, around DISTANCE. */
#define DEPTH_NEAR (DISTANCE - RING_RADIUS - 120)
#define DEPTH_SPAN ((RING_RADIUS + 120) * 2)

typedef struct {
	int16_t x0, x1, y0, y1, z0, z1; /* y0 the top: up is -y */
} Box;

typedef struct {
	UIContext   *ui;
	unsigned int used, limit;
} Budget;

/*
 * A box's faces, as corners: bit 0 picks x1, bit 1 y1, bit 2 z1. Each is in
 * the GPU's quad order - two corners of one edge, then the two opposite -
 * and wound so that a face toward the camera comes out with a negative
 * shoelace area from NCLIP, the same as the model's. Checked for every
 * face at every angle the scene turns through.
 */
static const uint8_t faces[6][4] = {
	{ 0, 1, 4, 5 }, /* top    */
	{ 3, 2, 7, 6 }, /* bottom */
	{ 1, 0, 3, 2 }, /* near   */
	{ 4, 5, 6, 7 }, /* far    */
	{ 0, 4, 2, 6 }, /* left   */
	{ 5, 1, 7, 3 }  /* right  */
};

/* How much light each face gets, out of 128: lit from above, a little left. */
static const uint8_t shade[6] = { 128, 40, 96, 72, 84, 60 };

static uint32_t shaded(uint32_t rgb, unsigned int face) {
	unsigned int r = (((rgb >> 16) & 0xff) * shade[face]) >> 7;
	unsigned int g = (((rgb >>  8) & 0xff) * shade[face]) >> 7;
	unsigned int b = (( rgb        & 0xff) * shade[face]) >> 7;

	return gp0_rgb((uint8_t) r, (uint8_t) g, (uint8_t) b);
}

static void corner(const Box *box, unsigned int which, GTEVector16 *out) {
	out->x    = (which & 1) ? box->x1 : box->x0;
	out->y    = (which & 2) ? box->y1 : box->y0;
	out->z    = (which & 4) ? box->z1 : box->z0;
	out->_pad = 0;
}

static void drawBox(Budget *budget, const Box *box, uint32_t rgb) {
	for (unsigned int f = 0; f < 6; f++) {
		GTEVector16 v[4];

		for (unsigned int c = 0; c < 4; c++)
			corner(box, faces[f][c], &v[c]);

		gte_loadV0(&v[0]);
		gte_loadV1(&v[1]);
		gte_loadV2(&v[2]);
		gte_command(GTE_CMD_RTPT | GTE_SF);
		gte_command(GTE_CMD_NCLIP);

		/* Edge-on or facing away: a box shows three faces at most. */
		if ((int) gte_getDataReg(GTE_MAC0) >= 0)
			continue;

		if ((budget->used + FACE_WORDS) > budget->limit)
			return;

		/* The fourth corner pushes the first out of the GTE's queue. */
		uint32_t xy0 = gte_getDataReg(GTE_SXY0);

		gte_loadV0(&v[3]);
		gte_command(GTE_CMD_RTPS | GTE_SF);
		gte_command(GTE_CMD_AVSZ4 | GTE_SF);

		/* OTZ is the plain mean Z, with ZSF4 as set below. */
		int depth = (int) gte_getDataReg(GTE_OTZ) - DEPTH_NEAR;

		if (depth < 0)
			depth = 0;
		if (depth > DEPTH_SPAN)
			depth = DEPTH_SPAN;

		unsigned int z = UI_Z_MODEL_NEAR + ((unsigned int) depth
			* (UI_Z_MODEL_FAR - UI_Z_MODEL_NEAR)) / DEPTH_SPAN;

		uint32_t *ptr = allocateOrderedGP0Packet(budget->ui->chain, z, 5);

		ptr[0] = shaded(rgb, f) | gp0_quad(false, false);
		ptr[1] = xy0;
		gte_storeDataReg(GTE_SXY0, &ptr[2]);
		gte_storeDataReg(GTE_SXY1, &ptr[3]);
		gte_storeDataReg(GTE_SXY2, &ptr[4]);

		budget->used += FACE_WORDS;
	}
}

/* A box standing on the ring's plane at `angle`, reaching to `top`. */
static Box ringBox(int angle, int half, int bottom, int top) {
	int x = (icos(angle) * RING_RADIUS) >> 12;
	int z = (isin(angle) * RING_RADIUS) >> 12;

	return (Box) {
		(int16_t) (x - half), (int16_t) (x + half),
		(int16_t) top, (int16_t) bottom,
		(int16_t) (z - half), (int16_t) (z + half)
	};
}

/* Bass red through to treble violet, a stop every four bands. */
static uint32_t bandColor(unsigned int band) {
	static const uint32_t stops[5] = {
		0xff3030, 0xffc020, 0x30e060, 0x30a0ff, 0xb050ff
	};
	unsigned int at = (band * 4 * 16) / (VISUAL_BANDS - 1); /* sixteenths */
	unsigned int i  = at / 16, t = at % 16;

	if (i >= 4)
		return stops[4];

	uint32_t out = 0;

	for (unsigned int shift = 0; shift < 24; shift += 8) {
		unsigned int a = (stops[i] >> shift) & 0xff;
		unsigned int b = (stops[i + 1] >> shift) & 0xff;

		out |= ((a * (16 - t) + b * t) / 16) << shift;
	}

	return out;
}

static void spectrum(Budget *budget, const Visual *visual) {
	for (unsigned int b = 0; b < VISUAL_BANDS; b++) {
		int height = 6 + (visual->band[b] * 200) / 255;
		Box box    = ringBox((int) ((b * 4096) / VISUAL_BANDS), 16, 0,
			-height);

		drawBox(budget, &box, bandColor(b));
	}
}

static void waveform(Budget *budget, const Visual *visual) {
	for (unsigned int i = 0; i < VISUAL_POINTS; i++) {
		/* Up for positive, down for negative, and never quite flat. */
		int value = -visual->wave[i];
		int top   = (value < 0) ? value : 0;
		int bottom = (value > 0) ? value : 0;

		if ((bottom - top) < 4) {
			top    -= 2;
			bottom += 2;
		}

		unsigned int size = (unsigned int) (bottom - top);
		uint32_t     rgb  = (size > 128) ? 0x80ffff
			: 0x20a0c0 + ((size / 2) << 16);

		Box box = ringBox((int) ((i * 4096) / VISUAL_POINTS), 10, bottom, top);

		drawBox(budget, &box, rgb);
	}
}

/*
 * Two columns of ten blocks, left and right: green, then amber, then red
 * at the top. Unlit blocks stay as dim glass so the meter keeps its shape,
 * and the held peak is lit on its own.
 */
static void levelMeter(Budget *budget, const Visual *visual) {
	for (unsigned int side = 0; side < 2; side++) {
		unsigned int lit  = (visual->level[side] * VISUAL_SEGMENTS + 127) / 255;
		unsigned int held = (visual->hold[side] * VISUAL_SEGMENTS + 127) / 255;
		int          x    = side ? 20 : -100;

		for (unsigned int s = 0; s < VISUAL_SEGMENTS; s++) {
			uint32_t on = (s >= 8) ? 0xff3030 : (s >= 6) ? 0xffc020 : 0x30e060;
			bool     shown = (s < lit) || (held && (s == held - 1));
			int      top   = 100 - (int) (s * 24) - 18;
			Box      box   = {
				(int16_t) x, (int16_t) (x + 80),
				(int16_t) top, (int16_t) (top + 18),
				-30, 30
			};

			drawBox(budget, &box, shown ? on : ((on >> 3) & 0x1f1f1f));
		}
	}
}

void sceneDraw(
	UIContext *ui, unsigned int wordBudget, const Visual *visual,
	VisualMode mode, unsigned int frame
) {
	unsigned int used = (unsigned int) (ui->chain->nextPacket - ui->chain->data);
	Budget       budget = { ui, used, used + wordBudget };

	/* Doubled across for the wide pixels, then turned: ui/model.h. */
	tinromGteScreenMatrix();

	if (mode == VISUAL_LEVEL) {
		/* Faces the screen, swaying a little rather than turning. */
		rotateCurrentMatrixX(LEVEL_TILT);
		rotateCurrentMatrixY((isin((int) (frame * 10)) * 300) >> 12);
	} else {
		rotateCurrentMatrixX(RING_TILT);
		rotateCurrentMatrixY((int) ((frame * RING_SPIN) & 4095));
	}

	gte_setControlReg(GTE_OFX,
		(uint32_t) (SCREEN_X * TINROM_SCREEN_SCALE_X) << 16);
	gte_setControlReg(GTE_OFY,
		(uint32_t) ((int) (ui->height / 2) + SCREEN_Y_BIAS) << 16);
	gte_setControlReg(GTE_TRX, 0);
	gte_setControlReg(GTE_TRY, 0);
	gte_setControlReg(GTE_TRZ, DISTANCE);
	gte_setControlReg(GTE_ZSF4, 4096 / 4);

	uint32_t plane = gte_getControlReg(GTE_H);

	gte_setControlReg(GTE_H, PROJECTION);

	switch (mode) {
		case VISUAL_LEVEL:
			levelMeter(&budget, visual);
			break;

		case VISUAL_WAVEFORM:
			waveform(&budget, visual);
			break;

		default:
			spectrum(&budget, visual);
			break;
	}

	/* H reads back sign-extended (psx-spx); the low 16 bits are the value. */
	gte_setControlReg(GTE_H, plane & 0xffff);
}
