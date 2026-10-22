/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The text canvas. See canvas.h.
 *
 * Every glyph is pre-expanded once into the sixteen bytes of each of its
 * rows, one byte per texel holding the font's shade (0 blank, 1..15 its CLUT
 * index). Writing a glyph in ink k is then four word stores per row: the
 * expanded row OR k * 10101010h, which puts k in the high nibble of all four
 * bytes at once. A blank texel comes out as k * 16, and every one of those
 * palette entries is transparent.
 *
 * That is what keeps a full screen of hex cheap enough to redraw every
 * frame on a 33 MHz CPU with no data cache: a glyph is forty stores, not
 * a hundred and forty-four read-modify-writes.
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "libc/string.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"
#include "ui/canvas.h"
#include "ui/font.h"

#define STRIDE      UI_CANVAS_TEXELS
#define ROW_BYTES   (STRIDE * UI_CANVAS_LINE_H)
#define CELL        (UI_CANVAS_GLYPH_W * UI_CANVAS_SCALE_X) /* texels */
#define GLYPH_WORDS (CELL / 4)

static uint32_t glyphRows[UI_FONT_GLYPHS][UI_FONT_HEIGHT][GLYPH_WORDS];
static bool     glyphsReady;

static unsigned int sheetTexel(unsigned int x, unsigned int y) {
	uint8_t byte = uiFontSheet[(y * UI_FONT_SHEET_WIDTH + x) / 2];

	return (x & 1) ? (byte >> 4) : (byte & 15);
}

static void expandGlyphs(void) {
	for (unsigned int g = 0; g < UI_FONT_GLYPHS; g++) {
		const UIFontGlyph *glyph = &uiFontGlyphs[g];

		/*
		 * Centred in the cell. The few glyphs wider than it - M and W, a
		 * couple of symbols - lose outline from each side, and the widest a
		 * little of themselves; in a hex dump's text column that is the
		 * price of every character lining up.
		 */
		int width = glyph->width;
		int skip  = (width > CELL) ? ((width - CELL) / 2) : 0;
		int pad   = (width < CELL) ? ((CELL - width) / 2) : 0;

		for (unsigned int y = 0; y < UI_FONT_HEIGHT; y++) {
			uint8_t row[CELL] = { 0 };

			for (int x = 0; x < CELL; x++) {
				int source = x - pad + skip;

				if ((source >= 0) && (source < width))
					row[x] = (uint8_t) sheetTexel(
						glyph->u + (unsigned int) source, glyph->v + y
					);
			}

			/* Little-endian: byte 0 of the word is the leftmost pixel. */
			for (unsigned int w = 0; w < GLYPH_WORDS; w++)
				glyphRows[g][y][w] = (uint32_t) row[w * 4]
					| ((uint32_t) row[w * 4 + 1] <<  8)
					| ((uint32_t) row[w * 4 + 2] << 16)
					| ((uint32_t) row[w * 4 + 3] << 24);
		}
	}

	glyphsReady = true;
}

/*
 * One font shade tinted by one ink, the way the GPU tints a glyph: the
 * font's white becomes the ink, and darker shades darken with it.
 */
static uint16_t tint(uint16_t shade, uint32_t ink) {
	unsigned int out = 0;

	for (unsigned int channel = 0; channel < 3; channel++) {
		unsigned int texel = (shade >> (channel * 5)) & 31;
		unsigned int level = (ink >> (16 - channel * 8)) & 0xff;
		unsigned int value = (texel * level) / 248;

		out |= ((value > 31) ? 31 : value) << (channel * 5);
	}

	/* 0000h is the transparent texel; a black outline must stay solid. */
	return out ? (uint16_t) out : 0x8000;
}

void uiCanvasInit(
	UICanvas *canvas, uint8_t *pixels, unsigned int rows,
	const uint32_t inks[UI_CANVAS_INKS]
) {
	if (!glyphsReady)
		expandGlyphs();

	if (rows > UI_CANVAS_MAX_ROWS)
		rows = UI_CANVAS_MAX_ROWS;

	canvas->pixels = pixels;
	canvas->rows   = rows;
	canvas->clut   = gp0_clut(UI_CANVAS_CLUT_X / 16, UI_CANVAS_CLUT_Y);

	/* Aligned for the DMA. */
	static uint16_t clut[UI_CANVAS_INKS * 16] __attribute__((aligned(4)));

	for (unsigned int ink = 0; ink < UI_CANVAS_INKS; ink++) {
		for (unsigned int shade = 0; shade < 16; shade++)
			clut[ink * 16 + shade] = (ink && shade)
				? tint(uiFontClut[shade], inks[ink]) : 0;
	}

	uiCanvasClear(canvas);

	sendVRAMData(
		clut, UI_CANVAS_CLUT_X, UI_CANVAS_CLUT_Y, UI_CANVAS_INKS * 16, 1
	);
	waitForGPUDMADone();
}

void uiCanvasClear(UICanvas *canvas) {
	memset(canvas->pixels, 0, UI_CANVAS_BYTES(canvas->rows));
}

int uiCanvasPut(
	UICanvas *canvas, int x, unsigned int row, char c, uint8_t ink
) {
	x &= ~3;

	if ((x < 0) || ((x + UI_CANVAS_GLYPH_W) > UI_CANVAS_WIDTH)
		|| (row >= canvas->rows))
		return x + UI_CANVAS_GLYPH_W;

	unsigned int texel = (unsigned int) x * UI_CANVAS_SCALE_X;

	const uint32_t (*rows)[GLYPH_WORDS] =
		glyphRows[uiFontGlyph(c) - uiFontGlyphs];
	uint32_t  pattern = (uint32_t) (ink & 15) * 0x10101010u;
	uint32_t *out     = (uint32_t *) (
		canvas->pixels + (row * ROW_BYTES) + texel
	);

	for (unsigned int y = 0; y < UI_FONT_HEIGHT; y++, out += STRIDE / 4) {
		for (unsigned int w = 0; w < GLYPH_WORDS; w++)
			out[w] = rows[y][w] | pattern;
	}

	/* The gap below the glyph, in case something was there before. */
	for (unsigned int y = UI_FONT_HEIGHT; y < UI_CANVAS_LINE_H;
		y++, out += STRIDE / 4) {
		for (unsigned int w = 0; w < GLYPH_WORDS; w++)
			out[w] = 0;
	}

	return x + UI_CANVAS_GLYPH_W;
}

int uiCanvasPrint(
	UICanvas *canvas, int x, unsigned int row, const char *text, uint8_t ink
) {
	while (*text && ((x + UI_CANVAS_GLYPH_W) <= UI_CANVAS_WIDTH))
		x = uiCanvasPut(canvas, x, row, *(text++), ink);

	return x;
}

int uiCanvasHex(
	UICanvas *canvas, int x, unsigned int row, uint32_t value,
	unsigned int digits, uint8_t ink
) {
	static const char hex[] = "0123456789ABCDEF";

	for (unsigned int i = digits; i > 0; i--)
		x = uiCanvasPut(canvas, x, row, hex[(value >> ((i - 1) * 4)) & 15],
			ink);

	return x;
}

void uiCanvasUpload(UICanvas *canvas) {
	/* sendVRAMData() waits for any transfer in flight, the last list too. */
	sendVRAMData(
		canvas->pixels, UI_CANVAS_VRAM_X, UI_CANVAS_VRAM_Y,
		UI_CANVAS_TEXELS / 2, canvas->rows * UI_CANVAS_LINE_H
	);
	waitForGPUDMADone();

	/* The GPU's texture cache would otherwise keep serving the old texels. */
	waitForGP0Ready();
	GPU_GP0 = gp0_flushCache();
}
