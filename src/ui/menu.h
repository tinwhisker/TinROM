/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/ui.h"

typedef struct TinromContext TinromContext;

typedef struct {
	const char *label;
	const char *summary;
	bool        enabled;
	/* Opaque tag the caller uses to identify the row it got back. */
	uintptr_t   tag;
} UIMenuItem;

#define UI_MENU_CANCELLED ((size_t) -1)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs a blocking list menu. Returns the index of the chosen item, or
 * UI_MENU_CANCELLED if the user backed out. The cursor starts at the top.
 */
size_t uiMenuRun(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count
);

/**
 * The same, but the caller owns the highlighted row: `*cursor` is both where
 * the cursor starts and where it is left, including when the user backs out.
 *
 * Any screen that loops - show the list, act on a choice, show it again -
 * wants this rather than uiMenuRun, because otherwise every action throws the
 * user back to the top of the list. Toggling a row is the obvious case: the
 * thing you just changed should still be under the cursor.
 *
 * The remembered row is clamped to the list rather than trusted, so a list
 * that has shrunk since last time still opens somewhere sensible.
 */
size_t uiMenuRunAt(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	size_t           *cursor
);

/**
 * Repaints the list exactly as uiMenuRunAt last drew it - same rows, same
 * scroll - for use as a popup's background painter, so a context menu opened
 * over a settings row leaves the screen underneath looking untouched.
 */
void uiMenuDraw(
	UIContext *ui, const UIMenuItem *items, size_t count, size_t selected
);

/**
 * Bottom-left of row `row` as the list last drew it, for anchoring a popup
 * under the row it belongs to. Accounts for the list's scroll.
 */
void uiMenuRowAnchor(size_t row, int *x, int *y);

/** The main menu: builds itself from the module registry every frame. */
void uiMenuRunMain(TinromContext *ctx);

#ifdef __cplusplus
}
#endif
