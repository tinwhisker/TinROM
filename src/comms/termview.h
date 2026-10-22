/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Drawing a TermScreen: 80 columns of the 4x8 terminal font
 * (src/ui/fonts/term4x8.txt) across the full 320-unit screen, composed on
 * the CPU into the text canvas's VRAM slot (ui/canvas.h) and drawn as one
 * bitmap - a texel per unit, which ui.c stretches over the display's two
 * pixels.
 *
 * A pixel is a colour index, 0-15 in the ANSI order, so a cell row is one
 * word: the glyph's four bits picking foreground or background. Only rows
 * that changed are composed and uploaded.
 */
#pragma once

#include <stdbool.h>
#include "comms/term.h"
#include "ui/ui.h"

#define TERM_VIEW_CELL_W 4
#define TERM_VIEW_CELL_H 8
#define TERM_VIEW_WIDTH  (TERM_COLS * TERM_VIEW_CELL_W)
#define TERM_VIEW_HEIGHT (TERM_ROWS * TERM_VIEW_CELL_H)

/* The font, generated from term4x8.txt by tools/convertbitfont.py. */
extern const uint8_t uiTermFont[256][TERM_VIEW_CELL_H];

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The one screen. The terminal and the IRC client never run at once, so
 * they share it rather than each keeping 36 KB of their own.
 */
TermScreen *termViewScreen(void);

/** Uploads the palette; the next update composes everything. */
void termViewInit(void);
/** Forces every row to be composed again. */
void termViewInvalidate(void);

/**
 * Composes what changed and sends it to VRAM. `back` is how many lines the
 * view is scrolled back into the history; 0 is the live screen.
 */
void termViewUpdate(TermScreen *term, unsigned back);

/**
 * Draws it with its top-left at (x, y), and the cursor - an underline, on
 * when `cursorOn` - if the view is live and the cursor is showing.
 */
void termViewDraw(
	UIContext *ui, const TermScreen *term, int x, int y, unsigned back,
	bool cursorOn
);

/** A colour of the palette as 0xRRGGBB, for drawing next to the screen. */
uint32_t termViewColor(unsigned index);

#ifdef __cplusplus
}
#endif
