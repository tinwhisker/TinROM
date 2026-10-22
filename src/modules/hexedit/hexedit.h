/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Internal interfaces of the hex editor module. See docs/hexedit.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "modules/modules.h"
#include "ui/canvas.h"

/* The canvas holds the data rows; the lines above and below are plain text. */
#define HEX_ROWS        16
#define HEX_ROW_BYTES   8
#define HEX_PAGE_BYTES  (HEX_ROWS * HEX_ROW_BYTES) /* 128: one card frame */

/*
 * Where things go along a canvas row, in pixels. Glyphs are 8 wide and start
 * on a multiple of 4. A byte takes two glyphs and a 4-pixel gap, so a unit of
 * n bytes starts every n * HEX_BYTE_PITCH pixels.
 */
#define HEX_X_ADDRESS   0
#define HEX_X_DATA      72
#define HEX_BYTE_PITCH  20
#define HEX_X_ASCII     (HEX_X_DATA + HEX_ROW_BYTES * HEX_BYTE_PITCH + 4)
#define HEX_CANVAS_W    (HEX_X_ASCII + HEX_ROW_BYTES * UI_CANVAS_GLYPH_W)

/* Canvas inks; the colours are in view.c. */
enum {
	HEX_INK_ADDRESS = 1,
	HEX_INK_DATA    = 2,
	HEX_INK_ZERO    = 3,
	HEX_INK_CHANGED = 4,
	HEX_INK_EDITED  = 5,
	HEX_INK_ASCII   = 6,
	HEX_INK_DIM     = 7,
	HEX_INK_BAD     = 8
};

typedef enum {
	HEX_REGION_MEMORY = 0,
	HEX_REGION_CARD   = 1
} HexRegionKind;

typedef struct {
	const char   *name;
	const char   *summary;
	HexRegionKind kind;
	/* Address shown for offset 0. For a card, 0 and `port` says which. */
	uint32_t      base;
	uint32_t      size;
	bool          writable;
	unsigned int  port;
	/* What L2/R2 jump by. */
	uint32_t      bigStep;
} HexRegion;

#define HEX_UNDO_DEPTH 16

typedef struct {
	uint32_t offset;
	uint32_t value;  /* what was there before */
	uint8_t  size;
} HexUndo;

typedef enum {
	HEX_FIND_BYTE = 1,
	HEX_FIND_HALF = 2,
	HEX_FIND_WORD = 4,
	HEX_FIND_TEXT = 0
} HexFindKind;

#define HEX_FIND_TEXT_MAX 16

typedef struct {
	HexFindKind kind;
	uint32_t    value;
	char        text[HEX_FIND_TEXT_MAX + 1];
	bool        valid;
} HexFind;

#define HEX_BACK_DEPTH 8

typedef struct {
	const HexRegion *region;

	uint32_t view;    /* offset of the first byte shown */
	uint32_t cursor;  /* offset of the unit under the cursor */
	unsigned unit;    /* 1, 2 or 4 bytes */

	bool     editing;
	unsigned nibble;  /* 0 is the most significant digit of the unit */
	uint32_t pending; /* the unit's value while it is being edited */

	/* What is on screen, and whether each byte could be read. */
	uint8_t  bytes[HEX_PAGE_BYTES];
	bool     readable;

	/* Recently changed bytes, for memory that something else is writing. */
	uint8_t  previous[HEX_PAGE_BYTES];
	uint8_t  age[HEX_PAGE_BYTES];
	uint32_t previousView;
	bool     havePrevious;

	/* A memory card is edited one frame at a time, in this copy. */
	uint8_t  frame[HEX_PAGE_BYTES];
	int32_t  frameNumber; /* -1: nothing loaded */
	bool     frameLoaded;
	uint8_t  frameDirty[HEX_PAGE_BYTES / 8];

	HexUndo  undo[HEX_UNDO_DEPTH];
	unsigned undoCount;

	uint32_t back[HEX_BACK_DEPTH];
	unsigned backCount;

	HexFind  find;

	/* Warnings already given, so they are given once. */
	uint32_t warned;

	/* One line of news for the value row, shown until the next move. */
	char     message[48];
} HexEditor;

/* regions.c */
extern const HexRegion hexRegions[];
extern const unsigned  hexRegionCount;

bool        hexRead(const HexRegion *region, uint32_t offset, uint32_t size,
	uint32_t *value);
bool        hexWrite(const HexRegion *region, uint32_t offset, uint32_t size,
	uint32_t value);
/** What part of tinrom's own RAM an offset is in, or NULL. */
const char *hexRamOwner(const HexRegion *region, uint32_t offset);
/** An address as the CPU sees it, back to an offset in `region`. */
bool        hexAddressToOffset(const HexRegion *region, uint32_t address,
	uint32_t *offset);

/* card.c */
bool hexCardLoad(HexEditor *ed, uint32_t frameNumber);
bool hexCardDirty(const HexEditor *ed);
bool hexCardByteDirty(const HexEditor *ed, uint32_t index);
void hexCardMark(HexEditor *ed, uint32_t index);
/** Writes the edited frame back, after asking. False if it stayed unwritten. */
bool hexCardWriteBack(TinromContext *ctx, HexEditor *ed);
/** Before leaving a frame with edits: write, discard or stay. */
bool hexCardLeave(TinromContext *ctx, HexEditor *ed);

/* view.c */
void hexViewInit(void);
void hexViewRefresh(HexEditor *ed);
void hexViewDraw(TinromContext *ctx, HexEditor *ed);
/** For popups: the editor as it stands, behind whatever is on top. */
void hexViewBackground(void *editor);

/* dial.c */
bool hexDialHex(TinromContext *ctx, HexEditor *ed, const char *title,
	unsigned digits, uint32_t *value);
bool hexDialText(TinromContext *ctx, HexEditor *ed, const char *title,
	char *text, unsigned max);

/* find.c */
bool hexFindPrompt(TinromContext *ctx, HexEditor *ed);
bool hexFindNext(TinromContext *ctx, HexEditor *ed);

/* hexedit.c */
bool hexMoveTo(TinromContext *ctx, HexEditor *ed, uint32_t offset);
