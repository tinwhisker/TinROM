/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Internal interfaces of the terminal module. See docs/terminal.md.
 */
#pragma once

#include "comms/config.h"
#include "core/app.h"

/**
 * Runs a session until the user leaves it: dials `bookmark`, or with NULL
 * talks straight to the modem. `name` is for the status line.
 */
void terminalSession(
	TinromContext *ctx, const char *name, const CommsBookmark *bookmark
);

/** Edits a bookmark in place; false if it was abandoned. */
bool terminalEditBookmark(TinromContext *ctx, CommsBookmark *bookmark);
