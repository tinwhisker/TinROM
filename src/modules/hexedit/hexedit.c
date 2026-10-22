/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The hex editor: picking a region, moving around it, and editing.
 *
 * Browsing:  D-pad moves, L1/R1 a page, L2/R2 further. Square cycles bytes,
 *            halfwords and words. Cross edits. Triangle opens the menu.
 *            Start finds the next match, Select goes back after following
 *            a pointer. Circle leaves.
 * Editing:   Up/Down roll the selected digit, Left/Right move between
 *            digits and on to the next unit, which is when the unit is
 *            written. Cross finishes, Circle drops the unit's change.
 *
 * Drawing is view.c, cards card.c, searching find.c, number entry dial.c.
 * See docs/hexedit.md.
 */

#include <stdio.h>
#include "libc/string.h"
#include "modules/hexedit/hexedit.h"

static HexEditor editor;

static uint16_t pressedAny(void) {
	return tinromPadPressed(0) | tinromPadPressed(1);
}

static uint16_t repeatedAny(void) {
	return tinromPadRepeated(0) | tinromPadRepeated(1);
}

/* --- moving ----------------------------------------------------------------- */

bool hexMoveTo(TinromContext *ctx, HexEditor *ed, uint32_t offset) {
	const HexRegion *region = ed->region;
	uint32_t         last   = region->size - ed->unit;

	if (offset > last)
		offset = last;

	offset &= ~(uint32_t) (ed->unit - 1);

	if (region->kind == HEX_REGION_CARD) {
		uint32_t frame = offset / HEX_PAGE_BYTES;

		if ((int32_t) frame != ed->frameNumber) {
			if (!hexCardLeave(ctx, ed))
				return false;

			hexCardLoad(ed, frame);
		}

		ed->view = frame * HEX_PAGE_BYTES;
	} else {
		if (offset < ed->view)
			ed->view = offset & ~(uint32_t) (HEX_ROW_BYTES - 1);
		else if (offset >= (ed->view + HEX_PAGE_BYTES))
			ed->view = (offset & ~(uint32_t) (HEX_ROW_BYTES - 1))
				- (HEX_PAGE_BYTES - HEX_ROW_BYTES);

		if (ed->view > (region->size - HEX_PAGE_BYTES))
			ed->view = region->size - HEX_PAGE_BYTES;
	}

	ed->cursor = offset;

	return true;
}

static void moveBy(TinromContext *ctx, HexEditor *ed, int32_t delta) {
	int64_t target = (int64_t) ed->cursor + delta;

	if (target < 0)
		target = 0;

	hexMoveTo(ctx, ed, (uint32_t) target);
}

/* --- values --------------------------------------------------------------- */

static uint32_t valueAtCursor(const HexEditor *ed) {
	uint32_t index = ed->cursor - ed->view;
	uint32_t value = 0;

	for (unsigned int b = 0; b < ed->unit; b++)
		value |= (uint32_t) ed->bytes[index + b] << (b * 8);

	return value;
}

static void remember(HexEditor *ed, uint32_t offset, unsigned size,
	uint32_t old) {
	if (ed->undoCount == HEX_UNDO_DEPTH) {
		memmove(&ed->undo[0], &ed->undo[1],
			sizeof(ed->undo[0]) * (HEX_UNDO_DEPTH - 1));
		ed->undoCount--;
	}

	ed->undo[ed->undoCount].offset = offset;
	ed->undo[ed->undoCount].value  = old;
	ed->undo[ed->undoCount].size   = (uint8_t) size;
	ed->undoCount++;
}

/* Puts `value` at `offset`, in memory or in the card frame being edited. */
static bool store(HexEditor *ed, uint32_t offset, unsigned size,
	uint32_t value) {
	if (ed->region->kind == HEX_REGION_MEMORY)
		return hexWrite(ed->region, offset, size, value);

	uint32_t index = offset - ed->view;

	if (!ed->frameLoaded || ((index + size) > HEX_PAGE_BYTES))
		return false;

	for (unsigned int b = 0; b < size; b++) {
		uint8_t byte = (uint8_t) (value >> (b * 8));

		if (ed->frame[index + b] != byte) {
			ed->frame[index + b] = byte;
			hexCardMark(ed, index + b);
		}
	}

	return true;
}

static void commit(HexEditor *ed) {
	uint32_t old = valueAtCursor(ed);

	if (ed->pending == old)
		return;

	if (store(ed, ed->cursor, ed->unit, ed->pending)) {
		remember(ed, ed->cursor, ed->unit, old);
	} else {
		snprintf(ed->message, sizeof(ed->message), "could not write %08X",
			(unsigned int) (ed->region->base + ed->cursor));
	}

	/* Show the result straight away, not a frame late. */
	hexViewRefresh(ed);
}

static void undoLast(HexEditor *ed) {
	if (!ed->undoCount) {
		snprintf(ed->message, sizeof(ed->message), "nothing to undo");

		return;
	}

	const HexUndo *entry = &ed->undo[--ed->undoCount];

	store(ed, entry->offset, entry->size, entry->value);
	snprintf(ed->message, sizeof(ed->message), "undid the write at %08X",
		(unsigned int) (ed->region->base + entry->offset));
	hexViewRefresh(ed);
}

/* --- editing ---------------------------------------------------------------- */

/*
 * Some RAM is tinrom's own - its code, the kernel it runs on, the debugger's
 * monitor. Editing it is allowed, and asked about once per kind per visit.
 */
static bool mayEditHere(TinromContext *ctx, HexEditor *ed) {
	if (!ed->region->writable) {
		snprintf(ed->message, sizeof(ed->message), "%s is read only",
			ed->region->name);

		return false;
	}

	const char *owner = hexRamOwner(ed->region, ed->cursor);

	if (!owner)
		return true;

	uint32_t bit = (owner[0] == 'k') ? 1 : ((owner[0] == 't') ? 2 : 4);

	if (ed->warned & bit)
		return true;

	char body[160];

	snprintf(body, sizeof(body),
		"This address belongs to the %s.\n\n"
		"Changing it can crash tinrom or lose the "
		"debugger. Edit it anyway?", owner);

	if (!uiConfirm(ctx, "Edit", body, true))
		return false;

	ed->warned |= bit;

	return true;
}

static void beginEdit(TinromContext *ctx, HexEditor *ed) {
	if (!ed->readable || !mayEditHere(ctx, ed))
		return;

	ed->editing = true;
	ed->nibble  = 0;
	ed->pending = valueAtCursor(ed);
}

static void editInput(TinromContext *ctx, HexEditor *ed, uint16_t pressed,
	uint16_t repeated) {
	unsigned digits = ed->unit * 2;
	unsigned shift  = (digits - 1 - ed->nibble) * 4;
	uint32_t digit  = (ed->pending >> shift) & 15;

	if (repeated & (TINROM_PAD_UP | TINROM_PAD_DOWN)) {
		digit = (repeated & TINROM_PAD_UP) ? ((digit + 1) & 15)
			: ((digit + 15) & 15);
		ed->pending = (ed->pending & ~(15u << shift)) | (digit << shift);
	}

	if (repeated & TINROM_PAD_RIGHT) {
		if ((ed->nibble + 1) < digits) {
			ed->nibble++;
		} else if ((ed->cursor + ed->unit) < ed->region->size) {
			/* Leaving the unit is what writes it. */
			commit(ed);

			if (hexMoveTo(ctx, ed, ed->cursor + ed->unit)) {
				hexViewRefresh(ed);
				ed->nibble  = 0;
				ed->pending = valueAtCursor(ed);
			}
		}
	}

	if (repeated & TINROM_PAD_LEFT) {
		if (ed->nibble) {
			ed->nibble--;
		} else if (ed->cursor) {
			commit(ed);

			if (hexMoveTo(ctx, ed, ed->cursor - ed->unit)) {
				hexViewRefresh(ed);
				ed->nibble  = digits - 1;
				ed->pending = valueAtCursor(ed);
			}
		}
	}

	if (pressed & TINROM_PAD_CROSS) {
		commit(ed);
		ed->editing = false;
	}

	if (pressed & TINROM_PAD_CIRCLE)
		ed->editing = false;
}

/* --- going places ------------------------------------------------------------ */

static void pushBack(HexEditor *ed) {
	if (ed->backCount == HEX_BACK_DEPTH) {
		memmove(&ed->back[0], &ed->back[1],
			sizeof(ed->back[0]) * (HEX_BACK_DEPTH - 1));
		ed->backCount--;
	}

	ed->back[ed->backCount++] = ed->cursor;
}

static void goBack(TinromContext *ctx, HexEditor *ed) {
	if (!ed->backCount) {
		snprintf(ed->message, sizeof(ed->message), "nowhere to go back to");

		return;
	}

	hexMoveTo(ctx, ed, ed->back[--ed->backCount]);
}

/* The word under the cursor, read as an address. */
static void followPointer(TinromContext *ctx, HexEditor *ed) {
	uint32_t index   = (ed->cursor - ed->view) & ~3u;
	uint32_t address = 0, offset;

	for (unsigned int b = 0; b < 4; b++)
		address |= (uint32_t) ed->bytes[index + b] << (b * 8);

	if (!hexAddressToOffset(ed->region, address, &offset)) {
		snprintf(ed->message, sizeof(ed->message), "%08X is not in %s",
			(unsigned int) address, ed->region->name);

		return;
	}

	pushBack(ed);
	hexMoveTo(ctx, ed, offset);
}

static void goToAddress(TinromContext *ctx, HexEditor *ed) {
	bool     card    = ed->region->kind == HEX_REGION_CARD;
	uint32_t address = ed->region->base + ed->cursor;
	uint32_t offset;

	if (!hexDialHex(ctx, ed, card ? "Go to offset" : "Go to address",
		card ? 5 : 8, &address))
		return;

	if (!hexAddressToOffset(ed->region, address, &offset)) {
		snprintf(ed->message, sizeof(ed->message), "%08X is not in %s",
			(unsigned int) address, ed->region->name);

		return;
	}

	pushBack(ed);
	hexMoveTo(ctx, ed, offset);
}

static void menu(TinromContext *ctx, HexEditor *ed) {
	bool card = ed->region->kind == HEX_REGION_CARD;

	const UIMenuItem items[] = {
		{ "Go to address",     "", true, 0 },
		{ "Find",              "", true, 1 },
		{ "Find next",         "", ed->find.valid, 2 },
		{ "Follow pointer",    "", true, 3 },
		{ "Go back",           "", ed->backCount > 0, 4 },
		{ "Undo last write",   "", ed->undoCount > 0, 5 },
		{ "Start of region",   "", true, 6 },
		{ "End of region",     "", true, 7 },
		{ "Write frame to card", "", card && hexCardDirty(ed), 8 },
		{ "Reload frame",      "", card, 9 }
	};
	static size_t cursor = 0;

	size_t count  = card ? 10 : 8;
	size_t choice = uiPopupRunAt(ctx, ed->region->name, items, count, 150, 40,
		hexViewBackground, ed, &cursor);

	if (choice == UI_MENU_CANCELLED)
		return;

	switch (items[choice].tag) {
		case 0: goToAddress(ctx, ed); break;
		case 1: hexFindPrompt(ctx, ed); break;
		case 2: hexFindNext(ctx, ed); break;
		case 3: followPointer(ctx, ed); break;
		case 4: goBack(ctx, ed); break;
		case 5: undoLast(ed); break;

		case 6:
			pushBack(ed);
			hexMoveTo(ctx, ed, 0);
			break;

		case 7:
			pushBack(ed);
			hexMoveTo(ctx, ed, ed->region->size - 1);
			break;

		case 8:
			hexCardWriteBack(ctx, ed);
			break;

		default:
			if (!hexCardDirty(ed) || uiConfirm(ctx, "Reload",
				"Drop the edits to this frame and read it "
				"from the card again?", true))
				hexCardLoad(ed, (uint32_t) ed->frameNumber);
			break;
	}
}

/* --- one region -------------------------------------------------------------- */

static void browse(TinromContext *ctx, const HexRegion *region) {
	HexEditor *ed   = &editor;
	HexFind    find = ed->find; /* a search carries over between regions */

	memset(ed, 0, sizeof(*ed));
	ed->region      = region;
	ed->unit        = 1;
	ed->frameNumber = -1;
	ed->find        = find;

	if (region->kind == HEX_REGION_CARD) {
		TinromMcInfo info;
		TinromResult result = tinromMcProbe(region->port, &info);

		if (result != TINROM_OK) {
			uiShowResult(ctx, region->name, result);

			return;
		}

		hexCardLoad(ed, 0);
	} else if (region->base == 0x80000000u) {
		/* Somewhere more interesting than the exception vectors. */
		ed->view   = 0x10000;
		ed->cursor = 0x10000;
	}

	hexViewInit();

	/*
	 * The model spinning behind the menu would show through the gaps in the
	 * canvas and fight the hex for attention, so it is hidden while the
	 * editor is open - the same thing uiTextPane() does - and put back as
	 * it was on the way out.
	 */
	UIContext *ui       = &ctx->ui;
	bool       hadModel = uiModelVisible(ui);

	uiSetModelVisible(ui, false);

	for (;;) {
		hexViewRefresh(ed);

		uiBeginFrame(ui);
		uiSetChrome(ui, "Hex editor", ed->editing
			? "Up/Dn: digit  L/R: move  X: done  O: cancel"
			: "X: edit  []: size  /\\: menu  L1/R1: page  O: back");
		hexViewDraw(ctx, ed);
		uiEndFrame(ui);

		uint16_t pressed  = pressedAny();
		uint16_t repeated = repeatedAny();

		if ((pressed | repeated) && !ed->editing)
			ed->message[0] = 0;

		if (ed->editing) {
			editInput(ctx, ed, pressed, repeated);
			continue;
		}

		if (repeated & TINROM_PAD_UP)    moveBy(ctx, ed, -HEX_ROW_BYTES);
		if (repeated & TINROM_PAD_DOWN)  moveBy(ctx, ed,  HEX_ROW_BYTES);
		if (repeated & TINROM_PAD_LEFT)  moveBy(ctx, ed, -(int32_t) ed->unit);
		if (repeated & TINROM_PAD_RIGHT) moveBy(ctx, ed,  (int32_t) ed->unit);
		if (repeated & TINROM_PAD_L1)    moveBy(ctx, ed, -HEX_PAGE_BYTES);
		if (repeated & TINROM_PAD_R1)    moveBy(ctx, ed,  HEX_PAGE_BYTES);
		if (repeated & TINROM_PAD_L2)
			moveBy(ctx, ed, -(int32_t) region->bigStep);
		if (repeated & TINROM_PAD_R2)
			moveBy(ctx, ed,  (int32_t) region->bigStep);

		if (pressed & TINROM_PAD_SQUARE) {
			ed->unit = (ed->unit == 4) ? 1 : (ed->unit * 2);
			hexMoveTo(ctx, ed, ed->cursor);
		}

		if (pressed & TINROM_PAD_CROSS)
			beginEdit(ctx, ed);
		if (pressed & TINROM_PAD_TRIANGLE)
			menu(ctx, ed);
		if (pressed & TINROM_PAD_START)
			hexFindNext(ctx, ed);
		if (pressed & TINROM_PAD_SELECT)
			goBack(ctx, ed);

		if (pressed & TINROM_PAD_CIRCLE) {
			if ((region->kind != HEX_REGION_CARD) || hexCardLeave(ctx, ed)) {
				uiSetModelVisible(ui, hadModel);

				return;
			}
		}
	}
}

static TinromResult hexEditEnter(TinromContext *ctx) {
	static UIMenuItem items[8];
	static size_t     cursor = 0;

	for (unsigned int i = 0; i < hexRegionCount; i++) {
		items[i].label   = hexRegions[i].name;
		items[i].summary = hexRegions[i].summary;
		items[i].enabled = true;
		items[i].tag     = i;
	}

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Hex editor", items, hexRegionCount,
			&cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		browse(ctx, &hexRegions[items[choice].tag]);
	}
}

const TinromModule tinromModule_hexedit = {
	.id      = "hexedit",
	.name    = "Hex editor",
	.summary = "View and patch RAM, ROM, cart and cards",
	.minTier = TINROM_TIER_DEVELOPER,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = hexEditEnter,
	.tick    = NULL,
	.shutdown = NULL
};
