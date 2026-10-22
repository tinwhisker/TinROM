/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * IRC text onto the terminal's screen: UTF-8 (or Latin-1, from clients that
 * still send it) turned into the PC character set, mIRC's formatting codes
 * into colours, and each line word-wrapped at 80 columns with what follows
 * the first row indented under the message.
 *
 * mIRC's codes, as modern.ircdocs.horse documents them: 02h bold (shown as
 * bright), 03h colours 0-15 (and 99, the default; 16-98 have no match in
 * sixteen colours and are shown as the default), 0Fh reset, 16h reverse.
 * Italic, underline, strike-through, monospace and 04h hex colours are taken
 * out, since the screen cannot show them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "comms/irc.h"
#include "comms/term.h"

typedef struct {
	uint8_t ch; /* CP437 */
	uint8_t fg, bg;
} IrcCell;

#ifdef __cplusplus
extern "C" {
#endif

/** A blank screen with the cursor hidden, ready for the first line. */
void ircTextReset(TermScreen *term);

/** One line, wrapped and coloured, below whatever is on screen. */
void ircTextWrite(TermScreen *term, const IrcLine *line);

/**
 * `text` as cells, starting in colour `fg` on black. Returns how many; at
 * most `max`.
 */
size_t ircTextCells(const char *text, uint8_t fg, bool highlight,
	IrcCell *out, size_t max);

/**
 * `text` with the codes out and anything outside ASCII as '?', for the menu
 * font - which is ASCII - to show a topic or a nick with.
 */
void ircTextPlain(const char *text, char *out, size_t size);

/** One code point of UTF-8 from `*text`, moving it on; Latin-1 if invalid. */
uint32_t ircTextDecode(const char **text);
/** A code point in CP437, or '?'. */
uint8_t  ircTextCp437(uint32_t codepoint);

#ifdef __cplusplus
}
#endif
