/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The ANSI terminal. See term.h. Written from ECMA-48 and the VT100 user
 * guide's description of the sequences, as ANSI.SYS-era BBSes use them; see
 * NOTICE.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "comms/term.h"
#include "libc/string.h"

enum {
	S_GROUND = 0,
	S_ESC,
	S_CSI,
	S_OSC,     /* ESC ] ... until BEL or ESC \ */
	S_OSC_ESC,
	S_SKIP_ONE /* ESC ( B and friends: one more byte, then done */
};

#define DEFAULT_FG 7
#define DEFAULT_BG 0

static void markRow(TermScreen *term, int row) {
	if ((row >= 0) && (row < TERM_ROWS))
		term->dirty |= 1u << row;
}

static void markRows(TermScreen *term, int from, int to) {
	for (int row = from; row <= to; row++)
		markRow(term, row);
}

static uint8_t currentAttr(const TermScreen *term) {
	uint8_t fg = term->fg, bg = term->bg;

	if (term->bold && (fg < 8))
		fg += 8;
	if (term->reverse) {
		uint8_t swap = fg;

		fg = bg;
		bg = swap;
	}
	if (term->conceal)
		fg = bg;

	return (uint8_t) ((fg & 15) | ((bg & 15) << 4));
}

/* Erasing uses the background colour, as every BBS assumes (BCE). */
static TermCell blankCell(const TermScreen *term) {
	uint8_t bg = term->reverse ? term->fg : term->bg;

	return (TermCell) { ' ', (uint8_t) (DEFAULT_FG | ((bg & 15) << 4)) };
}

static void clearCells(TermScreen *term, int row, int from, int to) {
	TermCell blank = blankCell(term);

	if (from < 0)
		from = 0;
	if (to >= TERM_COLS)
		to = TERM_COLS - 1;

	for (int col = from; col <= to; col++)
		termRow(term, row)[col] = blank;

	markRow(term, row);
}

static void clearRows(TermScreen *term, int from, int to) {
	for (int row = from; row <= to; row++)
		clearCells(term, row, 0, TERM_COLS - 1);
}

static void pushHistory(TermScreen *term, int row) {
	unsigned slot = (term->historyStart + term->historyCount) % TERM_HISTORY;

	memcpy(term->history[slot], termRow(term, row), sizeof(term->history[slot]));

	if (term->historyCount < TERM_HISTORY)
		term->historyCount++;
	else
		term->historyStart = (term->historyStart + 1) % TERM_HISTORY;

	term->historySerial++;
}

/*
 * Scrolling a region rotates its entries in order[]; the row that comes in
 * is cleared. `count` is at most the region's height - past that, the whole
 * region is simply blank. Only a real scroll of the whole screen feeds the
 * history: a line deleted with CSI M is gone.
 */
static void scrollUp(TermScreen *term, int top, int bottom, int count,
	bool toHistory) {
	int height = bottom - top + 1;

	if (count > height)
		count = height;

	toHistory = toHistory && (top == 0) && (bottom == TERM_ROWS - 1);

	for (int i = 0; i < count; i++) {
		if (toHistory)
			pushHistory(term, top);

		uint8_t first = term->order[top];

		memmove(&term->order[top], &term->order[top + 1], (size_t) (height - 1));
		term->order[bottom] = first;
		clearCells(term, bottom, 0, TERM_COLS - 1);
	}

	markRows(term, top, bottom);
}

static void scrollDown(TermScreen *term, int top, int bottom, int count) {
	int height = bottom - top + 1;

	if (count > height)
		count = height;

	for (int i = 0; i < count; i++) {
		uint8_t last = term->order[bottom];

		memmove(&term->order[top + 1], &term->order[top], (size_t) (height - 1));
		term->order[top] = last;
		clearCells(term, top, 0, TERM_COLS - 1);
	}

	markRows(term, top, bottom);
}

static void lineFeed(TermScreen *term) {
	term->wrapPending = false;

	if (term->cy == term->bottom)
		scrollUp(term, term->top, term->bottom, 1, true);
	else if (term->cy < TERM_ROWS - 1)
		term->cy++;
}

static void reverseIndex(TermScreen *term) {
	term->wrapPending = false;

	if (term->cy == term->top)
		scrollDown(term, term->top, term->bottom, 1);
	else if (term->cy > 0)
		term->cy--;
}

static void moveTo(TermScreen *term, int col, int row) {
	if (col < 0)
		col = 0;
	if (col >= TERM_COLS)
		col = TERM_COLS - 1;
	if (row < 0)
		row = 0;
	if (row >= TERM_ROWS)
		row = TERM_ROWS - 1;

	term->cx          = col;
	term->cy          = row;
	term->wrapPending = false;
}

static void putChar(TermScreen *term, uint8_t ch) {
	if (term->wrapPending) {
		term->cx = 0;
		lineFeed(term);
	}

	termRow(term, term->cy)[term->cx] = (TermCell) { ch, currentAttr(term) };
	markRow(term, term->cy);

	if (term->cx < TERM_COLS - 1)
		term->cx++;
	else if (term->autowrap)
		term->wrapPending = true;
}

void termReset(TermScreen *term) {
	term->fg            = DEFAULT_FG;
	term->bg            = DEFAULT_BG;
	term->bold          = false;
	term->reverse       = false;
	term->conceal       = false;
	term->top           = 0;
	term->bottom        = TERM_ROWS - 1;
	term->autowrap      = true;
	term->cursorVisible = true;
	term->state         = S_GROUND;
	term->savedX        = term->savedY = 0;
	term->savedFg       = DEFAULT_FG;
	term->savedBg       = DEFAULT_BG;
	term->savedBold     = false;

	clearRows(term, 0, TERM_ROWS - 1);
	moveTo(term, 0, 0);
}

void termInit(TermScreen *term, TermReply reply, void *user) {
	memset(term, 0, sizeof(*term));
	term->reply = reply;
	term->user  = user;

	for (int row = 0; row < TERM_ROWS; row++)
		term->order[row] = (uint8_t) row;

	termReset(term);
}

/* --- SGR ------------------------------------------------------------------ */

static uint8_t fromRgb(int r, int g, int b);

/* The nearest of the sixteen for an xterm 256-colour index. */
static uint8_t from256(int index) {
	if (index < 16)
		return (uint8_t) index;

	int r, g, b;

	if (index >= 232) {
		r = g = b = 8 + (index - 232) * 10;
	} else {
		index -= 16;
		r = (index / 36) * 51;
		g = ((index / 6) % 6) * 51;
		b = (index % 6) * 51;
	}

	return fromRgb(r, g, b);
}

static uint8_t fromRgb(int r, int g, int b) {
	int high = (r > g) ? r : g;

	high = (high > b) ? high : b;

	return (uint8_t) (((r > high / 2 && r > 64) ? 1 : 0)
		| ((g > high / 2 && g > 64) ? 2 : 0)
		| ((b > high / 2 && b > 64) ? 4 : 0)
		| ((high > 191) ? 8 : 0));
}

static void selectGraphics(TermScreen *term) {
	if (!term->paramCount) {
		term->params[0]  = 0;
		term->paramCount = 1;
	}

	for (unsigned i = 0; i < term->paramCount; i++) {
		int p = term->params[i];

		if (p == 0) {
			term->fg      = DEFAULT_FG;
			term->bg      = DEFAULT_BG;
			term->bold    = false;
			term->reverse = false;
			term->conceal = false;
		} else if (p == 1) {
			term->bold = true;
		} else if ((p == 2) || (p == 22)) {
			term->bold = false;
		} else if (p == 7) {
			term->reverse = true;
		} else if (p == 27) {
			term->reverse = false;
		} else if (p == 8) {
			term->conceal = true;
		} else if (p == 28) {
			term->conceal = false;
		} else if ((p >= 30) && (p <= 37)) {
			term->fg = (uint8_t) (p - 30);
		} else if (p == 39) {
			term->fg = DEFAULT_FG;
		} else if ((p >= 40) && (p <= 47)) {
			term->bg = (uint8_t) (p - 40);
		} else if (p == 49) {
			term->bg = DEFAULT_BG;
		} else if ((p >= 90) && (p <= 97)) {
			term->fg = (uint8_t) (p - 90 + 8);
		} else if ((p >= 100) && (p <= 107)) {
			term->bg = (uint8_t) (p - 100 + 8);
		} else if ((p == 38) || (p == 48)) {
			uint8_t colour = 0;

			if ((i + 2 < term->paramCount) && (term->params[i + 1] == 5)) {
				colour = from256(term->params[i + 2]);
				i += 2;
			} else if ((i + 4 < term->paramCount)
				&& (term->params[i + 1] == 2)) {
				colour = fromRgb(term->params[i + 2], term->params[i + 3],
					term->params[i + 4]);
				i += 4;
			} else {
				continue;
			}

			if (p == 38)
				term->fg = colour;
			else
				term->bg = colour;
		}
		/* 4 (underline), 5 and 6 (blink) and the rest: not shown. */
	}
}

/* --- CSI ------------------------------------------------------------------ */

static int param(const TermScreen *term, unsigned index, int fallback) {
	if ((index >= term->paramCount) || !term->params[index])
		return fallback;

	return term->params[index];
}

static void replyText(TermScreen *term, const char *text) {
	if (term->reply)
		term->reply(term->user, (const uint8_t *) text, strlen(text));
}

static void setMode(TermScreen *term, bool on) {
	if (term->privateMarker != '?')
		return;

	for (unsigned i = 0; i < term->paramCount; i++) {
		switch (term->params[i]) {
			case 7:
				term->autowrap = on;
				break;

			case 25:
				term->cursorVisible = on;
				break;
		}
	}
}

static void eraseInDisplay(TermScreen *term, int mode) {
	switch (mode) {
		case 0:
			clearCells(term, term->cy, term->cx, TERM_COLS - 1);
			clearRows(term, term->cy + 1, TERM_ROWS - 1);
			break;

		case 1:
			clearRows(term, 0, term->cy - 1);
			clearCells(term, term->cy, 0, term->cx);
			break;

		default:
			/* ANSI.SYS homes the cursor here, and BBSes count on it. */
			clearRows(term, 0, TERM_ROWS - 1);
			moveTo(term, 0, 0);
			break;
	}
}

static void eraseInLine(TermScreen *term, int mode) {
	switch (mode) {
		case 0:  clearCells(term, term->cy, term->cx, TERM_COLS - 1); break;
		case 1:  clearCells(term, term->cy, 0, term->cx);             break;
		default: clearCells(term, term->cy, 0, TERM_COLS - 1);        break;
	}
}

static void insertChars(TermScreen *term, int count) {
	TermCell *row = termRow(term, term->cy);

	if (count > TERM_COLS - term->cx)
		count = TERM_COLS - term->cx;

	memmove(&row[term->cx + count], &row[term->cx],
		(size_t) (TERM_COLS - term->cx - count) * sizeof(TermCell));
	clearCells(term, term->cy, term->cx, term->cx + count - 1);
}

static void deleteChars(TermScreen *term, int count) {
	TermCell *row = termRow(term, term->cy);

	if (count > TERM_COLS - term->cx)
		count = TERM_COLS - term->cx;

	memmove(&row[term->cx], &row[term->cx + count],
		(size_t) (TERM_COLS - term->cx - count) * sizeof(TermCell));
	clearCells(term, term->cy, TERM_COLS - count, TERM_COLS - 1);
}

static void dispatchCsi(TermScreen *term, uint8_t final) {
	int n = param(term, 0, 1);

	switch (final) {
		case 'A': moveTo(term, term->cx, term->cy - n); break;
		case 'B': moveTo(term, term->cx, term->cy + n); break;
		case 'C': moveTo(term, term->cx + n, term->cy); break;
		case 'D': moveTo(term, term->cx - n, term->cy); break;
		case 'E': moveTo(term, 0, term->cy + n);        break;
		case 'F': moveTo(term, 0, term->cy - n);        break;
		case 'G': moveTo(term, n - 1, term->cy);        break;
		case 'd': moveTo(term, term->cx, n - 1);        break;

		case 'H':
		case 'f':
			moveTo(term, param(term, 1, 1) - 1, param(term, 0, 1) - 1);
			break;

		case 'J': eraseInDisplay(term, param(term, 0, 0)); break;
		case 'K': eraseInLine(term, param(term, 0, 0));    break;

		case 'L':
			if ((term->cy >= term->top) && (term->cy <= term->bottom))
				scrollDown(term, term->cy, term->bottom, n);
			break;

		case 'M':
			if ((term->cy >= term->top) && (term->cy <= term->bottom))
				scrollUp(term, term->cy, term->bottom, n, false);
			break;

		case '@': insertChars(term, n); break;
		case 'P': deleteChars(term, n); break;

		case 'X':
			clearCells(term, term->cy, term->cx, term->cx + n - 1);
			break;

		case 'S': scrollUp(term, term->top, term->bottom, n, true); break;
		case 'T': scrollDown(term, term->top, term->bottom, n); break;

		case 'm':
			selectGraphics(term);
			break;

		case 'r': {
			int top    = param(term, 0, 1) - 1;
			int bottom = param(term, 1, TERM_ROWS) - 1;

			if ((top >= 0) && (bottom < TERM_ROWS) && (top < bottom)) {
				term->top    = top;
				term->bottom = bottom;
			} else {
				term->top    = 0;
				term->bottom = TERM_ROWS - 1;
			}

			moveTo(term, 0, 0);
			break;
		}

		case 's':
			term->savedX = term->cx;
			term->savedY = term->cy;
			break;

		case 'u':
			moveTo(term, term->savedX, term->savedY);
			break;

		case 'n':
			if (param(term, 0, 0) == 6) {
				char text[32];

				snprintf(text, sizeof(text), "\033[%d;%dR",
					term->cy + 1, term->cx + 1);
				replyText(term, text);
			} else if (param(term, 0, 0) == 5) {
				replyText(term, "\033[0n");
			}
			break;

		case 'c':
			if (!term->privateMarker)
				replyText(term, "\033[?1;0c");
			break;

		case 'h': setMode(term, true);  break;
		case 'l': setMode(term, false); break;

		default:
			break;
	}
}

/* --- the parser ------------------------------------------------------------ */

static void startSequence(TermScreen *term) {
	term->paramCount    = 0;
	term->paramStarted  = false;
	term->privateMarker = 0;
	memset(term->params, 0, sizeof(term->params));
}

static void control(TermScreen *term, uint8_t byte) {
	switch (byte) {
		case 0x07:
			term->bell = true;
			break;

		case 0x08:
			if (term->cx > 0)
				term->cx--;
			term->wrapPending = false;
			break;

		case 0x09: {
			int next = (term->cx / 8 + 1) * 8;

			moveTo(term, (next < TERM_COLS) ? next : TERM_COLS - 1, term->cy);
			break;
		}

		case 0x0a:
		case 0x0b:
			lineFeed(term);
			break;

		case 0x0c:
			/* Form feed clears the screen on a BBS. */
			eraseInDisplay(term, 2);
			break;

		case 0x0d:
			term->cx          = 0;
			term->wrapPending = false;
			break;

		case 0x1b:
			term->state = S_ESC;
			break;

		default:
			/* NUL, SO, SI and the rest of C0: nothing. */
			break;
	}
}

static void escape(TermScreen *term, uint8_t byte) {
	term->state = S_GROUND;

	switch (byte) {
		case '[':
			startSequence(term);
			term->state = S_CSI;
			break;

		case ']':
			term->state = S_OSC;
			break;

		case '(': case ')': case '*': case '+': case '#':
			term->state = S_SKIP_ONE;
			break;

		case '7':
			term->savedX    = term->cx;
			term->savedY    = term->cy;
			term->savedFg   = term->fg;
			term->savedBg   = term->bg;
			term->savedBold = term->bold;
			break;

		case '8':
			moveTo(term, term->savedX, term->savedY);
			term->fg   = term->savedFg;
			term->bg   = term->savedBg;
			term->bold = term->savedBold;
			break;

		case 'D': lineFeed(term);     break;
		case 'M': reverseIndex(term); break;

		case 'E':
			term->cx = 0;
			lineFeed(term);
			break;

		case 'c':
			termReset(term);
			break;

		default:
			break;
	}
}

static void csi(TermScreen *term, uint8_t byte) {
	if ((byte >= '0') && (byte <= '9')) {
		if (!term->paramStarted) {
			term->paramStarted = true;

			if (term->paramCount < TERM_PARAMS)
				term->paramCount++;
		}

		int *value = &term->params[term->paramCount - 1];

		if (*value < 10000)
			*value = *value * 10 + (byte - '0');
	} else if ((byte == ';') || (byte == ':')) {
		/* An empty parameter counts, as a default. */
		if (!term->paramStarted && (term->paramCount < TERM_PARAMS))
			term->paramCount++;

		term->paramStarted = false;
	} else if ((byte >= '<') && (byte <= '?')) {
		term->privateMarker = (char) byte;
	} else if ((byte >= 0x40) && (byte <= 0x7e)) {
		dispatchCsi(term, byte);
		term->state = S_GROUND;
	} else if (byte == 0x1b) {
		term->state = S_ESC;
	} else if (byte < 0x20) {
		/* C0 inside a sequence is acted on, per ECMA-48. */
		control(term, byte);
		term->state = S_CSI;
	}
	/* Intermediates (20h-2Fh) are ignored. */
}

void termFeed(TermScreen *term, const uint8_t *data, size_t length) {
	for (size_t i = 0; i < length; i++) {
		uint8_t byte = data[i];

		if (term->idle && !(i & 63))
			term->idle();

		switch (term->state) {
			case S_GROUND:
				if ((byte < 0x20) || (byte == 0x7f))
					control(term, byte);
				else
					putChar(term, byte);
				break;

			case S_ESC:
				escape(term, byte);
				break;

			case S_CSI:
				csi(term, byte);
				break;

			case S_OSC:
				if (byte == 0x07)
					term->state = S_GROUND;
				else if (byte == 0x1b)
					term->state = S_OSC_ESC;
				break;

			case S_OSC_ESC:
				term->state = (byte == '\\') ? S_GROUND : S_OSC;
				break;

			case S_SKIP_ONE:
				term->state = S_GROUND;
				break;
		}
	}
}

const TermCell *termLine(const TermScreen *term, unsigned back) {
	if (back < TERM_ROWS)
		return term->lines[term->order[TERM_ROWS - 1 - back]];

	back -= TERM_ROWS;

	if (back >= term->historyCount)
		return NULL;

	unsigned index = term->historyCount - 1 - back;

	return term->history[(term->historyStart + index) % TERM_HISTORY];
}

void termPrint(TermScreen *term, const char *text) {
	termFeed(term, (const uint8_t *) text, strlen(text));
}
