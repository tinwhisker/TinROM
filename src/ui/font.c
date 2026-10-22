/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Laying text out in the UI font. See font.h.
 */

#include <stddef.h>
#include <stdint.h>
#include "ui/font.h"

const UIFontGlyph *uiFontGlyph(char c) {
	unsigned int index = (unsigned int) (uint8_t) c - UI_FONT_FIRST;

	if (index >= UI_FONT_GLYPHS)
		index = '?' - UI_FONT_FIRST;

	return &uiFontGlyphs[index];
}

/* Layout is done in texels and reported in units, rounded up. */
#define TAB_TEXELS (UI_FONT_TAB_WIDTH * UI_FONT_SCALE_X)

static int toUnits(int texels) {
	return (texels + UI_FONT_SCALE_X - 1) / UI_FONT_SCALE_X;
}

/*
 * Moves a pen at `pen` past `c` and returns where it ends up, in texels.
 * `*extent` is widened to cover whatever `c` draws: a glyph reaches past
 * its advance, into the outline it shares with the next one.
 */
static int step(int pen, char c, int *extent) {
	if (c == '\t')
		return ((pen / TAB_TEXELS) + 1) * TAB_TEXELS;

	const UIFontGlyph *glyph = uiFontGlyph(c);

	if (glyph->width && ((pen + glyph->width) > *extent))
		*extent = pen + glyph->width;

	return pen + glyph->advance;
}

const char *uiFontNextLine(
	const char *text, int maxWidth, size_t *length, int *width
) {
	const char *c          = text;
	const char *space      = NULL;
	int         pen        = 0;
	int         extent     = 0;
	int         spaceWidth = 0;

	maxWidth *= UI_FONT_SCALE_X;

	for (; *c && (*c != '\n'); c++) {
		if (*c == ' ') {
			space      = c;
			spaceWidth = extent;
		}

		int reach = extent;
		int next  = step(pen, *c, &reach);

		/* The first character always goes in, however wide: a line has to
		 * make progress. */
		if ((maxWidth > 0) && (reach > maxWidth) && (c != text)) {
			if (space && (space != text)) {
				*length = (size_t) (space - text);
				*width  = toUnits(spaceWidth);

				/* The space the line broke at is not carried over. */
				while (*space == ' ')
					space++;

				return *space ? space : NULL;
			}

			*length = (size_t) (c - text);
			*width  = toUnits(extent);

			return c;
		}

		pen    = next;
		extent = reach;
	}

	*length = (size_t) (c - text);
	*width  = toUnits(extent);

	return (*c == '\n') ? (c + 1) : NULL;
}

int uiFontTextWidth(const char *text) {
	int widest = 0;

	while (text) {
		size_t length;
		int    width;

		text = uiFontNextLine(text, 0, &length, &width);

		if (width > widest)
			widest = width;
	}

	return widest;
}

unsigned int uiFontLineCount(const char *text, int maxWidth) {
	unsigned int lines = 0;

	while (text) {
		size_t length;
		int    width;

		text = uiFontNextLine(text, maxWidth, &length, &width);
		lines++;
	}

	return lines;
}
