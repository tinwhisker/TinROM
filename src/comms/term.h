/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * An 80x25 ANSI terminal: the screen a BBS draws on, and the parser that
 * turns what it sends into that screen.
 *
 * What a BBS sends is ANSI-BBS - ECMA-48 / VT100 control sequences in the
 * dialect DOS's ANSI.SYS spoke - in the CP437 character set:
 *
 *   cursor movement and positioning, save and restore; erasing the screen
 *   and lines (in the current background colour); inserting and deleting
 *   lines and characters; a scroll region; SGR colours 30-37/40-47 with
 *   bold as bright, 90-97/100-107, reverse and conceal; the device status
 *   and attribute queries, answered through `reply`; autowrap and cursor
 *   visibility; form feed as clear screen. 256-colour and truecolour SGRs
 *   are taken to the nearest of the sixteen. OSC strings and character set
 *   designations are swallowed.
 *
 * Lines scrolled off the top of a full-screen region go into a history the
 * view can scroll back through.
 *
 * Pure C with no hardware in it, so the host tests drive it directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_COLS    80
#define TERM_ROWS    25
#define TERM_HISTORY 200
#define TERM_PARAMS  16

/* attr: foreground in the low nibble, background in the high one. */
typedef struct {
	uint8_t ch;
	uint8_t attr;
} TermCell;

typedef void (*TermReply)(void *user, const uint8_t *data, size_t length);

typedef struct {
	/*
	 * The screen's rows, in the order order[] gives: screen row r is
	 * lines[order[r]]. Scrolling moves entries of order[] rather than whole
	 * rows of cells, so a line feed at the bottom costs nothing to speak of.
	 * Use termRow().
	 */
	TermCell lines[TERM_ROWS][TERM_COLS];
	uint8_t  order[TERM_ROWS];
	/* A bit per screen row that has changed since the view last drew it. */
	uint32_t dirty;

	int      cx, cy;
	int      savedX, savedY;
	uint8_t  savedFg, savedBg;
	bool     savedBold;
	uint8_t  fg, bg;      /* 0-7, or 8-15 when set bright directly */
	bool     bold, reverse, conceal;
	int      top, bottom; /* scroll region, inclusive */
	bool     wrapPending, autowrap, cursorVisible;

	/* Parser. */
	uint8_t  state;
	int      params[TERM_PARAMS];
	unsigned paramCount;
	bool     paramStarted;
	char     privateMarker;

	/* History, oldest first from historyStart, as a ring. */
	TermCell history[TERM_HISTORY][TERM_COLS];
	unsigned historyStart, historyCount;
	/* Bumped whenever a line is added, so a scrolled-back view can tell. */
	uint32_t historySerial;

	/* Set on BEL; the view clears it once it has flashed. */
	bool     bell;

	TermReply reply;
	void     *user;
	/*
	 * Called every so often while a long run of input is parsed, if set: the
	 * session keeps the serial port drained with it.
	 */
	void    (*idle)(void);
} TermScreen;

/** Screen row `row`'s cells. */
static inline TermCell *termRow(TermScreen *term, int row) {
	return term->lines[term->order[row]];
}

#ifdef __cplusplus
extern "C" {
#endif

void termInit(TermScreen *term, TermReply reply, void *user);
/** Back to power-on: blank, white on black, cursor home, history kept. */
void termReset(TermScreen *term);
void termFeed(TermScreen *term, const uint8_t *data, size_t length);

/**
 * A line `back` lines up from the bottom of the screen, history included, or
 * NULL past the oldest.
 */
const TermCell *termLine(const TermScreen *term, unsigned back);

/** Writes text the way a received string would be, for status messages. */
void termPrint(TermScreen *term, const char *text);

#ifdef __cplusplus
}
#endif
