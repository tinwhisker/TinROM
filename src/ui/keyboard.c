/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The on-screen keyboard. See keyboard.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/app.h"
#include "hal/pad.h"
#include "hal/timer.h"
#include "libc/string.h"
#include "ui/keyboard.h"
#include "ui/theme.h"
#include "ui/ui.h"

/* Keys that are not characters. */
enum {
	K_SHIFT = 0x200,
	K_CTRL
};

typedef struct {
	const char *label;   /* NULL: show the character itself */
	int         normal;  /* key code */
	int         shifted; /* key code with Shift; 0 for the same */
	uint8_t     units;   /* width, in key widths */
} Key;

#define ROWS     5
#define KEY_W    22
#define KEY_H    12
#define ROW_KEYS 13

static const Key rows[ROWS][ROW_KEYS] = {
	{
		{ 0, '`', '~', 1 }, { 0, '1', '!', 1 }, { 0, '2', '@', 1 },
		{ 0, '3', '#', 1 }, { 0, '4', '$', 1 }, { 0, '5', '%', 1 },
		{ 0, '6', '^', 1 }, { 0, '7', '&', 1 }, { 0, '8', '*', 1 },
		{ 0, '9', '(', 1 }, { 0, '0', ')', 1 }, { 0, '-', '_', 1 },
		{ 0, '=', '+', 1 }
	}, {
		{ 0, 'q', 'Q', 1 }, { 0, 'w', 'W', 1 }, { 0, 'e', 'E', 1 },
		{ 0, 'r', 'R', 1 }, { 0, 't', 'T', 1 }, { 0, 'y', 'Y', 1 },
		{ 0, 'u', 'U', 1 }, { 0, 'i', 'I', 1 }, { 0, 'o', 'O', 1 },
		{ 0, 'p', 'P', 1 }, { 0, '[', '{', 1 }, { 0, ']', '}', 1 },
		{ 0, '\\', '|', 1 }
	}, {
		{ 0, 'a', 'A', 1 }, { 0, 's', 'S', 1 }, { 0, 'd', 'D', 1 },
		{ 0, 'f', 'F', 1 }, { 0, 'g', 'G', 1 }, { 0, 'h', 'H', 1 },
		{ 0, 'j', 'J', 1 }, { 0, 'k', 'K', 1 }, { 0, 'l', 'L', 1 },
		{ 0, ';', ':', 1 }, { 0, '\'', '"', 1 },
		{ "Enter", '\r', 0, 2 }
	}, {
		{ "Shift", K_SHIFT, 0, 2 },
		{ 0, 'z', 'Z', 1 }, { 0, 'x', 'X', 1 }, { 0, 'c', 'C', 1 },
		{ 0, 'v', 'V', 1 }, { 0, 'b', 'B', 1 }, { 0, 'n', 'N', 1 },
		{ 0, 'm', 'M', 1 }, { 0, ',', '<', 1 }, { 0, '.', '>', 1 },
		{ 0, '/', '?', 1 },
		{ "BS", '\b', 0, 1 }
	}, {
		{ "Ctrl", K_CTRL, 0, 2 }, { "Esc", 0x1b, 0, 2 },
		{ "Tab", '\t', 0, 2 }, { "Space", ' ', 0, 3 },
		{ "<", UI_KEY_LEFT, 0, 1 }, { ">", UI_KEY_RIGHT, 0, 1 },
		{ "^", UI_KEY_UP, 0, 1 }, { "v", UI_KEY_DOWN, 0, 1 }
	}
};

static unsigned rowLength(unsigned row) {
	unsigned count = 0;

	while ((count < ROW_KEYS) && rows[row][count].units)
		count++;

	return count;
}

/* The left edge of a key, in key widths from the start of its row. */
static unsigned keyStart(unsigned row, unsigned col) {
	unsigned units = 0;

	for (unsigned i = 0; i < col; i++)
		units += rows[row][i].units;

	return units;
}

void uiKeyboardInit(UIKeyboard *kb) {
	*kb = (UIKeyboard) { 1, 0, false, false };
}

/* Down or up a row: the key whose middle is nearest the current one's. */
static void moveRow(UIKeyboard *kb, int delta) {
	unsigned from   = kb->row;
	unsigned middle = keyStart(from, kb->col) * 2 + rows[from][kb->col].units;
	unsigned to     = (unsigned) ((int) from + delta + ROWS) % ROWS;
	unsigned best   = 0, bestDistance = ~0u;

	for (unsigned col = 0; col < rowLength(to); col++) {
		unsigned centre   = keyStart(to, col) * 2 + rows[to][col].units;
		unsigned distance = (centre > middle) ? centre - middle
			: middle - centre;

		if (distance < bestDistance) {
			best         = col;
			bestDistance = distance;
		}
	}

	kb->row = (uint8_t) to;
	kb->col = (uint8_t) best;
}

static int applyModifiers(UIKeyboard *kb, const Key *key) {
	int code = (kb->shift && key->shifted) ? key->shifted : key->normal;

	if (kb->ctrl && (code >= 0x40) && (code < 0x80)) {
		code &= 0x1f;

		if (!code)
			code = UI_KEY_NUL;
	}

	kb->shift = false;
	kb->ctrl  = false;

	return code;
}

static int keyboardUpdate(UIKeyboard *kb, uint16_t pressed, uint16_t repeated) {
	unsigned length = rowLength(kb->row);

	if (repeated & TINROM_PAD_LEFT)
		kb->col = (uint8_t) ((kb->col + length - 1) % length);
	if (repeated & TINROM_PAD_RIGHT)
		kb->col = (uint8_t) ((kb->col + 1) % length);
	if (repeated & TINROM_PAD_UP)
		moveRow(kb, -1);
	if (repeated & TINROM_PAD_DOWN)
		moveRow(kb, 1);

	if (pressed & TINROM_PAD_TRIANGLE)
		kb->shift = !kb->shift;
	if (repeated & TINROM_PAD_SQUARE)
		return '\b';
	if (repeated & TINROM_PAD_L1) {
		kb->shift = false;

		return ' ';
	}
	if (pressed & TINROM_PAD_START)
		return '\r';

	if (repeated & TINROM_PAD_CROSS) {
		const Key *key = &rows[kb->row][kb->col];

		switch (key->normal) {
			case K_SHIFT:
				kb->shift = !kb->shift;
				return UI_KEY_NONE;

			case K_CTRL:
				kb->ctrl = !kb->ctrl;
				return UI_KEY_NONE;

			default:
				return applyModifiers(kb, key);
		}
	}

	return UI_KEY_NONE;
}

void uiKeyboardDraw(const UIKeyboard *kb, UIContext *ui, int y) {
	int left = ((int) ui->width - (ROW_KEYS * KEY_W)) / 2;

	uiFillRect(ui, 0, y, (int) ui->width, UI_KEYBOARD_HEIGHT,
		TINROM_COLOR_PANEL);

	for (unsigned row = 0; row < ROWS; row++) {
		int keyY = y + 3 + (int) (row * (KEY_H + 1));

		for (unsigned col = 0; col < rowLength(row); col++) {
			const Key *key     = &rows[row][col];
			int        keyX    = left + (int) (keyStart(row, col) * KEY_W);
			int        width   = (int) (key->units * KEY_W) - 1;
			bool       current = (row == kb->row) && (col == kb->col);
			bool       latched = ((key->normal == K_SHIFT) && kb->shift)
				|| ((key->normal == K_CTRL) && kb->ctrl);
			char       text[2] = { 0, 0 };
			const char *label  = key->label;

			uiFillRect(ui, keyX, keyY, width, KEY_H,
				current ? TINROM_COLOR_SELECTION
					: (latched ? TINROM_COLOR_ACCENT : TINROM_COLOR_HEADER));

			if (!label) {
				text[0] = (char) ((kb->shift && key->shifted)
					? key->shifted : key->normal);
				label   = text;
			}

			uiTextColored(ui, keyX + ((width - uiTextWidth(label)) / 2),
				keyY + 2, current ? TINROM_COLOR_TEXT : 0xc8d0dc, label);
		}
	}
}

static void (*keyHook)(uint32_t value);

void uiKeyboardSetKeyHook(void (*hook)(uint32_t value)) {
	keyHook = hook;
}

int uiKeyboardUpdate(UIKeyboard *kb, uint16_t pressed, uint16_t repeated) {
	int key = keyboardUpdate(kb, pressed, repeated);

	/* When each key lands is the one unpredictable thing a console has. */
	if ((key != UI_KEY_NONE) && keyHook)
		keyHook(tinromTimerNoise() ^ ((uint32_t) key << 16));

	return key;
}

/* --- the line editor ----------------------------------------------------- */

/* A password should not outlive the editor in its stack frame. */
static void wipe(char *data, size_t length) {
	volatile char *p = data;

	while (length--)
		*p++ = 0;
}

static bool editText(
	TinromContext *ctx, const char *title, const char *prompt,
	char *buffer, size_t size, bool masked
) {
	UIContext *ui = &ctx->ui;
	char       text[128];
	size_t     length, cursor;
	UIKeyboard kb;

	if (!size)
		return false;
	if (size > sizeof(text))
		size = sizeof(text);

	memcpy(text, buffer, size - 1);
	text[size - 1] = 0;
	length = cursor = strlen(text);
	uiKeyboardInit(&kb);

	bool hadModel = uiModelVisible(ui);

	uiSetModelVisible(ui, false);

	for (;;) {
		uiBeginFrame(ui);
		uiSetChrome(ui, title, "X: key  []: del  /\\: shift  Start: done");

		int y = TINROM_HEADER_H + 10;

		if (prompt)
			uiTextWrapped(ui, TINROM_MARGIN_X + 4, y,
				(int) ui->width - 2 * (TINROM_MARGIN_X + 4),
				TINROM_COLOR_DIM, prompt);

		int boxY = y + 34;

		uiFillRect(ui, TINROM_MARGIN_X, boxY, (int) ui->width
			- 2 * TINROM_MARGIN_X, 16, TINROM_COLOR_PANEL);

		/* What is drawn: the text, or a star for each of its characters. */
		char drawn[128];

		memcpy(drawn, text, length + 1);

		if (masked)
			memset(drawn, '*', length);

		/* Show the end of a long line rather than its start. */
		char        before[128];
		const char *shown = drawn;
		int         room  = (int) ui->width - 2 * TINROM_MARGIN_X - 12;

		memcpy(before, drawn, cursor);
		before[cursor] = 0;

		while ((uiTextWidth(before + (shown - drawn)) > room)
			&& (shown < drawn + cursor))
			shown++;

		int textX = TINROM_MARGIN_X + 6;

		uiTextClipped(ui, textX, boxY + 4, room, TINROM_COLOR_TEXT, shown);
		uiFillRect(ui, textX + uiTextWidth(before + (shown - drawn)), boxY + 3,
			1, 10, TINROM_COLOR_ACCENT);

		uiKeyboardDraw(&kb, ui,
			(int) ui->height - TINROM_FOOTER_H - UI_KEYBOARD_HEIGHT - 4);
		uiEndFrame(ui);

		uint16_t pressed  = tinromPadPressed(0) | tinromPadPressed(1);
		uint16_t repeated = tinromPadRepeated(0) | tinromPadRepeated(1);

		if (pressed & TINROM_PAD_CIRCLE) {
			uiSetModelVisible(ui, hadModel);
			wipe(text, sizeof(text));

			return false;
		}

		int key = uiKeyboardUpdate(&kb, pressed, repeated);

		switch (key) {
			case UI_KEY_NONE:
				break;

			case '\r':
				memcpy(buffer, text, length + 1);
				uiSetModelVisible(ui, hadModel);
				wipe(text, sizeof(text));

				return true;

			case '\b':
				if (cursor) {
					memmove(&text[cursor - 1], &text[cursor],
						length - cursor + 1);
					cursor--;
					length--;
				}
				break;

			case UI_KEY_LEFT:
				if (cursor)
					cursor--;
				break;

			case UI_KEY_RIGHT:
				if (cursor < length)
					cursor++;
				break;

			default:
				if ((key >= 0x20) && (key < 0x7f) && (length + 1 < size)) {
					memmove(&text[cursor + 1], &text[cursor],
						length - cursor + 1);
					text[cursor++] = (char) key;
					length++;
				}
				break;
		}
	}
}

bool uiEditText(
	TinromContext *ctx, const char *title, const char *prompt,
	char *buffer, size_t size
) {
	return editText(ctx, title, prompt, buffer, size, false);
}

bool uiEditSecret(
	TinromContext *ctx, const char *title, const char *prompt,
	char *buffer, size_t size
) {
	return editText(ctx, title, prompt, buffer, size, true);
}
