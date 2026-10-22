/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ui/ui.h"

typedef struct TinromContext TinromContext;

/*
 * A side-by-side grid picker.
 *
 * Built for the memory card manager - two cards, fifteen save slots each,
 * laid out 3 across and 5 down - but it knows nothing about memory cards.
 * Anything that is a fixed grid of occupied and empty cells can use it: one
 * pane or two, each of any shape up to 16 by 16 (UIGridPane::columns and
 * ::rows). Cells are sized to fit the screen - 32x20 at most, and smaller
 * with a 1px gap when there are many; a 10x10 pane gets 28x16, which still
 * holds a save icon.
 *
 * The cursor rests on empty cells as well as full ones, because seeing which
 * blocks are free is half the point of looking at a card. Only cells that
 * hold something can be selected.
 */

/* The shape of a pane that leaves columns and rows at 0: a memory card's. */
#define UI_GRID_COLUMNS 3
#define UI_GRID_ROWS    5
#define UI_GRID_CELLS   (UI_GRID_COLUMNS * UI_GRID_ROWS)

#define UI_GRID_MAX_COLUMNS 16
#define UI_GRID_MAX_ROWS    16
#define UI_GRID_MAX_CELLS   (UI_GRID_MAX_COLUMNS * UI_GRID_MAX_ROWS)
#define UI_GRID_MAX_PANES   2

/*
 * The cursor can also rest on a button under a pane, for actions that belong
 * to the pane as a whole rather than to one cell. UIGridCursor::cell takes
 * this value when it is there - past every cell of any shape, so "cell >=
 * the pane's cell count" also means the button.
 */
#define UI_GRID_CELL_MENU 0xffffu

typedef enum {
	/** Nothing here. Selectable: no. */
	UI_GRID_EMPTY = 0,
	/** The start of an entry. Selectable: yes. */
	UI_GRID_ITEM,
	/**
	 * A later block of an entry that starts in an earlier cell. Drawn joined
	 * to it; selecting it selects the entry's first cell.
	 */
	UI_GRID_CONTINUED,
	/** An entry that has been deleted but not yet overwritten. */
	UI_GRID_DELETED
} UIGridCellKind;

/** No icon for this cell - see UIGridCell::icon. */
#define UI_GRID_NO_ICON (-1)

typedef struct {
	UIGridCellKind kind;
	/**
	 * Drawn inside the cell when there is no icon. About four characters
	 * fit. NULL for none.
	 */
	const char    *label;
	/**
	 * A uiIconUpload() handle, or UI_GRID_NO_ICON. An icon takes the whole
	 * cell and replaces the label, so callers that do not use icons must
	 * still set this to UI_GRID_NO_ICON rather than leaving it zeroed.
	 */
	int            icon;
	/**
	 * Draws the whole cell - icon or label - blended half-way into its own
	 * background, which is what a deleted save looks like. A multi-block
	 * entry should set it on its continuation cells too, so the save fades
	 * as one thing rather than in pieces.
	 */
	bool           faded;
} UIGridCell;

typedef struct {
	const char       *title;
	const UIGridCell *cells; /* columns * rows entries, row-major */
	/** False greys the pane out and makes every cell unselectable. */
	bool              available;
	/** Shown in place of the grid when unavailable, e.g. "no card". */
	const char       *unavailableReason;
	/**
	 * Label for a button under the pane - conventionally "..." - opening
	 * whatever belongs to the pane as a whole. NULL for no button. It stays
	 * reachable even when the pane is unavailable, because that is often
	 * exactly when its menu is wanted.
	 */
	const char       *menuLabel;
	/**
	 * The shape, at most UI_GRID_MAX_COLUMNS by UI_GRID_MAX_ROWS. 0 for
	 * either takes UI_GRID_COLUMNS or UI_GRID_ROWS, so a pane that never
	 * sets them is the 3x5 it always was.
	 */
	uint8_t           columns;
	uint8_t           rows;
} UIGridPane;

/** How many cells `pane` has: columns * rows, defaults included. */
unsigned int uiGridPaneCells(const UIGridPane *pane);

typedef struct {
	unsigned int pane;
	unsigned int cell;
} UIGridCursor;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs a blocking grid picker.
 *
 * The d-pad moves within a pane and crosses between panes at the left and
 * right edges, keeping the same row. Cross selects; Circle backs out.
 *
 * `cursor` is both where the cursor starts and where the result is written,
 * so a caller that loops - select, act, come back - keeps its place. When a
 * continuation cell is selected, the cursor is moved to the entry it belongs
 * to, so the caller always receives the entry's first cell.
 *
 * `describe` supplies the line under the grid for whatever the cursor is
 * currently on; pass NULL for none. It is called once per frame, so it should
 * be cheap and return storage that outlives the call.
 *
 * `poll` is called once per frame before anything is drawn, for a caller
 * whose panes can change underneath it - a memory card being pulled out, say.
 * It owns the pane contents and may rewrite them; returning true tells the
 * grid to re-validate the cursor, which is needed because a pane can become
 * unavailable while the cursor is sitting in it. Unlike `describe` it is
 * allowed to be slow and to draw frames of its own, since it runs outside the
 * grid's own frame. Pass NULL for none.
 *
 * Returns what the user picked; `cursor` says where from.
 */
typedef enum {
	UI_GRID_OUTCOME_CANCELLED = 0,
	/** A cell. `cursor->cell` is the first cell of the entry. */
	UI_GRID_OUTCOME_SELECTED,
	/** The pane's button. `cursor->cell` is UI_GRID_CELL_MENU. */
	UI_GRID_OUTCOME_MENU
} UIGridOutcome;

/**
 * Screen position of a cell's bottom-left corner, or of the pane's button
 * when `cell` is UI_GRID_CELL_MENU, as the grid was last drawn. For
 * anchoring an overlay to whatever was picked.
 */
void uiGridCellAnchor(
	unsigned int pane,
	unsigned int paneCount,
	unsigned int cell,
	int         *x,
	int         *y
);

/**
 * Draws one frame of the grid without running a loop or reading input, so an
 * overlay can repaint it underneath itself. Call between uiBeginFrame() and
 * uiEndFrame().
 */
void uiGridDraw(
	UIContext          *ui,
	const UIGridPane   *panes,
	unsigned int        paneCount,
	const UIGridCursor *cursor
);

/**
 * The footer hint uiGridRun() shows, for a caller that takes more buttons
 * than the grid's own - in its poll hook, say. NULL puts the grid's back.
 * It stays until changed, so a caller that sets one clears it on its way
 * out.
 */
void uiGridSetHint(const char *hint);

UIGridOutcome uiGridRun(
	TinromContext    *ctx,
	const char       *title,
	const UIGridPane *panes,
	unsigned int      paneCount,
	UIGridCursor     *cursor,
	const char     *(*describe)(
		const UIGridPane *pane, unsigned int cell, void *user
	),
	bool            (*poll)(void *user),
	void             *user
);

#ifdef __cplusplus
}
#endif
