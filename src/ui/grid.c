/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include "core/app.h"
#include "hal/pad.h"
#include "hal/video.h"
#include "ui/grid.h"
#include "ui/theme.h"
#include "ui/ui.h"

/*
 * Layout. The memory card's two panes of 3x5 cells sit side by side and
 * centred:
 *
 *   2 * (3*32 + 2*3) + 28  =  232 px of a 320 px screen
 *
 * which leaves a 44 px margin either side.
 *
 * A cell is at most 32x20: two pixels taller than a 16x16 save icon, so an
 * icon sits centred with a one pixel border. A pane with more cells than fit
 * at that size gets smaller ones, sized to what the screen has room for;
 * when 3px gaps would squeeze a cell below an icon's size the gaps go down
 * to 1px. A single 10x10 pane comes out at 28x16 - icons still fit - and
 * 289x169 px over all.
 */
#define CELL_WIDTH   32
#define CELL_HEIGHT  20
#define CELL_GAP      3
#define CELL_GAP_TIGHT 1
#define CELL_MIN      8
#define PANE_GAP     28
#define PANE_TITLE_H 12

/* Thickness of the cursor ring drawn behind the selected cell. */
#define CURSOR_INSET  2

#define GRID_TOP     (TINROM_HEADER_H + 14)

/* The pane's own button, directly under its last row of cells. */
#define MENU_BUTTON_H 12

/* Room left under the grid for the description line. */
#define DESCRIBE_H   18

/* Where everything in one pane goes, worked out from its shape. */
typedef struct {
	unsigned int columns, rows, cells;
	int          cellWidth, cellHeight, gap;
	int          width, bodyHeight;
	int          originX, menuY;
} Layout;

/* As each pane was last drawn, for uiGridCellAnchor(). */
static Layout lastLayout[UI_GRID_MAX_PANES];

static unsigned int paneColumns(const UIGridPane *pane) {
	unsigned int columns = pane->columns ? pane->columns : UI_GRID_COLUMNS;

	return (columns > UI_GRID_MAX_COLUMNS) ? UI_GRID_MAX_COLUMNS : columns;
}

static unsigned int paneRows(const UIGridPane *pane) {
	unsigned int rows = pane->rows ? pane->rows : UI_GRID_ROWS;

	return (rows > UI_GRID_MAX_ROWS) ? UI_GRID_MAX_ROWS : rows;
}

unsigned int uiGridPaneCells(const UIGridPane *pane) {
	return paneColumns(pane) * paneRows(pane);
}

static int fit(int room, unsigned int count, int gap, int most) {
	int size = (room - ((int) (count - 1) * gap)) / (int) count;

	if (size > most)
		size = most;

	return (size < CELL_MIN) ? CELL_MIN : size;
}

static Layout layoutFor(
	const UIGridPane *pane, unsigned int paneCount, int screenHeight
) {
	Layout layout;

	layout.columns = paneColumns(pane);
	layout.rows    = paneRows(pane);
	layout.cells   = layout.columns * layout.rows;

	int roomWidth  = ((int) TINROM_SCREEN_WIDTH - (2 * TINROM_MARGIN_X)
		- ((int) (paneCount - 1) * PANE_GAP)) / (int) paneCount;
	int roomHeight = screenHeight - GRID_TOP - TINROM_FOOTER_H - DESCRIBE_H
		- (pane->menuLabel ? (MENU_BUTTON_H + CELL_GAP) : 0);

	layout.gap        = CELL_GAP;
	layout.cellWidth  = fit(roomWidth, layout.columns, CELL_GAP, CELL_WIDTH);
	layout.cellHeight = fit(roomHeight, layout.rows, CELL_GAP, CELL_HEIGHT);

	if ((layout.cellWidth < UI_ICON_SIZE)
		|| (layout.cellHeight < UI_ICON_SIZE)) {
		layout.gap        = CELL_GAP_TIGHT;
		layout.cellWidth  = fit(roomWidth, layout.columns, CELL_GAP_TIGHT,
			CELL_WIDTH);
		layout.cellHeight = fit(roomHeight, layout.rows, CELL_GAP_TIGHT,
			CELL_HEIGHT);
	}

	layout.width      = ((int) layout.columns * layout.cellWidth)
		+ ((int) (layout.columns - 1) * layout.gap);
	layout.bodyHeight = ((int) layout.rows * layout.cellHeight)
		+ ((int) (layout.rows - 1) * layout.gap);
	layout.menuY      = GRID_TOP + layout.bodyHeight + CELL_GAP;
	layout.originX    = 0;

	return layout;
}

/* Every pane's layout, placed side by side and centred as a group. */
static void layoutAll(
	const UIGridPane *panes, unsigned int paneCount, int screenHeight,
	Layout *out
) {
	int total = (int) ((paneCount - 1) * PANE_GAP);

	for (unsigned int i = 0; i < paneCount; i++) {
		out[i]  = layoutFor(&panes[i], paneCount, screenHeight);
		total  += out[i].width;
	}

	int x = ((int) TINROM_SCREEN_WIDTH - total) / 2;

	for (unsigned int i = 0; i < paneCount; i++) {
		out[i].originX = x;
		x += out[i].width + PANE_GAP;
	}
}

static bool cellSelectable(const UIGridPane *pane, unsigned int cell) {
	if (cell >= uiGridPaneCells(pane))
		return pane->menuLabel != NULL;

	if (!pane->available || !pane->cells)
		return false;

	switch (pane->cells[cell].kind) {
		case UI_GRID_ITEM:
		case UI_GRID_CONTINUED:
		case UI_GRID_DELETED:
			return true;

		default:
			return false;
	}
}

/*
 * Walk back to the cell an entry starts in. Continuation cells are laid out
 * in reading order, so this is a scan backwards to the first cell that is not
 * a continuation.
 */
static unsigned int entryHead(const UIGridPane *pane, unsigned int cell) {
	while (cell && pane->cells[cell].kind == UI_GRID_CONTINUED)
		cell--;

	return cell;
}

static uint32_t cellColor(const UIGridPane *pane, unsigned int cell) {
	if (!pane->available)
		return TINROM_COLOR_PANEL;

	/* A faded continuation cell keeps its entry's colour, not its kind's. */
	if (pane->cells[cell].faded)
		return TINROM_COLOR_WARNING;

	switch (pane->cells[cell].kind) {
		case UI_GRID_ITEM:
		case UI_GRID_CONTINUED:
			return TINROM_COLOR_HEADER;

		case UI_GRID_DELETED:
			return TINROM_COLOR_WARNING;

		default:
			return TINROM_COLOR_PANEL;
	}
}

/*
 * The "..." button. Drawn whether or not the pane is available, because a
 * card that failed to read is exactly when its menu is wanted.
 */
static void drawMenuButton(
	UIContext *ui, const UIGridPane *pane, const Layout *layout, bool focused
) {
	if (!pane->menuLabel)
		return;

	if (focused) {
		uiFillRect(
			ui,
			layout->originX - CURSOR_INSET, layout->menuY - CURSOR_INSET,
			layout->width + (CURSOR_INSET * 2),
			MENU_BUTTON_H + (CURSOR_INSET * 2),
			TINROM_COLOR_ACCENT
		);
	}

	uiFillRect(
		ui, layout->originX, layout->menuY, layout->width, MENU_BUTTON_H,
		focused ? TINROM_COLOR_SELECTION : TINROM_COLOR_PANEL
	);

	uiText(
		ui,
		layout->originX + ((layout->width - uiTextWidth(pane->menuLabel)) / 2),
		layout->menuY + 2,
		pane->menuLabel
	);
}

static void drawPane(
	UIContext        *ui,
	const UIGridPane *pane,
	const Layout     *layout,
	bool              focused,
	unsigned int      cursorCell
) {
	int  originX  = layout->originX;
	int  cellW    = layout->cellWidth, cellH = layout->cellHeight;
	int  gap      = layout->gap;
	bool onButton = focused && (cursorCell >= layout->cells);

	uiTextClipped(ui, originX, GRID_TOP - PANE_TITLE_H, layout->width,
		TINROM_COLOR_TEXT, pane->title);

	if (!pane->available || !pane->cells) {
		uiFillRect(
			ui, originX, GRID_TOP, layout->width, layout->bodyHeight,
			TINROM_COLOR_PANEL
		);
		uiTextWrapped(
			ui, originX + 6, GRID_TOP + 6, layout->width - 12,
			TINROM_COLOR_DIM,
			pane->unavailableReason ? pane->unavailableReason : "unavailable"
		);

		drawMenuButton(ui, pane, layout, onButton);

		return;
	}

	/*
	 * Cells never overlap one another, so each layer of them - fills,
	 * then icons and labels, then veils - shares one ordering-table bucket
	 * rather than taking one per cell. A 10x10 pane is some three hundred
	 * primitives, and the UI has 128 buckets a frame; past those, later
	 * draws (the screen's own title, say) would land underneath earlier
	 * ones. The cursor ring has a bucket of its own, behind its cell.
	 */
	if (focused && (cursorCell < layout->cells)) {
		int x = originX + (int) (cursorCell % layout->columns) * (cellW + gap);
		int y = GRID_TOP + (int) (cursorCell / layout->columns) * (cellH + gap);

		uiFillRect(
			ui,
			x - CURSOR_INSET, y - CURSOR_INSET,
			cellW + (CURSOR_INSET * 2),
			cellH + (CURSOR_INSET * 2),
			TINROM_COLOR_ACCENT
		);
	}

	for (unsigned int layer = 0; layer < 3; layer++) {
		uiShareBucket(ui);

		for (unsigned int cell = 0; cell < layout->cells; cell++) {
			const UIGridCell *c      = &pane->cells[cell];
			unsigned int      column = cell % layout->columns;
			unsigned int      row    = cell / layout->columns;

			int x = originX + (int) column * (cellW + gap);
			int y = GRID_TOP + (int) row * (cellH + gap);

			/*
			 * A continuation closes the gap to the cell before it, so a save
			 * that spans several blocks reads as one wide bar rather than
			 * three tiles that happen to be the same colour. It only joins
			 * within a row - there is nothing sensible to draw across a line
			 * break.
			 */
			int fillX     = x;
			int fillWidth = cellW;

			if ((c->kind == UI_GRID_CONTINUED) && column) {
				fillX     -= gap;
				fillWidth += gap;
			}

			switch (layer) {
				case 0:
					uiFillRect(ui, fillX, y, fillWidth, cellH,
						cellColor(pane, cell));
					break;

				case 1:
					/*
					 * The icon is the label when there is one. Saves written
					 * without a title frame, and any caller that does not use
					 * icons at all, fall back to the text - as does a cell
					 * too small for an icon.
					 */
					if ((c->icon >= 0) && (cellW >= UI_ICON_SIZE)
						&& (cellH >= UI_ICON_SIZE)
						&& uiIconPresent(ui, (unsigned int) c->icon)) {
						uiIconDraw(
							ui, (unsigned int) c->icon,
							x + ((cellW - UI_ICON_SIZE) / 2),
							y + ((cellH - UI_ICON_SIZE) / 2)
						);
					} else if (c->label) {
						uiTextClipped(ui, x + 2, y + ((cellH - 8) / 2),
							cellW - 3, TINROM_COLOR_TEXT, c->label);
					}
					break;

				default:
					/* Last, so it covers whichever of the two was drawn. */
					if (c->faded)
						uiVeilRect(ui, fillX, y, fillWidth, cellH,
							cellColor(pane, cell));
					break;
			}
		}

		uiShareBucketEnd(ui);
	}

	drawMenuButton(ui, pane, layout, onButton);
}

void uiGridCellAnchor(
	unsigned int pane,
	unsigned int paneCount,
	unsigned int cell,
	int         *x,
	int         *y
) {
	(void) paneCount;

	if (pane >= UI_GRID_MAX_PANES)
		pane = 0;

	const Layout *layout = &lastLayout[pane];

	if (!layout->columns) {
		*x = *y = 0;
		return;
	}

	if (cell >= layout->cells) {
		*x = layout->originX;
		*y = layout->menuY + MENU_BUTTON_H;

		return;
	}

	*x = layout->originX
		+ (int) (cell % layout->columns) * (layout->cellWidth + layout->gap);
	*y = GRID_TOP
		+ (int) (cell / layout->columns) * (layout->cellHeight + layout->gap)
		+ layout->cellHeight;
}

void uiGridDraw(
	UIContext          *ui,
	const UIGridPane   *panes,
	unsigned int        paneCount,
	const UIGridCursor *cursor
) {
	if (!paneCount)
		return;
	if (paneCount > UI_GRID_MAX_PANES)
		paneCount = UI_GRID_MAX_PANES;

	layoutAll(panes, paneCount, (int) ui->height, lastLayout);

	for (unsigned int i = 0; i < paneCount; i++)
		drawPane(
			ui, &panes[i], &lastLayout[i], i == cursor->pane, cursor->cell
		);
}

/*
 * Remembers which column the cursor came from when it dropped onto a pane's
 * button, so moving back up returns to the same place. One grid runs at a
 * time - uiGridRun() blocks - so a single value is enough.
 */
static unsigned int lastColumn;

#define DEFAULT_HINT "D-pad: move   Cross: select   Circle: back"

static const char *hint = DEFAULT_HINT;

void uiGridSetHint(const char *text) {
	hint = text ? text : DEFAULT_HINT;
}

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
) {
	UIContext *ui = &ctx->ui;

	if (!paneCount)
		return UI_GRID_OUTCOME_CANCELLED;
	if (paneCount > UI_GRID_MAX_PANES)
		paneCount = UI_GRID_MAX_PANES;

	if (cursor->pane >= paneCount)
		cursor->pane = 0;
	if ((cursor->cell >= uiGridPaneCells(&panes[cursor->pane]))
		&& (cursor->cell != UI_GRID_CELL_MENU))
		cursor->cell = 0;

	for (;;) {
		/*
		 * Before the frame, so a poll that takes its time - re-reading a card
		 * that has just been swapped, for instance - can draw its own
		 * progress frames without nesting inside this one.
		 */
		if (poll && poll(user)) {
			/*
			 * A pane the cursor was sitting in may have just gone away. Move
			 * to the first pane that is still usable and leave the cell where
			 * it was, so a card being pulled does not also lose the user's
			 * place on the other one. A cursor parked on a button stays put:
			 * the button survives its pane going unavailable.
			 */
			if (!panes[cursor->pane].available
				&& (cursor->cell != UI_GRID_CELL_MENU)) {
				for (unsigned int i = 0; i < paneCount; i++) {
					if (panes[i].available) {
						cursor->pane = i;
						break;
					}
				}

				/* Kept where it was, unless the new pane is smaller. */
				if (cursor->cell >= uiGridPaneCells(&panes[cursor->pane]))
					cursor->cell = 0;
			}
		}

		uiBeginFrame(ui);
		uiSetChrome(ui, title, hint);
		uiGridDraw(ui, panes, paneCount, cursor);

		if (describe) {
			const char *detail = describe(
				&panes[cursor->pane], cursor->cell, user
			);

			if (detail) {
				uiTextClipped(
					ui, TINROM_MARGIN_X + 4,
					(int) ui->height - TINROM_FOOTER_H - 16,
					(int) ui->width - (2 * (TINROM_MARGIN_X + 4)),
					TINROM_COLOR_TEXT, detail
				);
			}
		}

		uiEndFrame(ui);

		uint16_t pressed = tinromPadRepeated(0) | tinromPadRepeated(1);

		/*
		 * The button counts as one row below the last row of cells, and it
		 * spans the whole pane - so left and right there cross panes rather
		 * than moving a column.
		 */
		unsigned int columns  = paneColumns(&panes[cursor->pane]);
		unsigned int rows     = paneRows(&panes[cursor->pane]);
		bool         onButton = (cursor->cell == UI_GRID_CELL_MENU);
		unsigned int column   = onButton ? lastColumn
			: (cursor->cell % columns);
		unsigned int row      = onButton ? rows : (cursor->cell / columns);

		if (column >= columns)
			column = columns - 1;

		if ((pressed & TINROM_PAD_UP) && row)
			row--;

		if (pressed & TINROM_PAD_DOWN) {
			if ((row + 1) < rows)
				row++;
			else if ((row == (rows - 1)) && panes[cursor->pane].menuLabel)
				row = rows;
		}

		bool onButtonRow = (row >= rows);

		if (pressed & TINROM_PAD_LEFT) {
			if (!onButtonRow && column) {
				column--;
			} else if (cursor->pane) {
				/* Off the left edge and into the pane before it. */
				cursor->pane--;
				columns = paneColumns(&panes[cursor->pane]);

				if (!onButtonRow)
					column = columns - 1;
			}
		}
		if (pressed & TINROM_PAD_RIGHT) {
			if (!onButtonRow && ((column + 1) < columns)) {
				column++;
			} else if ((cursor->pane + 1) < paneCount) {
				cursor->pane++;
				columns = paneColumns(&panes[cursor->pane]);

				if (!onButtonRow)
					column = 0;
			}
		}

		/* The pane crossed into may be another shape, or have no button. */
		columns = paneColumns(&panes[cursor->pane]);
		rows    = paneRows(&panes[cursor->pane]);

		if (onButtonRow)
			row = panes[cursor->pane].menuLabel ? rows : (rows - 1);
		else if (row >= rows)
			row = rows - 1;

		if (column >= columns)
			column = columns - 1;

		if (row >= rows) {
			lastColumn   = column;
			cursor->cell = UI_GRID_CELL_MENU;
		} else {
			cursor->cell = (row * columns) + column;
		}

		if (pressed & TINROM_PAD_CIRCLE)
			return UI_GRID_OUTCOME_CANCELLED;

		if (pressed & TINROM_PAD_CROSS) {
			const UIGridPane *pane = &panes[cursor->pane];

			if (cursor->cell == UI_GRID_CELL_MENU) {
				if (pane->menuLabel)
					return UI_GRID_OUTCOME_MENU;
			} else if (cellSelectable(pane, cursor->cell)) {
				/* Always hand back the cell the entry starts in. */
				cursor->cell = entryHead(pane, cursor->cell);

				return UI_GRID_OUTCOME_SELECTED;
			}
		}
	}
}
