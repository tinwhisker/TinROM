/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Entering numbers and text with a pad: a row of characters, one of them
 * selected, rolled up and down.
 */

#include <stdio.h>
#include "libc/string.h"
#include "modules/hexedit/hexedit.h"

#define PANEL_W 200
#define PANEL_H 40
#define PANEL_X ((320 - PANEL_W) / 2)
#define PANEL_Y 90
#define CELL_W  11

static void drawPanel(TinromContext *ctx, HexEditor *ed, const char *title,
	const char *text, unsigned count, unsigned selected) {
	UIContext *ui = &ctx->ui;

	hexViewDraw(ctx, ed);

	uiFillRect(ui, PANEL_X - 1, PANEL_Y - 1, PANEL_W + 2, PANEL_H + 2,
		TINROM_COLOR_ACCENT);
	uiFillRect(ui, PANEL_X, PANEL_Y, PANEL_W, PANEL_H, TINROM_COLOR_PANEL);
	uiFillRect(ui, PANEL_X, PANEL_Y, PANEL_W, TINROM_ROW_HEIGHT,
		TINROM_COLOR_HEADER);
	uiText(ui, PANEL_X + 6, PANEL_Y + 2, title);

	int x = PANEL_X + ((PANEL_W - (int) (count * CELL_W)) / 2);
	int y = PANEL_Y + 20;

	for (unsigned int i = 0; i < count; i++) {
		char glyph[2] = { text[i], 0 };
		int  cell     = x + (int) (i * CELL_W);

		if (i == selected)
			uiFillRect(ui, cell, y - 2, CELL_W, TINROM_ROW_HEIGHT,
				TINROM_COLOR_SELECTION);

		/* A space would draw nothing; show where it is. */
		if (glyph[0] == ' ')
			glyph[0] = '_';

		uiText(ui, cell + ((CELL_W - uiTextWidth(glyph)) / 2), y, glyph);
	}
}

static uint16_t pressedAny(void) {
	return tinromPadPressed(0) | tinromPadPressed(1);
}

static uint16_t repeatedAny(void) {
	return tinromPadRepeated(0) | tinromPadRepeated(1);
}

bool hexDialHex(TinromContext *ctx, HexEditor *ed, const char *title,
	unsigned digits, uint32_t *value) {
	static const char hex[] = "0123456789ABCDEF";
	UIContext        *ui    = &ctx->ui;
	unsigned int      selected = 0;
	uint32_t          current  = *value;
	char              text[9];

	if (digits > 8)
		digits = 8;

	for (;;) {
		for (unsigned int i = 0; i < digits; i++)
			text[i] = hex[(current >> ((digits - 1 - i) * 4)) & 15];

		uiBeginFrame(ui);
		uiSetChrome(ui, ui->title, "Up/Down: digit  L/R: move  X: ok  O: back");
		drawPanel(ctx, ed, title, text, digits, selected);
		uiEndFrame(ui);

		uint16_t pressed  = pressedAny();
		uint16_t repeated = repeatedAny();
		unsigned shift    = (digits - 1 - selected) * 4;
		uint32_t digit    = (current >> shift) & 15;

		if (repeated & TINROM_PAD_UP)
			digit = (digit + 1) & 15;
		if (repeated & TINROM_PAD_DOWN)
			digit = (digit + 15) & 15;

		current = (current & ~(15u << shift)) | (digit << shift);

		if ((repeated & TINROM_PAD_LEFT) && selected)
			selected--;
		if ((repeated & TINROM_PAD_RIGHT) && (selected + 1 < digits))
			selected++;

		if (pressed & TINROM_PAD_CROSS) {
			*value = current;

			return true;
		}

		if (pressed & TINROM_PAD_CIRCLE)
			return false;
	}
}

bool hexDialText(TinromContext *ctx, HexEditor *ed, const char *title,
	char *text, unsigned max) {
	UIContext   *ui = &ctx->ui;
	char         buffer[HEX_FIND_TEXT_MAX + 1];
	unsigned int length, selected = 0;

	if (max > HEX_FIND_TEXT_MAX)
		max = HEX_FIND_TEXT_MAX;

	strncpy(buffer, text, max);
	buffer[max] = 0;
	length      = (unsigned int) strlen(buffer);

	if (!length) {
		buffer[0] = 'A';
		buffer[1] = 0;
		length    = 1;
	}

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, ui->title,
			"Up/Dn: char  L/R: move  []: del  X: ok  O: back");
		drawPanel(ctx, ed, title, buffer, length, selected);
		uiEndFrame(ui);

		uint16_t pressed  = pressedAny();
		uint16_t repeated = repeatedAny();
		char     c        = buffer[selected];

		/* Printable ASCII, wrapping. */
		if (repeated & TINROM_PAD_UP)
			c = (c >= 0x7e) ? 0x20 : (char) (c + 1);
		if (repeated & TINROM_PAD_DOWN)
			c = (c <= 0x20) ? 0x7e : (char) (c - 1);

		buffer[selected] = c;

		if ((repeated & TINROM_PAD_LEFT) && selected)
			selected--;

		/* Moving past the end grows the text by a copy of the last char. */
		if (repeated & TINROM_PAD_RIGHT) {
			if (selected + 1 < length) {
				selected++;
			} else if (length < max) {
				buffer[length] = buffer[length - 1];
				buffer[++length] = 0;
				selected++;
			}
		}

		if ((pressed & TINROM_PAD_SQUARE) && (length > 1)) {
			memmove(&buffer[selected], &buffer[selected + 1],
				length - selected);
			length--;

			if (selected >= length)
				selected = length - 1;
		}

		if (pressed & TINROM_PAD_CROSS) {
			memcpy(text, buffer, length + 1);

			return true;
		}

		if (pressed & TINROM_PAD_CIRCLE)
			return false;
	}
}
