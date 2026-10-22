/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "core/app.h"
#include "core/result.h"
#include "hal/pad.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#define PANE_X (TINROM_MARGIN_X + 4)

/* Popup geometry. */
#define POPUP_PAD_X    6
#define POPUP_PAD_Y    4
#define POPUP_MIN_W   72
#define POPUP_EDGE     4
/* The summary line under a popup: its distance up from the footer. */
#define POPUP_SUMMARY_H 14

/* Message and question text, wrapped to the screen. */
#define BODY_Y     (TINROM_HEADER_H + 14)
#define BODY_WIDTH(ui) ((int) (ui)->width - (2 * PANE_X))

static void drawBody(UIContext *ui, const char *body) {
	uiTextWrapped(ui, PANE_X, BODY_Y, BODY_WIDTH(ui), TINROM_COLOR_TEXT, body);
}

static int popupWidth(
	const char *title, const UIMenuItem *items, size_t count
) {
	int widest = uiTextWidth(title);

	for (size_t i = 0; i < count; i++) {
		int width = uiTextWidth(items[i].label);

		if (width > widest)
			widest = width;
	}

	widest += POPUP_PAD_X * 2;

	return (widest < POPUP_MIN_W) ? POPUP_MIN_W : widest;
}

size_t uiPopupRun(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	int               anchorX,
	int               anchorY,
	void            (*background)(void *user),
	void             *user
) {
	return uiPopupRunAt(
		ctx, title, items, count, anchorX, anchorY, background, user, NULL
	);
}

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
) {
	UIContext *ui = &ctx->ui;

	if (!count)
		return UI_MENU_CANCELLED;

	int width  = popupWidth(title, items, count);
	int height = (int) ((count + 1) * TINROM_ROW_HEIGHT) + (POPUP_PAD_Y * 2);

	/* Nudge back on screen rather than clipping. */
	int x = anchorX;
	int y = anchorY;

	if ((x + width) > ((int) ui->width - POPUP_EDGE))
		x = (int) ui->width - POPUP_EDGE - width;
	if (x < POPUP_EDGE)
		x = POPUP_EDGE;

	/*
	 * The highlighted row's summary is drawn on a line of its own just above
	 * the footer; a panel nudged down that far would sit on top of it.
	 */
	int bottom = (int) ui->height - TINROM_FOOTER_H - POPUP_EDGE;

	for (size_t i = 0; i < count; i++) {
		if (items[i].summary) {
			bottom -= POPUP_SUMMARY_H;
			break;
		}
	}

	if ((y + height) > bottom)
		y = bottom - height;
	if (y < TINROM_HEADER_H + POPUP_EDGE)
		y = TINROM_HEADER_H + POPUP_EDGE;

	size_t selected = cursor ? *cursor : 0;

	if (selected >= count)
		selected = count - 1;

	/* Fall back to the first row the user can actually pick. */
	if (!items[selected].enabled) {
		selected = 0;

		while ((selected < count) && !items[selected].enabled)
			selected++;

		if (selected >= count)
			selected = 0;
	}

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, ui->title, "Cross: choose   Circle: back");

		if (background)
			background(user);

		/* Border, then the panel inside it, so the edge reads as a frame. */
		uiFillRect(ui, x - 1, y - 1, width + 2, height + 2,
			TINROM_COLOR_ACCENT);
		uiFillRect(ui, x, y, width, height, TINROM_COLOR_PANEL);
		uiFillRect(ui, x, y, width, TINROM_ROW_HEIGHT,
			TINROM_COLOR_HEADER);
		uiText(ui, x + POPUP_PAD_X, y + 2, title);

		int rowY = y + TINROM_ROW_HEIGHT + POPUP_PAD_Y;

		for (size_t i = 0; i < count; i++) {
			if (i == selected)
				uiFillRect(
					ui, x + 2, rowY - 2, width - 4, TINROM_ROW_HEIGHT,
					TINROM_COLOR_SELECTION
				);

			uiTextColored(
				ui, x + POPUP_PAD_X, rowY,
				items[i].enabled ? TINROM_COLOR_TEXT : TINROM_COLOR_DIM,
				items[i].label
			);

			rowY += TINROM_ROW_HEIGHT;
		}

		if (items[selected].summary) {
			uiTextClipped(
				ui, PANE_X, (int) ui->height - TINROM_FOOTER_H - POPUP_SUMMARY_H,
				BODY_WIDTH(ui), TINROM_COLOR_TEXT, items[selected].summary
			);
		}

		uiEndFrame(ui);

		uint16_t pressed = tinromPadRepeated(0) | tinromPadRepeated(1);

		if ((pressed & TINROM_PAD_UP) && selected)
			selected--;
		if ((pressed & TINROM_PAD_DOWN) && ((selected + 1) < count))
			selected++;

		if (cursor)
			*cursor = selected;

		if (pressed & TINROM_PAD_CIRCLE)
			return UI_MENU_CANCELLED;

		if ((pressed & TINROM_PAD_CROSS) && items[selected].enabled)
			return selected;
	}
}

void uiMessageBox(TinromContext *ctx, const char *title, const char *body) {
	UIContext *ui = &ctx->ui;

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, title, "Cross / Circle: back");
		drawBody(ui, body);
		uiEndFrame(ui);

		uint16_t pressed = tinromPadPressed(0) | tinromPadPressed(1);

		if (pressed & (TINROM_PAD_CROSS | TINROM_PAD_CIRCLE))
			return;
	}
}

void uiShowResult(TinromContext *ctx, const char *action, TinromResult result) {
	char buffer[96];

	snprintf(buffer, sizeof(buffer), "%s:\n%s", action,
		tinromResultString(result));
	uiMessageBox(ctx, (result == TINROM_OK) ? "Done" : "Failed", buffer);
}

void uiNotImplemented(TinromContext *ctx, const char *what) {
	char buffer[192];

	snprintf(
		buffer, sizeof(buffer),
		"%s is not implemented yet.\n\n"
		"This is a skeleton build. The module is registered, reachable "
		"and wired into settings, but its work function is a stub.\n\n"
		"See docs/roadmap.md.",
		what
	);
	uiMessageBox(ctx, what, buffer);
}

bool uiConfirm(
	TinromContext *ctx, const char *title, const char *body, bool danger
) {
	UIContext *ui       = &ctx->ui;
	bool       selected = !danger; /* dangerous prompts default to "no" */

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, title, "Left/Right: choose  X: confirm  O: cancel");
		drawBody(ui, body);

		int y = (int) ui->height - TINROM_FOOTER_H - 26;

		for (int button = 0; button < 2; button++) {
			const char *label = button ? "No" : "Yes";
			int         left  = button ? 160 : 40;

			if (selected == !button)
				uiFillRect(
					ui, left, y - 2, 80, 14,
					danger ? TINROM_COLOR_ERROR : TINROM_COLOR_SELECTION
				);

			uiText(ui, left + ((80 - uiTextWidth(label)) / 2), y + 1, label);
		}

		uiEndFrame(ui);

		uint16_t pressed = tinromPadRepeated(0) | tinromPadRepeated(1);

		if (pressed & (TINROM_PAD_LEFT | TINROM_PAD_RIGHT))
			selected = !selected;
		if (pressed & TINROM_PAD_CIRCLE)
			return false;
		if (pressed & TINROM_PAD_CROSS)
			return selected;
	}
}

void uiProgressBar(
	UIContext *ui, int x, int y, int width, uint32_t done, uint32_t total
) {
	int filled = 0;

	if (total) {
		/* Scale down first: avoids a 64-bit multiply for large transfers. */
		uint32_t permille = (uint32_t) ((done / ((total / 1000u) + 1u)));

		if (permille > 1000u)
			permille = 1000u;

		filled = (int) (((uint32_t) width * permille) / 1000u);
	}

	uiFillRect(ui, x, y, width,  10, TINROM_COLOR_PANEL);
	uiFillRect(ui, x, y, filled, 10, TINROM_COLOR_ACCENT);
}

void uiTextPane(
	TinromContext *ctx, const char *title,
	const char *(*lineAt)(unsigned int index), unsigned int lineCount
) {
	UIContext   *ui       = &ctx->ui;
	unsigned int scroll   = 0;
	unsigned int onScreen = 1;
	bool         hadModel = uiModelVisible(ui);

	/*
	 * A full page of text is the most expensive thing tinrom draws - a glyph
	 * costs five words out of the frame's 4096 - so the background model gets
	 * out of the way here. It is behind a wall of text anyway.
	 */
	uiSetModelVisible(ui, false);

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, title, "Up/Down: scroll   Circle: back");

		int          y       = TINROM_HEADER_H + 6;
		int          bottom  = (int) ui->height - TINROM_FOOTER_H - 2;
		int          width   = BODY_WIDTH(ui);
		unsigned int drawn   = 0;

		for (unsigned int index = scroll; index < lineCount; index++) {
			const char  *line  = lineAt(index);
			unsigned int lines = uiTextLines(line, width);

			/*
			 * A line too long for the screen wraps, and the rest of its
			 * entry follows on closer together than the next entry does.
			 */
			int height = (int) TINROM_ROW_HEIGHT
				+ (int) ((lines - 1) * UI_FONT_LINE_HEIGHT);

			if (drawn && ((y + height) > bottom))
				break;

			/*
			 * Stop at the line that will not fit rather than letting
			 * uiText() drop it silently - scrolling is driven by how many
			 * actually drew, so every line stays reachable however long
			 * the text is.
			 */
			if (!uiHasRoom(ui, uiTextCost(line) + 32))
				break;

			uiTextWrapped(ui, PANE_X, y, width, TINROM_COLOR_TEXT, line);
			y += height;
			drawn++;
		}

		/* Never zero, or Down would stop working on a very long line. */
		onScreen = drawn ? drawn : 1;

		uiEndFrame(ui);

		uint16_t pressed = tinromPadRepeated(0) | tinromPadRepeated(1);

		if ((pressed & TINROM_PAD_UP) && scroll)
			scroll--;
		if ((pressed & TINROM_PAD_DOWN) && (scroll + onScreen) < lineCount)
			scroll++;
		if (pressed & TINROM_PAD_CIRCLE) {
			uiSetModelVisible(ui, hadModel);

			return;
		}
	}
}
