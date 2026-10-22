/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The CD player visualiser's analysis (src/modules/cdplay/visual.c), fed
 * made-up sound. Run by scripts/test-visual.sh.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "modules/cdplay/visual.h"

#define N 256

static int fails;

#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int16_t left[N], right[N];

static void tone(double hz, double amplitude, double phase) {
	for (int n = 0; n < N; n++) {
		double v = amplitude * sin(phase + 2 * M_PI * hz * n / 44100.0);

		left[n] = right[n] = (int16_t) lround(v);
	}
}

static int loudest(const Visual *v) {
	int best = 0;

	for (int b = 1; b < VISUAL_BANDS; b++)
		if (v->band[b] > v->band[best])
			best = b;

	return best;
}

int main(void) {
	static Visual v;

	puts("silence");
	visualInit(&v);
	memset(left, 0, sizeof(left));
	memset(right, 0, sizeof(right));
	visualAnalyse(&v, left, right, N);

	for (int b = 0; b < VISUAL_BANDS; b++)
		CHECK(v.band[b] == 0);

	CHECK(v.level[0] == 0 && v.level[1] == 0);

	puts("a tone in each band");
	for (int b = 0; b < VISUAL_BANDS; b++) {
		double hz = 100.0 * pow(120.0, b / 15.0);

		visualInit(&v);
		tone(hz, 16000, 0.3);
		visualAnalyse(&v, left, right, N);

		int top = loudest(&v);

		/* The lowest bands are closer together than 256 samples resolve. */
		if (b >= 4)
			CHECK(top == b);
		else
			CHECK(top <= 4);

		CHECK(v.band[b] > 150);

		/* Two bands up from a high tone is quiet. */
		if ((b >= 4) && (b + 2 < VISUAL_BANDS))
			CHECK(v.band[b + 2] < v.band[b] - 60);

		printf("  %5.0f Hz: band %2d = %3d, loudest %2d\n", hz, b,
			v.band[b], top);
	}

	puts("levels");
	visualInit(&v);
	tone(1000, 32767, 0);
	visualAnalyse(&v, left, right, N);
	CHECK(v.level[0] >= 250 && v.level[1] >= 250);

	visualInit(&v);
	tone(1000, 3277, 0); /* -20 dB */
	visualAnalyse(&v, left, right, N);
	CHECK(v.level[0] > 110 && v.level[0] < 145);
	printf("  -20 dB: %d\n", v.level[0]);

	/* One side only. */
	visualInit(&v);
	tone(1000, 16000, 0);
	memset(right, 0, sizeof(right));
	visualAnalyse(&v, left, right, N);
	CHECK(v.level[0] > 200 && v.level[1] == 0);

	puts("falling back");
	visualInit(&v);
	tone(2433, 16000, 0);
	visualAnalyse(&v, left, right, N);
	{
		int was = v.band[10];

		visualAnalyse(&v, NULL, NULL, 0);
		CHECK(v.band[10] == was - 8);
		CHECK(v.hold[0] >= v.level[0]);

		/* The held peak waits 30 frames, then falls like the rest. */
		for (int i = 0; i < 80; i++)
			visualAnalyse(&v, NULL, NULL, 0);

		CHECK(v.band[10] == 0 && v.level[0] == 0 && v.hold[0] == 0);
	}

	puts("waveform");
	visualInit(&v);
	tone(441, 20000, -1.0); /* starts below zero, crosses soon after */
	for (int i = 0; i < 8; i++)
		visualAnalyse(&v, left, right, N);
	CHECK(v.wave[0] >= -8 && v.wave[0] <= 8);
	CHECK(v.wave[2] > 0); /* rising from the crossing */
	{
		int high = 0, low = 0;

		for (int i = 0; i < VISUAL_POINTS; i++) {
			if (v.wave[i] > high) high = v.wave[i];
			if (v.wave[i] < low)  low  = v.wave[i];
		}

		CHECK(high > 60 && low < -60);
	}

	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);

	return fails ? 1 : 0;
}
