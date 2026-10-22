/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The CD player's visualiser: what it shows, worked out from the SPU's
 * capture of the CD audio (hal/spu.h), and drawn in 3D behind the track
 * grid (scene.c).
 *
 * This half - the analysis - is plain C, so it builds and is tested on the
 * host (scripts/test-visual.sh). It uses no 64-bit arithmetic: on the MIPS
 * toolchain that pulls in kilobytes of helper code (see model.c), so every
 * product below is kept inside 32 bits by scaling the samples first.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VISUAL_BANDS    16
#define VISUAL_SEGMENTS 10 /* per side, in the level meter */
#define VISUAL_POINTS   24 /* of waveform */

typedef enum {
	VISUAL_SPECTRUM = 0,
	VISUAL_LEVEL,
	VISUAL_WAVEFORM,
	VISUAL_MODES
} VisualMode;

typedef struct {
	/* 0..255 each, already eased: they rise at once and fall slowly. */
	uint8_t band[VISUAL_BANDS];
	/* Left and right peak level, 0..255 over 40 dB, and its held peak. */
	uint8_t level[2], hold[2];
	uint8_t holdFrames[2];
	/* Signed, -128..127, starting at a rising zero crossing. */
	int8_t  wave[VISUAL_POINTS];
} Visual;

#ifdef __cplusplus
extern "C" {
#endif

/** Everything at rest. Call once; it also builds the tables. */
void visualInit(Visual *visual);

/**
 * One frame's worth: `count` samples each side, normally
 * TINROM_SPU_CAPTURE_SAMPLES (256). NULL for both means no sound this frame
 * - stopped, or the capture failed - and everything falls back to rest.
 */
void visualAnalyse(
	Visual *visual, const int16_t *left, const int16_t *right,
	unsigned int count
);

/** The mode's name, for the screen. */
const char *visualModeName(VisualMode mode);

#ifdef __cplusplus
}
#endif
