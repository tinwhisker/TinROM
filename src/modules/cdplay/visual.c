/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The visualiser's analysis. See visual.h.
 */

#include <stddef.h>
#include <stdint.h>
#include "common/trig.h"
#include "modules/cdplay/visual.h"

/*
 * A sine table, a full turn in 256 steps, Q12 - built at start-up from
 * ps1-bare-metal's isin(), whose turn is 4096.
 */
#define TABLE      256
#define QUARTER    (TABLE / 4)
#define ONE        4096

static int16_t sine[TABLE];

/*
 * The bands, log-spaced from 100 Hz to 12 kHz - 100 * 120^(k/15) - as
 * phase steps per sample at 44.1 kHz, in 1/65536 of a turn. 256 samples
 * resolve 172 Hz, so the lowest three or four overlap; they still move
 * with the bass, which is what they are for.
 */
static const uint16_t step[VISUAL_BANDS] = {
	  149,   204,   281,   387,   533,   733,  1009,  1388,
	 1910,  2628,  3615,  4975,  6845,  9419, 12960, 17833
};

/* How fast things fall back, per frame, out of 255. */
#define BAND_FALL  8
#define LEVEL_FALL 6
#define HOLD_TIME  30

/*
 * log2 in eighths, the fraction straight from the bits below the top one -
 * near enough for bars. Of a power, an eighth is 0.38 dB; of an amplitude,
 * 0.75 dB. 0 for 0.
 */
static int log2Eighths(uint32_t value) {
	if (!value)
		return 0;

	int top = 31;

	while (!(value & (1u << top)))
		top--;

	uint32_t fraction = (top >= 3) ? ((value >> (top - 3)) & 7)
		: ((value << (3 - top)) & 7);

	return (top * 8) + (int) fraction;
}

/* `x` from `floor` up, over `range`, to 0..255. */
static uint8_t scale(int x, int floor, int range) {
	int out = ((x - floor) * 255) / range;

	return (uint8_t) ((out < 0) ? 0 : (out > 255) ? 255 : out);
}

static uint8_t fall(uint8_t shown, uint8_t now, unsigned int by) {
	if (now >= shown)
		return now;

	return (shown > (now + by)) ? (uint8_t) (shown - by) : now;
}

void visualInit(Visual *visual) {
	for (unsigned int i = 0; i < TABLE; i++)
		sine[i] = (int16_t) isin((int) (i * (4096 / TABLE)));

	for (unsigned int i = 0; i < VISUAL_BANDS; i++)
		visual->band[i] = 0;

	for (unsigned int i = 0; i < 2; i++) {
		visual->level[i]      = 0;
		visual->hold[i]       = 0;
		visual->holdFrames[i] = 0;
	}

	for (unsigned int i = 0; i < VISUAL_POINTS; i++)
		visual->wave[i] = 0;
}

/*
 * One band: the input correlated with a sine and a cosine at the band's
 * frequency - a single DFT bin, which for sixteen bands is cheaper and far
 * simpler than an FFT. `mono` is already windowed and eight bits, so a sum
 * of 256 products with the Q12 table stays under 2^27.
 *
 * Returns log2 of the power in eighths; a full-scale tone comes to about
 * 192.
 */
static int bandPower(const int8_t *mono, unsigned int count, uint16_t rate) {
	int32_t  re = 0, im = 0;
	uint32_t phase = 0;

	for (unsigned int n = 0; n < count; n++) {
		unsigned int index = (phase >> 8) & (TABLE - 1);

		re += mono[n] * sine[(index + QUARTER) & (TABLE - 1)];
		im += mono[n] * sine[index];
		phase += rate;
	}

	re >>= 13;
	im >>= 13;

	return log2Eighths((uint32_t) (re * re) + (uint32_t) (im * im));
}

void visualAnalyse(
	Visual *visual, const int16_t *left, const int16_t *right,
	unsigned int count
) {
	static int8_t mono[TABLE];
	uint8_t       level[2] = { 0, 0 };
	bool          sound    = left && right && count;

	if (count > TABLE)
		count = TABLE;

	if (sound) {
		/*
		 * Both sides to eight bits, and a Hann window over it - 1 - cos,
		 * halved - so a tone between two bands does not smear into all of
		 * them. The level meter wants the peaks before any of that.
		 */
		int peak[2] = { 0, 0 };

		for (unsigned int n = 0; n < count; n++) {
			int l = left[n], r = right[n];

			if ((l < 0 ? -l : l) > peak[0]) peak[0] = (l < 0) ? -l : l;
			if ((r < 0 ? -r : r) > peak[1]) peak[1] = (r < 0) ? -r : r;

			unsigned int at  = (n * TABLE) / count;
			int          hann = (ONE - sine[(at + QUARTER) & (TABLE - 1)]) / 2;

			mono[n] = (int8_t) ((((l + r) >> 9) * hann) >> 12);
		}

		/* 40 dB of peak level, which is 53 eighths, up to full scale. */
		for (unsigned int side = 0; side < 2; side++)
			level[side] = scale(log2Eighths((uint32_t) peak[side]),
				120 - 53, 53);
	}

	for (unsigned int b = 0; b < VISUAL_BANDS; b++) {
		uint8_t now = 0;

		/*
		 * Music has less in its higher bands, about 3 dB an octave less;
		 * each band is near half an octave above the last, so it gets
		 * 1.4 dB - four eighths - more. The bars show 45 dB, topping out
		 * a little above a full-scale tone, 192: music spreads its power
		 * over many bands, and a bar pinned at the top says nothing.
		 */
		if (sound)
			now = scale(bandPower(mono, count, step[b]) + (int) (b * 4),
				200 - 120, 120);

		visual->band[b] = fall(visual->band[b], now, BAND_FALL);
	}

	for (unsigned int side = 0; side < 2; side++) {
		visual->level[side] = fall(visual->level[side], level[side],
			LEVEL_FALL);

		if (visual->level[side] >= visual->hold[side]) {
			visual->hold[side]       = visual->level[side];
			visual->holdFrames[side] = HOLD_TIME;
		} else if (visual->holdFrames[side]) {
			visual->holdFrames[side]--;
		} else {
			visual->hold[side] = fall(visual->hold[side],
				visual->level[side], LEVEL_FALL);
		}
	}

	/*
	 * The waveform from a rising zero crossing in the first half, so a
	 * steady tone stands still instead of crawling; five samples a point,
	 * which is 120 of the 256. Halved toward the new shape each frame.
	 */
	unsigned int start = 0;

	if (sound) {
		for (unsigned int n = 1; n < (count / 2); n++) {
			if ((left[n - 1] + right[n - 1] < 0) && (left[n] + right[n] >= 0)) {
				start = n;
				break;
			}
		}
	}

	for (unsigned int i = 0; i < VISUAL_POINTS; i++) {
		unsigned int n   = start + (i * 5);
		int          now = 0;

		if (sound && (n < count))
			now = (left[n] + right[n]) >> 9;

		visual->wave[i] = (int8_t) ((visual->wave[i] + now) / 2);
	}
}

const char *visualModeName(VisualMode mode) {
	switch (mode) {
		case VISUAL_SPECTRUM: return "spectrum";
		case VISUAL_LEVEL:    return "level meter";
		case VISUAL_WAVEFORM: return "waveform";
		default:              return "";
	}
}
