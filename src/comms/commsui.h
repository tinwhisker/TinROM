/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The screens the terminal and the IRC client share: the modem's settings,
 * and saving and loading the bookmarks, phrases and modem settings they keep
 * together (comms/config.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "comms/config.h"
#include "ui/menu.h"

typedef struct TinromContext TinromContext;

/* Tags for the three saving rows; a list's own tags must stay below. */
enum {
	COMMS_UI_TAG_SAVE = 0x200,
	COMMS_UI_TAG_SAVE_AS,
	COMMS_UI_TAG_LOAD
};

/* Room the saving rows need in a caller's summary buffer. */
#define COMMS_UI_SUMMARY_MAX 48

#ifdef __cplusplus
extern "C" {
#endif

/** A frame saying what is happening, for the second or so a card takes. */
void commsUiBusy(TinromContext *ctx, const char *title, const char *text);

/** The first time either client is entered: the newest save, either card. */
void commsUiLoadOnce(TinromContext *ctx, const char *title);

/** Rate, init string, dial prefix, how to hang up. */
void commsUiModemSettings(TinromContext *ctx);

/**
 * Appends Save, Save as... and Load from... to `items` at `count`, and
 * returns the new count. `summary` holds the Save row's text, and must last
 * as long as the menu does.
 */
size_t commsUiSaveItems(UIMenuItem *items, size_t count,
	char summary[COMMS_UI_SUMMARY_MAX]);

/** Runs a COMMS_UI_TAG_* row. False for any other tag. */
bool commsUiSaveAction(TinromContext *ctx, uintptr_t tag, const char *title);

#ifdef __cplusplus
}
#endif
