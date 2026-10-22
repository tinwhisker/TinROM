/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"
#include "ui/menu.h"
#include "ui/ui.h"

typedef struct TinromContext TinromContext;

/*
 * Small blocking helpers modules use so every screen behaves the same way.
 * Each runs its own frame loop and returns when the user answers.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Shows text and waits for Cross/Circle. */
void uiMessageBox(TinromContext *ctx, const char *title, const char *body);

/** Renders a TinromResult the same way everywhere. */
void uiShowResult(TinromContext *ctx, const char *action, TinromResult result);

/** The standard "this module is a stub" screen. */
void uiNotImplemented(TinromContext *ctx, const char *what);

/** Returns true for yes. Destructive actions should pass danger = true. */
bool uiConfirm(
	TinromContext *ctx, const char *title, const char *body, bool danger
);

/** A progress bar driven by a caller-owned counter. Does not block. */
void uiProgressBar(
	UIContext *ui, int x, int y, int width, uint32_t done, uint32_t total
);

/**
 * A small menu panel drawn over whatever `background` paints, for actions
 * that belong to one thing the user has just picked rather than to a screen.
 * Blocking, like every other widget here.
 *
 * `anchorX` / `anchorY` are where its top-left would like to be; the panel is
 * nudged back on screen if that would push it off. `background` is called
 * once per frame inside the widget's own frame, so the screen underneath
 * stays live - an animating icon keeps animating.
 *
 * Returns the chosen index, or UI_MENU_CANCELLED.
 */
size_t uiPopupRun(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	int               anchorX,
	int               anchorY,
	void            (*background)(void *user),
	void             *user
);

/**
 * The same, opening on `*cursor` rather than on the first pickable row, and
 * leaving the row it closed on there.
 *
 * A popup that is choosing a value - rather than choosing an action - should
 * open on the value that is already set, which is what this is for. The row
 * is clamped to the list, and if it lands on a disabled one the first
 * pickable row is used instead.
 */
size_t uiPopupRunAt(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	int               anchorX,
	int               anchorY,
	void            (*background)(void *user),
	void             *user,
	size_t           *cursor
);

/** Scrolling text pane, e.g. the log viewer. */
void uiTextPane(
	TinromContext *ctx, const char *title,
	const char *(*lineAt)(unsigned int index), unsigned int lineCount
);

#ifdef __cplusplus
}
#endif
