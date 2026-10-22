/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * A text canvas: rows of monospaced text, composed into an 8bpp bitmap by the
 * CPU and drawn as sprites 256 texels wide - three across the screen.
 *
 * The UI's normal text path costs about four packet words per glyph out of the
 * frame's packet budget (see docs/architecture.md, "The per-frame packet
 * budget"), and its spacing is proportional, so it can neither fill a screen
 * with text nor line text up in columns. A canvas costs at most 21 words
 * however much is on it, and every glyph is the same width.
 *
 * It uses the UI font (ui/font.h), each glyph centred in a cell 8 units
 * wide. Like the screen, the canvas has two texels across per unit - a cell
 * is 16 texels - and is drawn one texel to a display pixel; positions passed
 * in are units, as everywhere else in the UI.
 * The canvas has sixteen inks; a pixel is ink * 16 + the font's own shade, so
 * every ink keeps the font's outline and shading, the way a tinted GPU glyph
 * does.
 *
 * There is one canvas slot in VRAM, right of the framebuffers, so only one
 * canvas can be on screen at a time. Ink 0 and the font's blank texels are
 * transparent: whatever is drawn behind the canvas shows through between the
 * glyphs, which is how a cursor is drawn - a filled rectangle behind them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A glyph cell, in units, and how far apart rows are. */
#define UI_CANVAS_GLYPH_W 8
#define UI_CANVAS_LINE_H  10

/* The screen's width, in units and in texels. A texel is a byte. */
#define UI_CANVAS_WIDTH    320
#define UI_CANVAS_SCALE_X  2
#define UI_CANVAS_TEXELS   (UI_CANVAS_WIDTH * UI_CANVAS_SCALE_X)
#define UI_CANVAS_MAX_ROWS 24

/** Bytes of pixel storage a canvas of `rows` needs. */
#define UI_CANVAS_BYTES(rows) (UI_CANVAS_TEXELS * UI_CANVAS_LINE_H * (rows))

/*
 * Where it lives in VRAM, in 16-bit words: 320 words is the 640 texels of
 * one row. See the layout note in ui.c. The terminal (comms/termview.c)
 * borrows the same slot for a bitmap of its own, 320 texels wide, which
 * ui.c stretches across the screen.
 */
#define UI_CANVAS_VRAM_X 640
#define UI_CANVAS_VRAM_Y 256
#define UI_CANVAS_CLUT_X 640
#define UI_CANVAS_CLUT_Y 496

/* Inks as 0x00RRGGBB, like the theme. Ink 0 is always transparent. */
#define UI_CANVAS_INKS 16

typedef struct UICanvas {
	/* UI_CANVAS_BYTES(rows) bytes, 4-byte aligned: it is DMA'd as is. */
	uint8_t     *pixels;
	unsigned int rows;
	/* The GPU's CLUT attribute for the palette. */
	uint16_t     clut;
} UICanvas;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sets a canvas up over caller-owned storage and uploads its palette.
 * `inks` gives inks 1..15 (index 0 of the array is ignored). Blocking: call
 * it when a screen opens, not per frame.
 */
void uiCanvasInit(
	UICanvas *canvas, uint8_t *pixels, unsigned int rows,
	const uint32_t inks[UI_CANVAS_INKS]
);

void uiCanvasClear(UICanvas *canvas);

/**
 * One glyph with its cell's left edge at pixel `x` of `row`. `x` is rounded
 * down to a multiple of 4, which is what lets a glyph be written a word at a
 * time. ' ' clears the cell. Returns the x of the next cell.
 */
int uiCanvasPut(
	UICanvas *canvas, int x, unsigned int row, char c, uint8_t ink
);

/** A run of glyphs; stops at the right edge. Returns the x after it. */
int uiCanvasPrint(
	UICanvas *canvas, int x, unsigned int row, const char *text, uint8_t ink
);

/** `digits` hex digits of `value`, most significant first. */
int uiCanvasHex(
	UICanvas *canvas, int x, unsigned int row, uint32_t value,
	unsigned int digits, uint8_t ink
);

/**
 * Drawing it is uiCanvasDraw(), in ui.h, since it needs the frame.
 *
 * Sends the bitmap to VRAM. Once per frame, after composing and before the
 * frame is presented; it waits for the previous frame's list to finish
 * transferring, so the GPU never samples a half-written canvas.
 */
void uiCanvasUpload(UICanvas *canvas);

#ifdef __cplusplus
}
#endif
