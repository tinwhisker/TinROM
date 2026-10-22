/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Drawing the hex editor: what is read each frame, and how it is laid out -
 * the data rows on the text canvas, the lines above and below as plain text.
 */

#include <stdio.h>
#include "libc/string.h"
#include "modules/hexedit/hexedit.h"

static uint8_t  canvasPixels[UI_CANVAS_BYTES(HEX_ROWS)]
	__attribute__((aligned(4)));
static UICanvas canvas;

/* Centred on a 320-pixel screen, under a line of status text. */
#define CANVAS_X ((320 - HEX_CANVAS_W) / 2)
#define STATUS_Y (TINROM_HEADER_H + 4)
#define CANVAS_Y (STATUS_Y + TINROM_ROW_HEIGHT + 2)
#define VALUE_Y  (CANVAS_Y + (HEX_ROWS * UI_CANVAS_LINE_H) + 4)

/* Status line columns, in screen pixels. */
#define STATUS_ADDRESS_X (CANVAS_X + 112)
#define STATUS_UNIT_X    (CANVAS_X + 176)
#define STATUS_EDIT_X    (CANVAS_X + 216)
#define STATUS_OWNER_X   (CANVAS_X + 254)

/* Text colours for the lines either side of the canvas. */
#define COLOR_LABEL 0x98a4b8
#define COLOR_VALUE 0xe8eef8

/* Frames a changed byte stays highlighted for. */
#define CHANGE_HOLD 30

void hexViewInit(void) {
	static const uint32_t inks[UI_CANVAS_INKS] = {
		[HEX_INK_ADDRESS] = TINROM_COLOR_ACCENT,
		[HEX_INK_DATA]    = COLOR_VALUE,
		[HEX_INK_ZERO]    = 0x687890,
		[HEX_INK_CHANGED] = TINROM_COLOR_WARNING,
		[HEX_INK_EDITED]  = TINROM_COLOR_OK,
		[HEX_INK_ASCII]   = 0xc0c8d4,
		[HEX_INK_DIM]     = 0x4c5c74,
		[HEX_INK_BAD]     = TINROM_COLOR_ERROR
	};

	uiCanvasInit(&canvas, canvasPixels, HEX_ROWS, inks);
}

/* --- reading -------------------------------------------------------------- */

void hexViewRefresh(HexEditor *ed) {
	const HexRegion *region = ed->region;

	if (region->kind == HEX_REGION_CARD) {
		ed->readable = ed->frameLoaded;

		if (ed->frameLoaded)
			memcpy(ed->bytes, ed->frame, HEX_PAGE_BYTES);

		return;
	}

	ed->readable = true;

	for (uint32_t i = 0; i < HEX_PAGE_BYTES; i += 4) {
		uint32_t word = 0;

		hexRead(region, ed->view + i, 4, &word);

		for (unsigned int b = 0; b < 4; b++)
			ed->bytes[i + b] = (uint8_t) (word >> (b * 8));
	}

	/* Something else writing to what is on screen shows up for a moment. */
	if (!ed->havePrevious || (ed->previousView != ed->view)) {
		memset(ed->age, 0, sizeof(ed->age));
	} else {
		for (unsigned int i = 0; i < HEX_PAGE_BYTES; i++) {
			if (ed->bytes[i] != ed->previous[i])
				ed->age[i] = CHANGE_HOLD;
			else if (ed->age[i])
				ed->age[i]--;
		}
	}

	memcpy(ed->previous, ed->bytes, HEX_PAGE_BYTES);
	ed->previousView = ed->view;
	ed->havePrevious = true;
}

/* --- layout ----------------------------------------------------------------- */

static int unitX(unsigned int unit, unsigned int index) {
	return HEX_X_DATA + (int) (index * unit * HEX_BYTE_PITCH);
}

static uint32_t unitValue(const HexEditor *ed, uint32_t start, unsigned unit) {
	uint32_t value = 0;

	for (unsigned int b = 0; b < unit; b++)
		value |= (uint32_t) ed->bytes[start + b] << (b * 8);

	return value;
}

static uint8_t unitInk(const HexEditor *ed, uint32_t start, unsigned unit,
	uint32_t value) {
	bool changed = false, edited = false;

	for (unsigned int b = 0; b < unit; b++) {
		if (ed->age[start + b])
			changed = true;
		if ((ed->region->kind == HEX_REGION_CARD)
			&& hexCardByteDirty(ed, start + b))
			edited = true;
	}

	if (edited)
		return HEX_INK_EDITED;
	if (changed)
		return HEX_INK_CHANGED;

	return value ? HEX_INK_DATA : HEX_INK_ZERO;
}

static unsigned int addressDigits(const HexEditor *ed) {
	return (ed->region->kind == HEX_REGION_CARD) ? 5 : 8;
}

static void composeRow(const HexEditor *ed, unsigned int row) {
	const HexRegion *region = ed->region;
	uint32_t         first  = row * HEX_ROW_BYTES;
	uint32_t         offset = ed->view + first;

	uiCanvasHex(&canvas, HEX_X_ADDRESS, row, region->base + offset,
		addressDigits(ed), HEX_INK_ADDRESS);

	if (!ed->readable) {
		for (unsigned int i = 0; i < HEX_ROW_BYTES; i++)
			uiCanvasPrint(&canvas, unitX(1, i), row, "??", HEX_INK_BAD);

		return;
	}

	unsigned int unit  = ed->unit;
	unsigned int units = HEX_ROW_BYTES / unit;

	for (unsigned int i = 0; i < units; i++) {
		uint32_t start = first + i * unit;
		uint32_t value = unitValue(ed, start, unit);
		uint8_t  ink   = unitInk(ed, start, unit, value);

		/* The unit being edited shows what it will become. */
		if (ed->editing && ((ed->view + start) == ed->cursor)) {
			if (ed->pending != value)
				ink = HEX_INK_EDITED;

			value = ed->pending;
		}

		uiCanvasHex(&canvas, unitX(unit, i), row, value, unit * 2, ink);
	}

	for (unsigned int i = 0; i < HEX_ROW_BYTES; i++) {
		uint8_t byte      = ed->bytes[first + i];
		bool    printable = (byte >= 0x20) && (byte <= 0x7e);

		uiCanvasPut(&canvas, HEX_X_ASCII + (int) (i * UI_CANVAS_GLYPH_W),
			row, printable ? (char) byte : '.',
			printable ? HEX_INK_ASCII : HEX_INK_DIM);
	}
}

/* The region, the cursor's address, the unit, and whose RAM it is. */
static void drawStatus(UIContext *ui, const HexEditor *ed) {
	const HexRegion *region = ed->region;
	char             text[12];

	uiTextColored(ui, CANVAS_X, STATUS_Y, COLOR_LABEL, region->name);

	snprintf(text, sizeof(text), "%0*X", (int) addressDigits(ed),
		(unsigned int) (region->base + ed->cursor));
	uiTextColored(ui, STATUS_ADDRESS_X, STATUS_Y, TINROM_COLOR_ACCENT, text);

	static const char *const units[] = { "", "byte", "half", "", "word" };

	uiTextColored(ui, STATUS_UNIT_X, STATUS_Y, COLOR_LABEL, units[ed->unit]);

	if (ed->editing)
		uiTextColored(ui, STATUS_EDIT_X, STATUS_Y, TINROM_COLOR_OK, "EDIT");

	const char *owner = hexRamOwner(region, ed->cursor);

	if (owner)
		uiTextColored(ui, STATUS_OWNER_X, STATUS_Y, TINROM_COLOR_WARNING,
			owner);
}

/* Draws `label` then `value`, and returns the x after them both. */
static int labelled(UIContext *ui, int x, const char *label,
	const char *value) {
	uiTextColored(ui, x, VALUE_Y, COLOR_LABEL, label);
	x += uiTextWidth(label) + 4;
	uiTextColored(ui, x, VALUE_Y, COLOR_VALUE, value);

	return x + uiTextWidth(value) + 14;
}

/* The value under the cursor at each size, or a message, or card state. */
static void drawValue(UIContext *ui, const HexEditor *ed) {
	if (ed->message[0]) {
		uiTextColored(ui, CANVAS_X, VALUE_Y, TINROM_COLOR_WARNING,
			ed->message);

		return;
	}

	char text[40];

	if (ed->region->kind == HEX_REGION_CARD) {
		bool dirty = hexCardDirty(ed);

		snprintf(text, sizeof(text), "frame %03X  block %u  %s",
			(unsigned int) ed->frameNumber,
			(unsigned int) (ed->frameNumber / 64),
			dirty ? "edited, not written" : "as on the card");
		uiTextColored(ui, CANVAS_X, VALUE_Y,
			dirty ? TINROM_COLOR_OK : COLOR_LABEL, text);

		return;
	}

	uint32_t index = ed->cursor - ed->view;
	int      x     = CANVAS_X;

	snprintf(text, sizeof(text), "%02X", (unsigned int) ed->bytes[index]);
	x = labelled(ui, x, "u8", text);
	snprintf(text, sizeof(text), "%04X",
		(unsigned int) unitValue(ed, index & ~1u, 2));
	x = labelled(ui, x, "u16", text);
	snprintf(text, sizeof(text), "%08X",
		(unsigned int) unitValue(ed, index & ~3u, 4));
	labelled(ui, x, "u32", text);
}

/* --- drawing -------------------------------------------------------------- */

void hexViewDraw(TinromContext *ctx, HexEditor *ed) {
	UIContext *ui = &ctx->ui;

	uiCanvasClear(&canvas);

	for (unsigned int row = 0; row < HEX_ROWS; row++)
		composeRow(ed, row);

	uiCanvasUpload(&canvas);

	drawStatus(ui, ed);
	drawValue(ui, ed);

	/* The cursor goes behind the text: the canvas is transparent there. */
	uint32_t     index = ed->cursor - ed->view;
	unsigned int row   = index / HEX_ROW_BYTES;
	unsigned int slot  = (index % HEX_ROW_BYTES) / ed->unit;
	int          x     = CANVAS_X + unitX(ed->unit, slot);
	int          y     = CANVAS_Y + (int) (row * UI_CANVAS_LINE_H);

	uiFillRect(ui, x - 2, y - 1, (int) (ed->unit * 2 * UI_CANVAS_GLYPH_W) + 3,
		UI_CANVAS_LINE_H, TINROM_COLOR_SELECTION);

	/* The digit being rolled gets a bar under it. */
	if (ed->editing)
		uiFillRect(ui, x + (int) (ed->nibble * UI_CANVAS_GLYPH_W) + 1,
			y + UI_FONT_HEIGHT - 1, UI_CANVAS_GLYPH_W - 2, 2,
			TINROM_COLOR_ACCENT);

	/* And the byte's ASCII cell, so the two columns can be matched up. */
	uiFillRect(ui,
		CANVAS_X + HEX_X_ASCII
			+ (int) ((index % HEX_ROW_BYTES) * UI_CANVAS_GLYPH_W),
		y - 1, (int) (ed->unit * UI_CANVAS_GLYPH_W), UI_CANVAS_LINE_H,
		TINROM_COLOR_HEADER);

	uiCanvasDraw(ui, &canvas, CANVAS_X, CANVAS_Y, HEX_CANVAS_W, HEX_ROWS);
}

void hexViewBackground(void *editor) {
	hexViewDraw(tinromContext(), (HexEditor *) editor);
}
