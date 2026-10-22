/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Drawing the terminal. See termview.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include "comms/termview.h"
#include "common/gpu.h"
#include "hal/uart_sio.h"
#include "libc/string.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"
#include "ui/canvas.h"

#define STRIDE    UI_CANVAS_WIDTH
#define ROW_BYTES (STRIDE * TERM_VIEW_CELL_H)

static uint8_t  pixels[ROW_BYTES * TERM_ROWS] __attribute__((aligned(4)));
static uint32_t expand[16];
static bool     everything = true;
static unsigned shownBack;
static uint32_t shownSerial;

/* The usual sixteen, in ANSI order: black, red, green, yellow, blue... */
static const uint32_t palette[16] = {
	0x000000, 0xaa0000, 0x00aa00, 0xaa5500,
	0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
	0x555555, 0xff5555, 0x55ff55, 0xffff55,
	0x5555ff, 0xff55ff, 0x55ffff, 0xffffff
};

uint32_t termViewColor(unsigned index) {
	return palette[index & 15];
}

static uint16_t rgb15(uint32_t color) {
	uint16_t value = (uint16_t) ((((color >> 16) & 0xff) >> 3)
		| ((((color >> 8) & 0xff) >> 3) << 5)
		| (((color & 0xff) >> 3) << 10));

	/* 0000h is the transparent texel; black has to be spelled 8000h. */
	return value ? value : 0x8000;
}

TermScreen *termViewScreen(void) {
	static TermScreen screen;

	return &screen;
}

void termViewInit(void) {
	/* Bit 3 of a font row is the leftmost pixel, which is byte 0. */
	for (unsigned mask = 0; mask < 16; mask++) {
		uint32_t word = 0;

		for (unsigned x = 0; x < 4; x++)
			if (mask & (8 >> x))
				word |= 1u << (x * 8);

		expand[mask] = word;
	}

	static uint16_t clut[256] __attribute__((aligned(4)));

	for (unsigned i = 0; i < 256; i++)
		clut[i] = rgb15(palette[i & 15]);

	sendVRAMData(clut, UI_CANVAS_CLUT_X, UI_CANVAS_CLUT_Y, 256, 1);
	waitForGPUDMADone();

	everything = true;
}

void termViewInvalidate(void) {
	everything = true;
}

static void composeRow(unsigned row, const TermCell *cells) {
	uint32_t *base = (uint32_t *) (pixels + row * ROW_BYTES);

	for (unsigned col = 0; col < TERM_COLS; col++) {
		const uint8_t *glyph;
		uint32_t       fg = 7, bg = 0;

		if (cells) {
			glyph = uiTermFont[cells[col].ch];
			fg    = cells[col].attr & 15;
			bg    = cells[col].attr >> 4;
		} else {
			glyph = uiTermFont[' '];
		}

		uint32_t *out = base + col;

		/* Each byte of expand[] is 0 or 1, so the products never carry. */
		for (unsigned y = 0; y < TERM_VIEW_CELL_H; y++, out += STRIDE / 4) {
			unsigned bits = glyph[y] & 15;

			*out = (expand[bits] * fg) | (expand[bits ^ 15] * bg);
		}
	}
}

void termViewUpdate(TermScreen *term, unsigned back) {
	uint32_t rows = term->dirty;

	/*
	 * Dirty bits are live screen rows. Scrolled back, those are drawn
	 * somewhere else, or not at all, so any change redraws the view.
	 */
	if (everything || (back != shownBack)
		|| (back && (rows || (term->historySerial != shownSerial))))
		rows = (1u << TERM_ROWS) - 1;

	term->dirty = 0;
	everything  = false;
	shownBack   = back;
	shownSerial = term->historySerial;

	if (!rows)
		return;

	/*
	 * A row at a time, composed and then sent, with the serial port drained
	 * in between: a full screen is 64 KB of pixels, and at the higher rates
	 * the port's FIFO lasts well under a millisecond.
	 */
	for (unsigned row = 0; row < TERM_ROWS; row++) {
		if (!(rows & (1u << row)))
			continue;

		composeRow(row, termLine(term, (TERM_ROWS - 1 - row) + back));
		tinromSioPump();

		/* A row is 640 words: the wait for the one before is short. */
		sendVRAMData(
			pixels + row * ROW_BYTES, UI_CANVAS_VRAM_X,
			UI_CANVAS_VRAM_Y + row * TERM_VIEW_CELL_H,
			UI_CANVAS_WIDTH / 2, TERM_VIEW_CELL_H
		);
	}

	waitForGPUDMADone();
	waitForGP0Ready();
	GPU_GP0 = gp0_flushCache();
}

void termViewDraw(
	UIContext *ui, const TermScreen *term, int x, int y, unsigned back,
	bool cursorOn
) {
	/* A texel per unit, so each is stretched over two display pixels. */
	uiBitmapDraw(ui, gp0_clut(UI_CANVAS_CLUT_X / 16, UI_CANVAS_CLUT_Y),
		x, y, TERM_VIEW_WIDTH, TERM_VIEW_HEIGHT, true);

	if (!back && cursorOn && term->cursorVisible) {
		uint8_t attr = termRow((TermScreen *) term, term->cy)[term->cx].attr;

		uiFillRect(ui, x + term->cx * TERM_VIEW_CELL_W,
			y + term->cy * TERM_VIEW_CELL_H + TERM_VIEW_CELL_H - 2,
			TERM_VIEW_CELL_W, 2, palette[attr & 15]);
	}
}
