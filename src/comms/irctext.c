/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * IRC lines onto the terminal's screen. See irctext.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "comms/irctext.h"
#include "comms/term.h"
#include "libc/string.h"

/* ANSI colour indices, as the screen numbers them. */
enum {
	BLACK = 0, RED, GREEN, BROWN, BLUE, MAGENTA, CYAN, GREY,
	DARK_GREY, LIGHT_RED, LIGHT_GREEN, YELLOW, LIGHT_BLUE, PINK, LIGHT_CYAN,
	WHITE
};

#define DEFAULT_FG GREY
#define INDENT_MAX 24

/* --- characters ------------------------------------------------------------ */

/*
 * Every CP437 byte from 80h up and the code point it shows, sorted by code
 * point for a binary search. Generated from Python's cp437 codec, which
 * follows the Unicode Consortium's published mapping (see NOTICE.md).
 */
static const struct {
	uint16_t codepoint;
	uint8_t  cp437;
} cp437Table[] = {
	{ 0x00a0, 0xff }, { 0x00a1, 0xad }, { 0x00a2, 0x9b }, { 0x00a3, 0x9c }, { 0x00a5, 0x9d },
	{ 0x00aa, 0xa6 }, { 0x00ab, 0xae }, { 0x00ac, 0xaa }, { 0x00b0, 0xf8 }, { 0x00b1, 0xf1 },
	{ 0x00b2, 0xfd }, { 0x00b5, 0xe6 }, { 0x00b7, 0xfa }, { 0x00ba, 0xa7 }, { 0x00bb, 0xaf },
	{ 0x00bc, 0xac }, { 0x00bd, 0xab }, { 0x00bf, 0xa8 }, { 0x00c4, 0x8e }, { 0x00c5, 0x8f },
	{ 0x00c6, 0x92 }, { 0x00c7, 0x80 }, { 0x00c9, 0x90 }, { 0x00d1, 0xa5 }, { 0x00d6, 0x99 },
	{ 0x00dc, 0x9a }, { 0x00df, 0xe1 }, { 0x00e0, 0x85 }, { 0x00e1, 0xa0 }, { 0x00e2, 0x83 },
	{ 0x00e4, 0x84 }, { 0x00e5, 0x86 }, { 0x00e6, 0x91 }, { 0x00e7, 0x87 }, { 0x00e8, 0x8a },
	{ 0x00e9, 0x82 }, { 0x00ea, 0x88 }, { 0x00eb, 0x89 }, { 0x00ec, 0x8d }, { 0x00ed, 0xa1 },
	{ 0x00ee, 0x8c }, { 0x00ef, 0x8b }, { 0x00f1, 0xa4 }, { 0x00f2, 0x95 }, { 0x00f3, 0xa2 },
	{ 0x00f4, 0x93 }, { 0x00f6, 0x94 }, { 0x00f7, 0xf6 }, { 0x00f9, 0x97 }, { 0x00fa, 0xa3 },
	{ 0x00fb, 0x96 }, { 0x00fc, 0x81 }, { 0x00ff, 0x98 }, { 0x0192, 0x9f }, { 0x0393, 0xe2 },
	{ 0x0398, 0xe9 }, { 0x03a3, 0xe4 }, { 0x03a6, 0xe8 }, { 0x03a9, 0xea }, { 0x03b1, 0xe0 },
	{ 0x03b4, 0xeb }, { 0x03b5, 0xee }, { 0x03c0, 0xe3 }, { 0x03c3, 0xe5 }, { 0x03c4, 0xe7 },
	{ 0x03c6, 0xed }, { 0x207f, 0xfc }, { 0x20a7, 0x9e }, { 0x2219, 0xf9 }, { 0x221a, 0xfb },
	{ 0x221e, 0xec }, { 0x2229, 0xef }, { 0x2248, 0xf7 }, { 0x2261, 0xf0 }, { 0x2264, 0xf3 },
	{ 0x2265, 0xf2 }, { 0x2310, 0xa9 }, { 0x2320, 0xf4 }, { 0x2321, 0xf5 }, { 0x2500, 0xc4 },
	{ 0x2502, 0xb3 }, { 0x250c, 0xda }, { 0x2510, 0xbf }, { 0x2514, 0xc0 }, { 0x2518, 0xd9 },
	{ 0x251c, 0xc3 }, { 0x2524, 0xb4 }, { 0x252c, 0xc2 }, { 0x2534, 0xc1 }, { 0x253c, 0xc5 },
	{ 0x2550, 0xcd }, { 0x2551, 0xba }, { 0x2552, 0xd5 }, { 0x2553, 0xd6 }, { 0x2554, 0xc9 },
	{ 0x2555, 0xb8 }, { 0x2556, 0xb7 }, { 0x2557, 0xbb }, { 0x2558, 0xd4 }, { 0x2559, 0xd3 },
	{ 0x255a, 0xc8 }, { 0x255b, 0xbe }, { 0x255c, 0xbd }, { 0x255d, 0xbc }, { 0x255e, 0xc6 },
	{ 0x255f, 0xc7 }, { 0x2560, 0xcc }, { 0x2561, 0xb5 }, { 0x2562, 0xb6 }, { 0x2563, 0xb9 },
	{ 0x2564, 0xd1 }, { 0x2565, 0xd2 }, { 0x2566, 0xcb }, { 0x2567, 0xcf }, { 0x2568, 0xd0 },
	{ 0x2569, 0xca }, { 0x256a, 0xd8 }, { 0x256b, 0xd7 }, { 0x256c, 0xce }, { 0x2580, 0xdf },
	{ 0x2584, 0xdc }, { 0x2588, 0xdb }, { 0x258c, 0xdd }, { 0x2590, 0xde }, { 0x2591, 0xb0 },
	{ 0x2592, 0xb1 }, { 0x2593, 0xb2 }, { 0x25a0, 0xfe },
};

/* A few that CP437 lacks but has a fair stand-in for. */
static const struct {
	uint16_t codepoint;
	uint8_t  cp437;
} lookalikes[] = {
	{ 0x2018, '\'' }, { 0x2019, '\'' }, { 0x201a, ',' }, { 0x201c, '"' },
	{ 0x201d, '"' }, { 0x201e, '"' }, { 0x2013, '-' }, { 0x2014, '-' },
	{ 0x2022, 0xf9 }, { 0x2026, 0xfa }, { 0x20ac, 'E' }, { 0x00a9, 'c' },
	{ 0x00ae, 'R' }, { 0x2122, 'T' }, { 0x00d7, 'x' }, { 0x2192, 0x1a },
	{ 0x2190, 0x1b }
};

uint8_t ircTextCp437(uint32_t codepoint) {
	if ((codepoint >= 0x20) && (codepoint < 0x7f))
		return (uint8_t) codepoint;

	size_t low = 0, high = sizeof(cp437Table) / sizeof(cp437Table[0]);

	while (low < high) {
		size_t mid = (low + high) / 2;

		if (cp437Table[mid].codepoint == codepoint)
			return cp437Table[mid].cp437;
		if (cp437Table[mid].codepoint < codepoint)
			low = mid + 1;
		else
			high = mid;
	}

	for (size_t i = 0; i < sizeof(lookalikes) / sizeof(lookalikes[0]); i++) {
		/* The arrows' glyphs live below 20h, which the screen would take
		 * for control codes; they stay as plain substitutes. */
		if ((lookalikes[i].codepoint == codepoint)
			&& (lookalikes[i].cp437 >= 0x20))
			return lookalikes[i].cp437;
	}

	return '?';
}

uint32_t ircTextDecode(const char **text) {
	const uint8_t *p     = (const uint8_t *) *text;
	uint32_t       first = p[0];

	if (first < 0x80) {
		*text += 1;
		return first;
	}

	unsigned more;
	uint32_t value, least;

	if ((first & 0xe0) == 0xc0) {
		more = 1; value = first & 0x1f; least = 0x80;
	} else if ((first & 0xf0) == 0xe0) {
		more = 2; value = first & 0x0f; least = 0x800;
	} else if ((first & 0xf8) == 0xf0) {
		more = 3; value = first & 0x07; least = 0x10000;
	} else {
		more = 0; value = 0; least = 0;
	}

	bool valid = more != 0;

	for (unsigned i = 1; valid && (i <= more); i++) {
		if ((p[i] & 0xc0) != 0x80)
			valid = false;
		else
			value = (value << 6) | (p[i] & 0x3f);
	}

	/* Overlong forms and surrogates are as invalid as a stray byte. */
	if (valid && ((value < least) || (value > 0x10ffff)
		|| ((value >= 0xd800) && (value < 0xe000))))
		valid = false;

	if (!valid) {
		/* Not UTF-8: the old clients' Latin-1, one byte each. */
		*text += 1;
		return first;
	}

	*text += 1 + more;

	return value;
}

/* --- mIRC formatting ---------------------------------------------------------- */

/* mIRC's sixteen in the screen's order. */
static const uint8_t mircColors[16] = {
	WHITE, BLACK, BLUE, GREEN, LIGHT_RED, RED, MAGENTA, BROWN,
	YELLOW, LIGHT_GREEN, CYAN, LIGHT_CYAN, LIGHT_BLUE, PINK, DARK_GREY, GREY
};

/* Up to two digits; -1 if there are none. */
static int readNumber(const char **text) {
	const char *p = *text;
	int         value = -1;

	for (unsigned i = 0; (i < 2) && (*p >= '0') && (*p <= '9'); i++, p++)
		value = ((value < 0) ? 0 : value * 10) + (*p - '0');

	*text = p;

	return value;
}

static bool isHex(char c) {
	return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f'))
		|| ((c >= 'A') && (c <= 'F'));
}

size_t ircTextCells(const char *text, uint8_t baseFg, bool highlight,
	IrcCell *out, size_t max) {
	int    fg = -1, bg = -1;
	bool   bold = false, reverse = false;
	size_t count = 0;

	(void) highlight;

	while (*text && (count < max)) {
		uint8_t c = (uint8_t) *text;

		switch (c) {
			case 0x02: bold = !bold; text++; continue;
			case 0x16: reverse = !reverse; text++; continue;
			case 0x0f: fg = bg = -1; bold = reverse = false; text++; continue;
			case 0x1d: case 0x1f: case 0x1e: case 0x11: text++; continue;

			case 0x03: {
				text++;

				int f = readNumber(&text), b = -1;

				if ((f >= 0) && (text[0] == ',') && (text[1] >= '0')
					&& (text[1] <= '9')) {
					text++;
					b = readNumber(&text);
				}

				if (f < 0) {
					fg = bg = -1;
				} else {
					fg = (f < 16) ? mircColors[f] : -1;

					if (b >= 0)
						bg = (b < 16) ? mircColors[b] : -1;
				}
				continue;
			}

			case 0x04: {
				/* RRGGBB[,RRGGBB]: read past, shown as the default. */
				text++;

				for (unsigned i = 0; (i < 6) && isHex(*text); i++)
					text++;

				if ((text[0] == ',') && isHex(text[1])) {
					text++;

					for (unsigned i = 0; (i < 6) && isHex(*text); i++)
						text++;
				}

				fg = bg = -1;
				continue;
			}

			case '\t':
				c = ' ';
				break;
		}

		uint32_t codepoint = ircTextDecode(&text);

		/* Combining marks have no cell of their own; other controls,
		 * and DEL, would be taken for the screen's own. */
		if ((codepoint >= 0x300) && (codepoint < 0x370))
			continue;

		uint8_t ch = (c == ' ') ? ' ' : ircTextCp437(codepoint);

		if ((codepoint < 0x20) && (c != ' '))
			ch = '?';
		if (codepoint == 0x7f)
			ch = '?';

		uint8_t cellFg = (fg >= 0) ? (uint8_t) fg : baseFg;
		uint8_t cellBg = (bg >= 0) ? (uint8_t) bg : BLACK;

		if (bold && (cellFg < 8))
			cellFg += 8;

		if (reverse) {
			uint8_t swap = cellFg;

			cellFg = cellBg;
			cellBg = swap;
		}

		out[count++] = (IrcCell) { ch, cellFg, cellBg };
	}

	return count;
}

void ircTextPlain(const char *text, char *out, size_t size) {
	static IrcCell cells[IRC_LINE_MAX];
	size_t         count = ircTextCells(text, DEFAULT_FG, false, cells,
		sizeof(cells) / sizeof(cells[0]));
	size_t         used  = 0;

	if (!size)
		return;

	for (size_t i = 0; (i < count) && (used + 1 < size); i++) {
		uint8_t ch = cells[i].ch;

		out[used++] = (char) (((ch >= 0x20) && (ch < 0x7f)) ? ch : '?');
	}

	out[used] = 0;
}

/* --- lines ------------------------------------------------------------------- */

static bool fresh = true;

void ircTextReset(TermScreen *term) {
	termReset(term);
	/* No cursor: nothing is typed into this screen. */
	termFeed(term, (const uint8_t *) "\033[?25l", 6);
	fresh = true;
}

/* The colour a nick is always shown in: one of ten, by a hash of it. */
static uint8_t nickColor(const char *nick) {
	static const uint8_t colors[] = {
		GREEN, BROWN, MAGENTA, CYAN, LIGHT_RED, LIGHT_GREEN, YELLOW,
		LIGHT_BLUE, PINK, LIGHT_CYAN
	};
	uint32_t hash = 5381;

	for (; nick && *nick; nick++) {
		int c = (unsigned char) *nick;

		/* Case-blind, so a nick keeps its colour when it changes case. */
		if ((c >= 'A') && (c <= 'Z'))
			c += 'a' - 'A';

		hash = hash * 33 + (uint32_t) c;
	}

	return colors[hash % sizeof(colors)];
}

/*
 * Decoration, nicks and channel names: decoded like any text, with nothing
 * below 20h let through. A nick is the server's to send, and an ESC in one
 * would otherwise be the screen's to obey.
 */
static size_t put(IrcCell *cells, size_t count, size_t max, const char *text,
	uint8_t fg, uint8_t bg) {
	while (text && *text && (count < max)) {
		uint32_t codepoint = ircTextDecode(&text);
		uint8_t  ch        = ((codepoint < 0x20) || (codepoint == 0x7f)) ? '?'
			: ircTextCp437(codepoint);

		cells[count++] = (IrcCell) { ch, fg, bg };
	}

	return count;
}

/* The decoration in front of the text, and the text's own colour. */
static size_t prefixCells(const IrcLine *line, IrcCell *cells, size_t max,
	uint8_t *textFg) {
	size_t      n   = 0;
	const char *who = line->who ? line->who : "";
	uint8_t     whoFg = nickColor(who), whoBg = BLACK;

	if (line->highlight) {
		whoFg = BLACK;
		whoBg = YELLOW;
	}

	if (line->where) {
		n = put(cells, n, max, "[", DARK_GREY, BLACK);
		n = put(cells, n, max, line->where, CYAN, BLACK);
		n = put(cells, n, max, "] ", DARK_GREY, BLACK);
	}

	*textFg = DEFAULT_FG;

	switch (line->style) {
		case IRC_STYLE_MESSAGE:
			n = put(cells, n, max, "<", DARK_GREY, BLACK);
			n = put(cells, n, max, who, whoFg, whoBg);
			n = put(cells, n, max, "> ", DARK_GREY, BLACK);
			if (line->highlight)
				*textFg = YELLOW;
			break;

		case IRC_STYLE_OWN:
			n = put(cells, n, max, "<", DARK_GREY, BLACK);
			n = put(cells, n, max, who, WHITE, BLACK);
			n = put(cells, n, max, "> ", DARK_GREY, BLACK);
			*textFg = WHITE;
			break;

		case IRC_STYLE_ACTION:
		case IRC_STYLE_OWN_ACTION:
			n = put(cells, n, max, "* ", PINK, BLACK);
			n = put(cells, n, max, who, (line->style == IRC_STYLE_OWN_ACTION)
				? WHITE : whoFg, (line->style == IRC_STYLE_OWN_ACTION)
				? BLACK : whoBg);
			n = put(cells, n, max, " ", PINK, BLACK);
			*textFg = line->highlight ? YELLOW : PINK;
			break;

		case IRC_STYLE_NOTICE:
			n = put(cells, n, max, "-", BROWN, BLACK);
			n = put(cells, n, max, who, YELLOW, BLACK);
			n = put(cells, n, max, "- ", BROWN, BLACK);
			*textFg = BROWN;
			break;

		case IRC_STYLE_PRIVATE:
			n = put(cells, n, max, "*", PINK, BLACK);
			n = put(cells, n, max, who, BLACK, PINK);
			n = put(cells, n, max, "* ", PINK, BLACK);
			*textFg = WHITE;
			break;

		case IRC_STYLE_PRIVATE_OUT:
			n = put(cells, n, max, "-> *", MAGENTA, BLACK);
			n = put(cells, n, max, who, PINK, BLACK);
			n = put(cells, n, max, "* ", MAGENTA, BLACK);
			break;

		case IRC_STYLE_MOTD:
			break;

		case IRC_STYLE_ERROR:
			n = put(cells, n, max, "-!- ", LIGHT_RED, BLACK);
			*textFg = LIGHT_RED;
			break;

		case IRC_STYLE_JOIN:
			n = put(cells, n, max, "--> ", LIGHT_GREEN, BLACK);
			*textFg = GREEN;
			break;

		case IRC_STYLE_PART:
			n = put(cells, n, max, "<-- ", BROWN, BLACK);
			*textFg = BROWN;
			break;

		case IRC_STYLE_CHANGE:
			n = put(cells, n, max, "-!- ", CYAN, BLACK);
			*textFg = LIGHT_CYAN;
			break;

		default:
			n = put(cells, n, max, "-!- ", DARK_GREY, BLACK);
			break;
	}

	return n;
}

static void emit(TermScreen *term, const IrcCell *cells, size_t count,
	int *fg, int *bg) {
	uint8_t buffer[96];
	size_t  used = 0;

	for (size_t i = 0; i < count; i++) {
		if ((cells[i].fg != *fg) || (cells[i].bg != *bg)) {
			*fg = cells[i].fg;
			*bg = cells[i].bg;

			if (used + 16 > sizeof(buffer)) {
				termFeed(term, buffer, used);
				used = 0;
			}

			used += (size_t) snprintf((char *) &buffer[used],
				sizeof(buffer) - used, "\033[0;%d;%dm",
				(*fg < 8) ? 30 + *fg : 90 + *fg - 8,
				(*bg < 8) ? 40 + *bg : 100 + *bg - 8);
		}

		if (used + 1 > sizeof(buffer)) {
			termFeed(term, buffer, used);
			used = 0;
		}

		buffer[used++] = cells[i].ch;
	}

	termFeed(term, buffer, used);
}

static void newline(TermScreen *term, unsigned indent, int *fg, int *bg) {
	if (!fresh)
		termFeed(term, (const uint8_t *) "\033[0m\r\n", 6);

	fresh = false;
	*fg = *bg = -1;

	/* Spaces in the default colours, so an indent has no stray background. */
	static const uint8_t spaces[INDENT_MAX] = {
		' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
		' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
	};

	if (indent)
		termFeed(term, spaces, indent);
}

void ircTextWrite(TermScreen *term, const IrcLine *line) {
	static IrcCell cells[IRC_LINE_MAX + 64];
	const size_t   max = sizeof(cells) / sizeof(cells[0]);
	uint8_t        textFg;
	size_t         prefix = prefixCells(line, cells, max, &textFg);
	size_t         count  = prefix + ircTextCells(line->text ? line->text : "",
		textFg, line->highlight, cells + prefix, max - prefix);

	unsigned indent = (prefix <= INDENT_MAX) ? (unsigned) prefix : 2;
	size_t   at     = 0;
	bool     first  = true;
	int      fg = -1, bg = -1;

	do {
		size_t width = first ? TERM_COLS : (TERM_COLS - indent);
		size_t end   = count;

		if (count - at > width) {
			end = at + width;

			/* Back to a space, unless that leaves the row mostly empty. */
			size_t space = end;

			while ((space > at) && (cells[space].ch != ' '))
				space--;

			if (space > at + (width / 3))
				end = space;
		}

		newline(term, first ? 0 : indent, &fg, &bg);
		emit(term, &cells[at], end - at, &fg, &bg);

		at = end;

		while ((at < count) && (cells[at].ch == ' '))
			at++;

		first = false;
	} while (at < count);
}
