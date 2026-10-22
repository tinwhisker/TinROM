/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "core/app.h"
#include "core/log.h"
#include "core/module.h"
#include "core/profile.h"
#include "hal/pad.h"
#include "hal/timer.h"
#include "tinrom/version.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"

#define MENU_X      (TINROM_MARGIN_X + 4)
#define MENU_TOP    (TINROM_HEADER_H + 8)
#define MENU_MAX_ROWS 32

/*
 * What the list last drew, so a popup opened over it can repaint the same
 * rows behind itself and so a caller can anchor that popup under the row it
 * belongs to. One menu is on screen at a time, which is what makes a file
 * static honest here.
 */
static size_t menuScroll;
static size_t menuVisible;

static void drawList(
	UIContext        *ui,
	const UIMenuItem *items,
	size_t            count,
	size_t            selected,
	size_t            scroll,
	size_t            visible,
	bool              withSummary
) {
	int y = MENU_TOP;

	for (size_t i = 0; i < visible; i++) {
		size_t index = scroll + i;

		if (index >= count)
			break;

		if (index == selected) {
			uiFillRect(
				ui, MENU_X - 4, y - 2, (int) ui->width - (2 * MENU_X) + 8,
				TINROM_ROW_HEIGHT, TINROM_COLOR_SELECTION
			);
		}

		if (items[index].enabled) {
			uiText(ui, MENU_X, y, items[index].label);
		} else {
			static const char unavailable[] = "unavailable";

			uiTextColored(ui, MENU_X, y, TINROM_COLOR_DIM, items[index].label);
			uiTextColored(
				ui, (int) ui->width - MENU_X - uiTextWidth(unavailable), y,
				TINROM_COLOR_DIM, unavailable
			);
		}

		y += TINROM_ROW_HEIGHT;
	}

	/* Summary of the highlighted row, above the footer. A popup drawn over
	 * this list puts its own summary on that line, so it asks for the list
	 * to be repainted without one rather than have the two overlap. */
	if (withSummary && (selected < count) && items[selected].summary) {
		uiTextClipped(
			ui, MENU_X, (int) ui->height - TINROM_FOOTER_H - 14,
			(int) ui->width - (2 * MENU_X), TINROM_COLOR_TEXT,
			items[selected].summary
		);
	}
}

size_t uiMenuRun(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count
) {
	return uiMenuRunAt(ctx, title, items, count, NULL);
}

/*
 * `isMain` is the main menu, which also follows the cart switch: a flip that
 * swaps the profile ends this run, as backing out does, and the caller's
 * loop builds the menu again from the new one.
 */
static size_t runMenu(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	size_t           *cursor,
	bool              isMain
) {
	UIContext *ui     = &ctx->ui;
	size_t     scroll = 0;

	if (!count)
		return UI_MENU_CANCELLED;

	size_t selected = cursor ? *cursor : 0;

	/* The list may be shorter than it was when the row was remembered. */
	if (selected >= count)
		selected = count - 1;

	for (;;) {
		size_t visible = (ui->height - MENU_TOP - TINROM_FOOTER_H - 18)
			/ TINROM_ROW_HEIGHT;

		if (visible > MENU_MAX_ROWS)
			visible = MENU_MAX_ROWS;
		if (selected < scroll)
			scroll = selected;
		if (selected >= (scroll + visible))
			scroll = selected - visible + 1;

		uiBeginFrame(ui);
		uiSetChrome(ui, title, "Up/Down: move   Cross: select   Circle: back");
		menuScroll  = scroll;
		menuVisible = visible;

		drawList(ui, items, count, selected, scroll, visible, true);
		uiEndFrame(ui);

		uint16_t pressed = tinromPadRepeated(0) | tinromPadRepeated(1);

		if ((pressed & TINROM_PAD_UP) && selected)
			selected--;
		if ((pressed & TINROM_PAD_DOWN) && (selected + 1) < count)
			selected++;
		if (cursor)
			*cursor = selected;

		if (pressed & TINROM_PAD_CIRCLE)
			return UI_MENU_CANCELLED;

		if (pressed & TINROM_PAD_CROSS) {
			if (items[selected].enabled)
				return selected;
		}

		tinromModulesTick(ctx);
		ctx->frame++;

		if (isMain && tinromSwitchPoll(&ctx->settings, tinromFrameCounter()))
			return UI_MENU_CANCELLED;
	}
}

size_t uiMenuRunAt(
	TinromContext    *ctx,
	const char       *title,
	const UIMenuItem *items,
	size_t            count,
	size_t           *cursor
) {
	return runMenu(ctx, title, items, count, cursor, false);
}

/*
 * The main menu is rebuilt and re-run every time a module returns, so its row
 * has to outlive the call rather than the loop. Static rather than passed in:
 * there is exactly one main menu, and app.c should not have to hold its
 * cursor for it.
 */
static size_t mainCursor;

void uiMenuDraw(
	UIContext *ui, const UIMenuItem *items, size_t count, size_t selected
) {
	drawList(ui, items, count, selected, menuScroll, menuVisible, false);
}

void uiMenuRowAnchor(size_t row, int *x, int *y) {
	if (x)
		*x = MENU_X - 4;

	if (y) {
		size_t visualRow = (row > menuScroll) ? (row - menuScroll) : 0;

		*y = MENU_TOP
			+ (int) (visualRow * TINROM_ROW_HEIGHT)
			+ TINROM_ROW_HEIGHT;
	}
}

void uiMenuRunMain(TinromContext *ctx) {
	UIMenuItem items[MENU_MAX_ROWS];
	size_t     itemIndices[MENU_MAX_ROWS];
	size_t     count = 0;

	for (size_t i = 0; i < tinromModuleCount() && count < MENU_MAX_ROWS; i++) {
		if (!tinromModuleVisible(ctx, i))
			continue;

		const TinromModule *module = tinromModuleAt(i);

		items[count].label   = module->name;
		items[count].summary = module->summary;
		items[count].enabled = tinromModuleAvailable(i) && module->enter;
		items[count].tag     = i;
		itemIndices[count]   = i;
		count++;
	}

	char        title[80];
	const char *position = tinromSwitchLabel();

	snprintf(
		title, sizeof(title), "%s %s  -  %s%s%s",
		TINROM_NAME, tinromVersionFull,
		tinromTierName(ctx->settings.tier),
		position ? ", " : "", position ? position : ""
	);

	size_t choice = runMenu(ctx, title, items, count, &mainCursor, true);

	if (choice == UI_MENU_CANCELLED)
		return;

	const TinromModule *module = tinromModuleAt(itemIndices[choice]);

	if (module && module->enter) {
		TinromResult result = module->enter(ctx);

		if (result != TINROM_OK && result != TINROM_ERR_ABORTED)
			uiShowResult(ctx, module->name, result);
	}
}
