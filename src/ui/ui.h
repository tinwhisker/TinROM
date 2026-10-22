/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "ui/canvas.h"
#include "ui/font.h"
#include "ui/model.h"
#include "ui/theme.h"

/*
 * Ordering-table layout. The chain is sent starting at the highest bucket, so
 * a HIGHER index is drawn EARLIER and therefore ends up further back.
 *
 *   UI_Z_CLEAR      the framebuffer clear - behind everything
 *   UI_Z_MODEL_FAR  \  the spinning model is bucketed by its own depth
 *   UI_Z_MODEL_NEAR /   somewhere inside this range
 *   UI_Z_UI_FIRST   menus, panels and text - always in front
 *
 * Within a single bucket the chain is LIFO, so the packet allocated last is
 * drawn first. That is the opposite of the painter's order the UI code is
 * written in, so UI primitives do not share a bucket: each one takes the next
 * bucket down from UI_Z_UI_FIRST, which puts later draws in front of earlier
 * ones as expected. 128 of them per frame covers the busiest screen with
 * room to spare; past that the cursor sticks at 0 and ordering within that
 * bucket inverts. Many primitives that do not overlap - a grid's cells - can
 * share one bucket instead: uiShareBucket(). The model still gets 110 buckets, comfortably more
 * than its 90 faces.
 */
#define UI_Z_UI_FIRST   127
#define UI_Z_MODEL_NEAR 128
#define UI_Z_MODEL_FAR  (GPU_ORDERING_TABLE_SIZE - 2)
#define UI_Z_CLEAR      (GPU_ORDERING_TABLE_SIZE - 1)

/*
 * Save icons, uploaded once when a screen opens and drawn by handle after
 * that. Thirty covers two memory cards' worth of directory slots.
 */
#define UI_ICON_SIZE       16
#define UI_ICON_MAX_FRAMES 3
#define UI_ICON_SLOTS      30

typedef struct {
	TextureInfo texture;
	/** 0 means nothing has been uploaded to this handle. */
	uint8_t     frames;
} UIIcon;

typedef struct UIContext UIContext;

/*
 * Something drawn behind the UI in place of the spinning model - see
 * uiSetBackground(). It gets the frame's chain through `ui->chain`, the
 * buckets from UI_Z_MODEL_NEAR to UI_Z_MODEL_FAR to sort into, and at most
 * `wordBudget` words of the packet buffer.
 */
typedef void (*UIBackground)(
	UIContext *ui, unsigned int wordBudget, void *user
);

struct UIContext {
	GPUOrderedDMAChain  chains[2];
	TextureInfo         font;
	bool                secondFrame;
	int                 bufferX, bufferY;
	unsigned int        width, height;
	/* The chain for the frame currently being built. */
	GPUOrderedDMAChain *chain;

	const char         *title;
	const char         *hint;
	/* False for a screen that is all its own, like the terminal's. */
	bool                chromeVisible;

	/* The model idling behind the UI. Absent or unparseable is not fatal. */
	TinromModel         model;
	bool                modelVisible;
	bool                modelReported;
	int                 modelAngleX, modelAngleY, modelAngleZ;

	/* Drawn instead of the model while set. */
	UIBackground        background;
	void               *backgroundUser;

	/* Next ordering-table bucket for a UI primitive; see the note above. */
	unsigned int        uiZ;
	/* Inside uiShareBucket(): everything goes in uiZ; and whether it did. */
	bool                sharingBucket;
	bool                sharedBucketUsed;

	UIIcon              icons[UI_ICON_SLOTS];
};

#ifdef __cplusplus
extern "C" {
#endif

void uiInit(UIContext *ui);

/** Turn the background model on or off. Harmless if there is no model. */
void uiSetModelVisible(UIContext *ui, bool visible);
bool uiModelVisible(const UIContext *ui);

/**
 * Draws `draw` behind the UI every frame instead of the model, until it is
 * set back to NULL. Drawn last, like the model, so it gets whatever packet
 * space the screen has left - the UI in front matters more. A screen that
 * sets one clears it on the way out.
 */
void uiSetBackground(UIContext *ui, UIBackground draw, void *user);

/*
 * Every packet in a frame comes out of one fixed buffer - ps1-bare-metal sizes
 * it at GPU_CHAIN_BUFFER_SIZE words, and a glyph costs about four and a
 * quarter of them (see uiTextCost()). A screen
 * full of text gets close to that, so anything that draws in a loop should ask
 * first. Overrunning it trips an assert that spins forever in a debug build,
 * and corrupts memory in a release one.
 */
bool uiHasRoom(const UIContext *ui, unsigned int words);

/*
 * Between these two, every primitive goes into one bucket instead of taking
 * one each - for drawing many things that do not overlap one another, such
 * as a grid's cells, without running through the 128 the UI has a frame.
 * The bucket is LIFO, so they come out in the reverse of the order drawn;
 * that is only harmless because none overlaps another. Each primitive still
 * comes out whole - a string's page command still goes ahead of its glyphs.
 * Something that must be in front of them is drawn after uiShareBucketEnd().
 */
void uiShareBucket(UIContext *ui);
void uiShareBucketEnd(UIContext *ui);

/** Packet words drawing `text` takes, for uiHasRoom(). An upper bound. */
unsigned int uiTextCost(const char *text);

/** Poll input, start a new frame and clear it. */
void uiBeginFrame(UIContext *ui);
/** Draw the standard header/footer chrome and present. Waits for vsync. */
void uiEndFrame(UIContext *ui);

/** Sets the header title and footer hint for this and following frames. */
void uiSetChrome(UIContext *ui, const char *title, const char *hint);
/** Whether uiEndFrame() draws the header and footer at all. On by default. */
void uiSetChromeVisible(UIContext *ui, bool visible);

void uiFillRect(
	UIContext *ui, int x, int y, int width, int height, uint32_t color
);
/**
 * Fills a rectangle blended half-way into what is already drawn there - the
 * GPU's B/2+F/2 mode. Painting one over a sprite in the colour of the
 * background the sprite sits on is how a deleted save is shown: the sprite
 * goes 50% transparent and the background comes out unchanged, because
 * half of a colour plus half of itself is itself.
 */
void uiVeilRect(
	UIContext *ui, int x, int y, int width, int height, uint32_t color
);
/*
 * Text, in the font described in ui/font.h.
 *
 * `color` is what the font's white comes out as (0xRRGGBB); its shading and
 * outline darken with it. uiText() uses TINROM_COLOR_TEXT. A '\n' starts a
 * new line UI_FONT_LINE_HEIGHT further down, back at `x`, and a '\t' moves
 * on to the next multiple of UI_FONT_TAB_WIDTH.
 *
 * A string is drawn whole or not at all: half a line looks like a bug, a
 * missing line looks like the end of the list. See uiHasRoom().
 */
void uiText(UIContext *ui, int x, int y, const char *text);
void uiTextf(UIContext *ui, int x, int y, const char *format, ...);
void uiTextColored(
	UIContext *ui, int x, int y, uint32_t color, const char *text
);

/**
 * Word-wrapped to `width` units, breaking at spaces as well as at '\n'.
 * Returns how many lines it took, which uiTextLines() can tell in advance.
 */
unsigned int uiTextWrapped(
	UIContext *ui, int x, int y, int width, uint32_t color, const char *text
);
unsigned int uiTextLines(const char *text, int width);

/** The first line of `text`, cut short where it would pass `width` units. */
void uiTextClipped(
	UIContext *ui, int x, int y, int width, uint32_t color, const char *text
);

/** Units covered by the widest line of `text`, rounded up. Not an estimate. */
int  uiTextWidth(const char *text);

/*
 * Small indexed sprites, for the memory card browser.
 *
 * `sheet` is UI_ICON_MAX_FRAMES frames laid out left to right - 48x16 texels
 * at 4bpp - with `palette` giving its sixteen 16-bit entries. It goes to VRAM
 * with every texel doubled across, for the display's two pixels a unit, and
 * is drawn as a sprite. Uploading is a blocking VRAM DMA, so do it when a
 * screen opens, not per frame.
 *
 * Icons are drawn with semi-transparency off, which is what the BIOS does:
 * a palette entry of 0000h is then fully transparent and 8000h is solid
 * black.
 */
void uiIconsClear(UIContext *ui);
/** Drops one handle, so a stale icon is not drawn after its card is pulled. */
void uiIconForget(UIContext *ui, unsigned int slot);
bool uiIconUpload(
	UIContext      *ui,
	unsigned int    slot,
	const uint8_t  *sheet,
	const uint16_t *palette,
	unsigned int    frames
);
bool uiIconPresent(const UIContext *ui, unsigned int slot);
/** Draws icon `slot` with its top-left at (x, y), animating if it has frames. */
void uiIconDraw(UIContext *ui, unsigned int slot, int x, int y);

/**
 * Draws the left `width` units of a text canvas's first `rows` rows
 * (ui/canvas.h), with its top-left at (x, y). Twenty-one packet words at
 * most, however full it is. Upload it first, in the same frame.
 */
void uiCanvasDraw(
	UIContext *ui, const UICanvas *canvas, int x, int y,
	int width, unsigned int rows
);

/**
 * Draws the canvas slot's VRAM (ui/canvas.h) as an 8bpp bitmap through
 * `clut`, `width` units by `height` rows, for anything else that composes
 * into it - the terminal's screen. `stretched` says the bitmap has one texel
 * per unit across, to be drawn two display pixels wide (the terminal);
 * otherwise it has two, like the canvas.
 */
void uiBitmapDraw(
	UIContext *ui, uint16_t clut, int x, int y, int width, int height,
	bool stretched
);

#ifdef __cplusplus
}
#endif
