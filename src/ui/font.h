/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The UI font: Superpowers' outlined glyphs (CC0; see src/ui/fonts/README.md
 * and NOTICE.md), drawn at 20x20 and turned into the tables below at build
 * time by tools/convertfont.py: all 20 columns kept, rows scaled to 10.
 *
 * Its glyphs are measured in texels, which are display pixels: the screen
 * is 640 wide, twice the 320 layout units everything else is placed in
 * (hal/video.h), so a glyph has two texels for every unit across and one
 * for every unit down. The layout calls below take and give units, like the
 * rest of the UI; only the glyph table and the renderers deal in texels.
 *
 * It is drawn two ways:
 *
 *   by the GPU, a glyph at a time, proportionally spaced and tinted any
 *   colour - uiText() and friends in ui.h;
 *
 *   by the CPU into a text canvas, monospaced - ui/canvas.h.
 *
 * This file is the part both share: the glyph table and the rules for laying
 * a string out in lines. None of it touches the GPU, so it builds on the host
 * too.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The sizes below must match the font. tools/convertfont.py checks them when
 * the generated source is compiled, and the error names any it disagrees
 * with.
 */
#define UI_FONT_FIRST        32 /* the first cell is a space */
#define UI_FONT_GLYPHS       95 /* printable ASCII, 20h..7Eh */
#define UI_FONT_HEIGHT        9 /* every glyph is drawn this many rows tall */
#define UI_FONT_SHEET_WIDTH 256 /* texels, 4bpp: one texture page */
#define UI_FONT_SHEET_HEIGHT 45

/* Texels across per layout unit. */
#define UI_FONT_SCALE_X 2
#define UI_FONT_SHEET_BYTES  (UI_FONT_SHEET_WIDTH * UI_FONT_SHEET_HEIGHT / 2)

/* How far a '\n' moves the pen down, and where tab stops fall, in units. */
#define UI_FONT_LINE_HEIGHT 10
#define UI_FONT_TAB_WIDTH   72

typedef struct {
	/* Top-left in the sheet. */
	uint8_t u, v;
	/* Texels drawn across; 0 for a space. */
	uint8_t width;
	/*
	 * How far the pen moves after it, in texels. Two less than the width:
	 * the glyphs are outlined, two texels thick, and two neighbours share an
	 * outline rather than putting two side by side.
	 */
	uint8_t advance;
} UIFontGlyph;

/* Generated; see tools/convertfont.py. Index 0 of the CLUT is transparent. */
extern const uint16_t    uiFontClut[16];
extern const UIFontGlyph uiFontGlyphs[UI_FONT_GLYPHS];
extern const uint8_t     uiFontSheet[UI_FONT_SHEET_BYTES];

#ifdef __cplusplus
extern "C" {
#endif

/** The glyph for `c`. Anything the font does not have draws as '?'. */
const UIFontGlyph *uiFontGlyph(char c);

/**
 * Lays out one line of `text`, starting at its beginning: up to a '\n', the
 * end of the string, or - when `maxWidth` is above zero - the last space that
 * keeps the line within `maxWidth` units. A single word wider than that is
 * broken where it stops fitting.
 *
 * `*length` gets the number of characters to draw and `*width` the units
 * they cover, rounded up. Returns where the next line starts - past the '\n'
 * or the space the line was broken at - or NULL when this was the last line.
 */
const char *uiFontNextLine(
	const char *text, int maxWidth, size_t *length, int *width
);

/** The width of the widest line, in units, with no wrapping. */
int uiFontTextWidth(const char *text);

/** How many lines `text` takes when wrapped at `maxWidth` (0: no wrap). */
unsigned int uiFontLineCount(const char *text, int maxWidth);

#ifdef __cplusplus
}
#endif
