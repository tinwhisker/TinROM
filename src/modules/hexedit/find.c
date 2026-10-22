/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Searching forwards from the cursor for a value or a run of text.
 *
 * Values are compared as the CPU would load them - little-endian, at an
 * address aligned to their size - so a word search finds what a program
 * reading that word would see. Text matches byte for byte, anywhere.
 */

#include <stdio.h>
#include "libc/string.h"
#include "modules/hexedit/hexedit.h"

static unsigned int patternLength(const HexFind *find) {
	return (find->kind == HEX_FIND_TEXT)
		? (unsigned int) strlen(find->text) : (unsigned int) find->kind;
}

static unsigned int patternStep(const HexFind *find) {
	return (find->kind == HEX_FIND_TEXT) ? 1 : (unsigned int) find->kind;
}

static bool matchesAt(const HexFind *find, const uint8_t *bytes) {
	if (find->kind == HEX_FIND_TEXT)
		return !memcmp(bytes, find->text, strlen(find->text));

	uint32_t value = 0;

	for (unsigned int i = 0; i < (unsigned int) find->kind; i++)
		value |= (uint32_t) bytes[i] << (i * 8);

	return value == find->value;
}

/* A frame on screen now and then, so a long search does not look hung. */
static bool progress(TinromContext *ctx, HexEditor *ed, uint32_t done,
	uint32_t total) {
	UIContext *ui = &ctx->ui;

	uiBeginFrame(ui);
	uiSetChrome(ui, ui->title, "Circle: stop searching");
	hexViewDraw(ctx, ed);
	uiFillRect(ui, 58, 100, 204, 30, TINROM_COLOR_PANEL);
	uiText(ui, 64, 104, "Searching...");
	uiProgressBar(ui, 64, 118, 192, done, total);
	uiEndFrame(ui);

	return !((tinromPadPressed(0) | tinromPadPressed(1)) & TINROM_PAD_CIRCLE);
}

static bool searchMemory(TinromContext *ctx, HexEditor *ed, uint32_t start,
	uint32_t *found) {
	const HexRegion *region = ed->region;
	unsigned int     length = patternLength(&ed->find);
	unsigned int     step   = patternStep(&ed->find);
	uint8_t          window[HEX_FIND_TEXT_MAX];

	start = (start + step - 1) & ~(step - 1);

	uint8_t first = (ed->find.kind == HEX_FIND_TEXT)
		? (uint8_t) ed->find.text[0] : (uint8_t) ed->find.value;

	for (uint32_t offset = start; (offset + length) <= region->size;
		offset += step) {
		uint32_t lead = 0;

		/* Most places fail on the first byte; check that on its own. */
		hexRead(region, offset, 1, &lead);

		if ((uint8_t) lead != first) {
			if (!(offset & 0xffff) && (offset != start)
				&& !progress(ctx, ed, offset, region->size))
				return false;

			continue;
		}

		for (unsigned int i = 0; i < length; i++) {
			uint32_t byte = 0;

			hexRead(region, offset + i, 1, &byte);
			window[i] = (uint8_t) byte;
		}

		if (matchesAt(&ed->find, window)) {
			*found = offset;

			return true;
		}

		/* 64 KB at a time between progress frames. */
		if (!(offset & 0xffff) && (offset != start)
			&& !progress(ctx, ed, offset, region->size))
			return false;
	}

	return false;
}

/*
 * A card is read a frame at a time, and a match may straddle two frames, so
 * the tail of each frame is carried into the next one's window.
 */
static bool searchCard(TinromContext *ctx, HexEditor *ed, uint32_t start,
	uint32_t *found) {
	const HexRegion *region = ed->region;
	unsigned int     length = patternLength(&ed->find);
	unsigned int     step   = patternStep(&ed->find);
	static uint8_t   window[HEX_FIND_TEXT_MAX + HEX_PAGE_BYTES];
	unsigned int     carried = 0;
	uint32_t         frames  = region->size / HEX_PAGE_BYTES;

	start = (start + step - 1) & ~(step - 1);

	for (uint32_t frame = start / HEX_PAGE_BYTES; frame < frames; frame++) {
		uint8_t *data = window + carried;

		/* The frame being edited is searched as edited. */
		if (ed->frameLoaded && ((int32_t) frame == ed->frameNumber)) {
			memcpy(data, ed->frame, HEX_PAGE_BYTES);
		} else if (tinromMcReadFrame(region->port, (uint16_t) frame, data)
			!= TINROM_OK) {
			snprintf(ed->message, sizeof(ed->message),
				"could not read frame %03X", (unsigned int) frame);

			return false;
		}

		uint32_t windowStart = frame * HEX_PAGE_BYTES - carried;
		unsigned int total   = carried + HEX_PAGE_BYTES;

		for (unsigned int i = 0; (i + length) <= total; i++) {
			uint32_t offset = windowStart + i;

			if ((offset < start) || (offset % step))
				continue;

			if (matchesAt(&ed->find, &window[i])) {
				*found = offset;

				return true;
			}
		}

		carried = (length > 1) ? (length - 1) : 0;
		memmove(window, window + total - carried, carried);

		if (!(frame % 16) && !progress(ctx, ed, frame, frames))
			return false;
	}

	return false;
}

static bool run(TinromContext *ctx, HexEditor *ed, uint32_t start) {
	uint32_t found;
	bool     hit = (ed->region->kind == HEX_REGION_CARD)
		? searchCard(ctx, ed, start, &found)
		: searchMemory(ctx, ed, start, &found);

	if (!hit) {
		if (!ed->message[0])
			snprintf(ed->message, sizeof(ed->message),
				"not found before the end");

		return false;
	}

	if (!hexMoveTo(ctx, ed, found))
		return false;

	snprintf(ed->message, sizeof(ed->message), "found at %08X",
		(unsigned int) (ed->region->base + found));

	return true;
}

bool hexFindPrompt(TinromContext *ctx, HexEditor *ed) {
	static const UIMenuItem kinds[] = {
		{ "Byte",     "", true, HEX_FIND_BYTE },
		{ "Halfword", "", true, HEX_FIND_HALF },
		{ "Word",     "", true, HEX_FIND_WORD },
		{ "Text",     "", true, HEX_FIND_TEXT }
	};
	static size_t kindCursor = 0;

	size_t choice = uiPopupRunAt(ctx, "Find", kinds,
		sizeof(kinds) / sizeof(kinds[0]), 120, 70, hexViewBackground, ed,
		&kindCursor);

	if (choice == UI_MENU_CANCELLED)
		return false;

	HexFind find = ed->find;

	find.kind = (HexFindKind) kinds[choice].tag;

	if (find.kind == HEX_FIND_TEXT) {
		if (!hexDialText(ctx, ed, "Find text", find.text, HEX_FIND_TEXT_MAX))
			return false;
	} else {
		unsigned digits = (unsigned int) find.kind * 2;

		if (digits < 8)
			find.value &= (1u << (digits * 4)) - 1;

		if (!hexDialHex(ctx, ed, "Find value", digits, &find.value))
			return false;
	}

	find.valid = true;
	ed->find   = find;

	return run(ctx, ed, ed->cursor);
}

bool hexFindNext(TinromContext *ctx, HexEditor *ed) {
	if (!ed->find.valid)
		return hexFindPrompt(ctx, ed);

	return run(ctx, ed, ed->cursor + 1);
}
