/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Immediate-mode drawing over ps1-bare-metal's GPU helpers. Two DMA chains,
 * two framebuffers one above the other in VRAM, one chain built per frame.
 *
 * Callers place everything in layout units on a 320-wide screen; the
 * display is 640 wide (hal/video.h), and this file is where x and widths
 * are doubled - see dx(). Heights and y are the same in both.
 *
 * The double-buffering pattern follows ps1-bare-metal's example 11 (MIT,
 * spicyjpeg); see NOTICE.md. Text is tinrom's own renderer, drawing the font
 * in ui/font.h.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "common/gpu.h"
#include "common/gte.h"
#include "core/log.h"
#include "hal/pad.h"
#include "hal/timer.h"
#include "hal/video.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"
#include "libc/string.h"
#include "ui/font.h"
#include "ui/model.h"
#include "ui/theme.h"
#include "ui/ui.h"

/* The font sheet sits right of both framebuffers; see the layout below. */
#define FONT_VRAM_X 640
#define FONT_VRAM_Y   0
#define FONT_CLUT_X 640
#define FONT_CLUT_Y UI_FONT_SHEET_HEIGHT

/*
 * The background model can be left out of a build (TINROM_MODEL and
 * TINROM_MCR_MODEL in cmake/options.cmake) - the memory card image is short
 * of room. Without it the assets are not embedded and nothing below may
 * name them; the menu then has a plain background.
 */
#ifndef TINROM_WITH_MODEL
#define TINROM_WITH_MODEL 1
#endif

#if TINROM_WITH_MODEL
/* Embedded by tinromAddBinaryFile(), cmake/payload.cmake. */
extern const uint8_t romMesh[], romTexture[], romPalette[];
extern const uint32_t romMeshSize;
#endif

/*
 * VRAM layout, in 16-bit words. The two 640x240 framebuffers - 640x256 for
 * PAL - are stacked at (0,0) and (0,256), filling x 0..639 top to bottom;
 * everything else is to their right.
 *
 *   (640,  0)  font sheet, 4bpp, 256x45 ->  64 words wide (ui/font.h)
 *   (640, 45)  font palette, 16 entries
 *   (768,  0)  model texture, 8bpp, 128x128 -> 64 words wide
 *   (768,128)  model palette, 256 entries
 *   (640,176)  save icon palettes, one 16-entry row per strip
 *   (640,208)  save icon sheets, 4bpp, thirty 96x16 strips on three rows
 *   (640,256)  text canvas, 8bpp, 640 wide (ui/canvas.c) -> 320 words wide
 *   (640,496)  text canvas palette, 256 entries
 *
 * 768 is a multiple of 64, so the model texture sits on a texture-page
 * boundary and its texels address from u=0. A CLUT must start on a multiple
 * of 16, which 768 also satisfies.
 */
/*
 * Save icons. A card's icon is 16x16, three frames at most, and the display
 * has two pixels to a unit, so each is stored twice as wide as the card has
 * it: 96x16 texels at 4bpp, each texel doubled across, which is 24 VRAM
 * halfwords. Drawn as a sprite, every pixel then comes from exactly the texel
 * meant. They used to be stretched by a textured quad instead, and the GPU's
 * interpolation of a quad's u reached the column just past the frame at its
 * right edge - the next frame's, or the next icon's - as a stray line.
 *
 * They cannot simply run along one row. A 4bpp texture page is 64 halfwords
 * wide and a sprite's u is only 8 bits (256 texels), so a strip may start no
 * more than 40 halfwords into a page: two strips to a page, at 0 and 24. Six
 * pages fill x 640..1023, so twelve strips to a row of 16 lines, and three
 * rows at y 208, 224 and 240 hold thirty-six - clear of the font, the model,
 * the palettes above and the canvas below.
 */
#define ICON_SHEET_WIDTH (UI_ICON_SIZE * 2 * UI_ICON_MAX_FRAMES)
#define ICON_HALFWORDS   (ICON_SHEET_WIDTH / 4)
#define ICON_PAGE_WIDTH  64
#define ICON_PER_PAGE    ((ICON_PAGE_WIDTH - ICON_HALFWORDS) / ICON_HALFWORDS + 1)
#define ICON_PAGES       6
#define ICON_PER_ROW     (ICON_PER_PAGE * ICON_PAGES)
#define ICON_VRAM_X      640
#define ICON_VRAM_Y      208
#define ICON_CLUT_X      640
#define ICON_CLUT_Y      176

_Static_assert(((UI_ICON_SLOTS + ICON_PER_ROW - 1) / ICON_PER_ROW)
	* UI_ICON_SIZE + ICON_VRAM_Y <= 256,
	"the icon rows run into the canvas");
_Static_assert(TINROM_SCREEN_SCALE_X == 2,
	"icons are stored doubled across, for two pixels to a unit");
_Static_assert(ICON_CLUT_Y + UI_ICON_SLOTS <= ICON_VRAM_Y,
	"the icon palettes run into the icons");

/* psx-spx: two frames change every 16 PAL frames, three every 11. */
#define ICON_PERIOD_2 16
#define ICON_PERIOD_3 11

#define MODEL_TEXTURE_X 768
#define MODEL_TEXTURE_Y   0
#define MODEL_CLUT_X    768
#define MODEL_CLUT_Y    128

/*
 * How the model sits on screen. Distance is in the same units as the model's
 * own vertices (mkmesh reported a span of about 64), so this is roughly "one
 * and a bit model widths away".
 */
/*
 * Where the model sits and how it moves. These four are the whole of it:
 *
 *   MODEL_SCREEN_X / _Y_BIAS  where it sits, in screen pixels. X is absolute;
 *                             Y is an offset from the vertical centre, so it
 *                             stays put between NTSC's 240 lines and PAL's
 *                             256. The default parks it right of centre,
 *                             clear of the menu labels down the left.
 *   MODEL_DISTANCE            how big it looks. Smaller is nearer and bigger.
 *                             Position and size are independent, so changing
 *                             this scales it in place.
 *   MODEL_TILT_X              fixed tilt, so it is seen slightly from above
 *                             rather than exactly edge-on.
 *   MODEL_SPIN_SPEED          angle units added per frame.
 *
 * Angles are in units where 4096 is a full turn, so at 60Hz a speed of 2 is
 * one revolution every ~34 seconds.
 *
 * Keep MODEL_DISTANCE comfortably above the model's radius: the GTE's
 * perspective divide overflows once a vertex's Z reaches H/2, and H is half
 * the smaller screen dimension - 120 at 640x240, as it was at 320x240. At
 * 140 with this model's radius of 40 the nearest vertex sits at 100, which
 * is ample.
 */
/*
 * How much of the frame's packet buffer the model may take. The rest is left
 * for the UI, which draws on top of it and matters more: a text-heavy screen
 * like the credits needs most of the buffer to itself.
 *
 * 700 words is 70 quads. The mesh is split at conversion time so its big
 * faces can sort (docs/model-sorting.md), which took it from 90 faces to 126
 * and the peak visible count from 45 to 63 - so 630 words at the worst
 * rotation. Cutting the model off mid-draw looks worse than the sorting
 * artefact it was split to fix, so the budget follows the mesh.
 */
/*
 * The most the model may take even when the screen is otherwise empty. It is
 * a ceiling on GPU time rather than on memory, now that the model draws with
 * whatever the UI has left: 500 words is 50 quads, and the mesh peaks at 45
 * visible after backface culling.
 */
#define MODEL_WORD_BUDGET 500

/* Left free at the end of the buffer so the caller's own tag always fits. */
#define MODEL_TAIL_MARGIN 8

#define MODEL_SCREEN_X    242 /* units, like everything else */
#define MODEL_SCREEN_Y_BIAS 50
#define MODEL_DISTANCE    100
#define MODEL_SPIN_SPEED    4
#define MODEL_TILT_X      300

_Static_assert(UI_FONT_SCALE_X == TINROM_SCREEN_SCALE_X,
	"the font is drawn for the display's pixels");
_Static_assert(UI_CANVAS_SCALE_X == TINROM_SCREEN_SCALE_X,
	"the canvas is drawn one texel to a pixel");

/* Layout units across to display pixels. */
static int dx(int x) {
	return x * TINROM_SCREEN_SCALE_X;
}

void uiInit(UIContext *ui) {
	ui->width       = TINROM_SCREEN_WIDTH;
	ui->height      = tinromVideoHeight();
	ui->secondFrame  = false;
	ui->chain        = &ui->chains[0];
	ui->title        = "tinrom";
	ui->hint         = "";
	ui->chromeVisible = true;
	ui->modelAngleX  = MODEL_TILT_X;
	ui->modelAngleY  = 0;
	ui->modelAngleZ  = 0;
	ui->modelVisible = false;
	ui->background   = NULL;

	uiIconsClear(ui);

	setupGTE(TINROM_SCREEN_DEVICE_WIDTH, tinromVideoHeight());

	uploadIndexedTexture(
		&ui->font, uiFontSheet, uiFontClut,
		FONT_VRAM_X, FONT_VRAM_Y, FONT_CLUT_X, FONT_CLUT_Y,
		UI_FONT_SHEET_WIDTH, UI_FONT_SHEET_HEIGHT, GP0_COLOR_4BPP
	);

	/*
	 * The model is decoration. If the blob is missing or malformed the menu
	 * should still come up, so a failure here is logged and dropped.
	 */
#if TINROM_WITH_MODEL
	TinromResult result = tinromModelParse(romMesh, romMeshSize, &ui->model);

	if (result == TINROM_OK) {
		tinromModelUploadTexture(
			&ui->model, romTexture, romPalette,
			MODEL_TEXTURE_X, MODEL_TEXTURE_Y,
			MODEL_CLUT_X, MODEL_CLUT_Y,
			GP0_COLOR_8BPP
		);
		ui->modelVisible = true;

		TINROM_LOG("model: %d faces, %d verts",
			(int) ui->model.faceCount, (int) ui->model.vertexCount);
	} else {
		TINROM_LOG("model: %s", tinromResultString(result));
	}
#else
	ui->model.valid = false;
	TINROM_LOG("model: not in this build");
#endif
}

void uiSetModelVisible(UIContext *ui, bool visible) {
	ui->modelVisible = visible && ui->model.valid;
}

bool uiModelVisible(const UIContext *ui) {
	return ui->modelVisible;
}

void uiSetBackground(UIContext *ui, UIBackground draw, void *user) {
	ui->background     = draw;
	ui->backgroundUser = user;
}

bool uiHasRoom(const UIContext *ui, unsigned int words) {
	const GPUOrderedDMAChain *chain = ui->chain;
	unsigned int              used  =
		(unsigned int) (chain->nextPacket - chain->data);

	/* Leave a little at the end so the caller's own tag always fits. */
	return (used + words + 2) < GPU_CHAIN_BUFFER_SIZE;
}

/*
 * Glyphs are four GP0 words each, packed GLYPHS_PER_PACKET to a linked-list
 * node - ps1-bare-metal caps a node at 16 words - and a node costs one tag.
 * The texture page is one more node, two words, once per string.
 */
#define GLYPHS_PER_PACKET 4

static unsigned int glyphsCost(unsigned int glyphs) {
	return (glyphs * 4) + ((glyphs + GLYPHS_PER_PACKET - 1) / GLYPHS_PER_PACKET)
		+ 2;
}

/* Characters that put something on screen: not spaces, tabs or newlines. */
static bool drawsSomething(char c) {
	return (c != '\t') && (c != '\n') && uiFontGlyph(c)->width;
}

unsigned int uiTextCost(const char *text) {
	unsigned int glyphs = 0;

	for (const char *c = text; *c; c++) {
		if (drawsSomething(*c))
			glyphs++;
	}

	return glyphsCost(glyphs);
}

void uiSetChromeVisible(UIContext *ui, bool visible) {
	ui->chromeVisible = visible;
}

void uiSetChrome(UIContext *ui, const char *title, const char *hint) {
	ui->title = title ? title : "";
	ui->hint  = hint  ? hint  : "";
}

void uiBeginFrame(UIContext *ui) {
	tinromPadPoll();

	/* 256 apart, so PAL's taller frames fit as well as NTSC's. */
	ui->height  = tinromVideoHeight();
	ui->bufferX = 0;
	ui->bufferY = ui->secondFrame ? 256 : 0;
	ui->chain   = &ui->chains[ui->secondFrame ? 1 : 0];

	ui->secondFrame = !ui->secondFrame;

	GPUOrderedDMAChain *chain = ui->chain;
	uint32_t           *ptr;

	clearOrderingTable(chain->orderingTable, GPU_ORDERING_TABLE_SIZE);
	chain->nextPacket = chain->data;
	ui->uiZ           = UI_Z_UI_FIRST;
	ui->sharingBucket = false;

	/*
	 * Both of these go in the backmost bucket, and the clear is allocated
	 * first so that - the bucket being LIFO - the drawing area is set up
	 * before the fill runs.
	 */
	ptr    = allocateOrderedGP0Packet(chain, UI_Z_CLEAR, 3);
	ptr[0] = gp0_rgb(
		TINROM_RGB_R(TINROM_COLOR_BACKGROUND),
		TINROM_RGB_G(TINROM_COLOR_BACKGROUND),
		TINROM_RGB_B(TINROM_COLOR_BACKGROUND)
	) | gp0_vramFill();
	ptr[1] = gp0_xy(ui->bufferX, ui->bufferY);
	ptr[2] = gp0_xy(TINROM_SCREEN_DEVICE_WIDTH, ui->height);

	ptr    = allocateOrderedGP0Packet(chain, UI_Z_CLEAR, 4);
	ptr[0] = gp0_setPage(0, true, false);
	ptr[1] = gp0_fbOffset1(ui->bufferX, ui->bufferY);
	ptr[2] = gp0_fbOffset2(
		ui->bufferX + TINROM_SCREEN_DEVICE_WIDTH - 1,
		ui->bufferY + ui->height - 1
	);
	ptr[3] = gp0_fbOrigin(ui->bufferX, ui->bufferY);
}

/*
 * One bucket per UI primitive, walking forwards. See the note in ui.h.
 */
static unsigned int nextUIBucket(UIContext *ui) {
	unsigned int z = ui->uiZ;

	if (ui->sharingBucket) {
		ui->sharedBucketUsed = true;
		return z;
	}

	if (ui->uiZ)
		ui->uiZ--;

	return z;
}

void uiShareBucket(UIContext *ui) {
	ui->sharingBucket    = true;
	ui->sharedBucketUsed = false;
}

void uiShareBucketEnd(UIContext *ui) {
	ui->sharingBucket = false;

	/* What follows goes in front of everything the shared bucket got. */
	if (ui->sharedBucketUsed && ui->uiZ)
		ui->uiZ--;
}

static void drawChrome(UIContext *ui) {
	uiFillRect(ui, 0, 0, (int) ui->width, TINROM_HEADER_H, TINROM_COLOR_HEADER);
	uiText(ui, TINROM_MARGIN_X, 6, ui->title);

	int footerY = (int) ui->height - TINROM_FOOTER_H;

	uiFillRect(
		ui, 0, footerY, (int) ui->width, TINROM_FOOTER_H, TINROM_COLOR_PANEL
	);
	uiText(ui, TINROM_MARGIN_X, footerY + 3, ui->hint);
}

/* What the model, or a background in its place, may take of the buffer. */
static unsigned int backgroundBudget(const UIContext *ui) {
	unsigned int used = (unsigned int)
		(ui->chain->nextPacket - ui->chain->data);
	unsigned int spare = (used < GPU_CHAIN_BUFFER_SIZE)
		? (GPU_CHAIN_BUFFER_SIZE - used - MODEL_TAIL_MARGIN)
		: 0;

	return (spare > MODEL_WORD_BUDGET) ? MODEL_WORD_BUDGET : spare;
}

/*
 * The model is drawn LAST, not first, even though it ends up behind
 * everything. The ordering table decides what is in front; the packet buffer
 * decides what gets drawn at all, and it is first come first served.
 *
 * Drawing the model first meant it took its slice of the buffer - 2048
 * words then - before the menu had asked for anything, and the busiest
 * screen - the main menu, at about 1500 words by uiTextCost's own
 * reckoning - could then lose whole rows of text. A missing menu entry is worse than a missing polygon,
 * so the model now takes what is left instead of what it would like.
 */
static void drawModel(UIContext *ui) {
#if !TINROM_WITH_MODEL
	(void) ui;
#else
	if (!ui->modelVisible)
		return;

	ui->modelAngleY += MODEL_SPIN_SPEED;
	ui->modelAngleY &= 4095;

	unsigned int spare = backgroundBudget(ui);
	unsigned int drawn = tinromModelDraw(
		ui->chain, &ui->model,
		ui->modelAngleX, ui->modelAngleY, ui->modelAngleZ,
		dx(MODEL_SCREEN_X),
		(int) (ui->height / 2) + MODEL_SCREEN_Y_BIAS,
		MODEL_DISTANCE, UI_Z_MODEL_NEAR, UI_Z_MODEL_FAR,
		spare
	);

	/* Once, on the first frame that draws it: if this says 0 the model is
	 * being culled rather than merely invisible, which is a much shorter
	 * hunt. Readable on the console from About -> Boot log. */
	if (!ui->modelReported) {
		ui->modelReported = true;

		TINROM_LOG("model: %d/%d faces drawn, %u words spare, radius %d",
			(int) drawn, (int) ui->model.faceCount, spare,
			ui->model.radius);
	}
#endif
}

void uiEndFrame(UIContext *ui) {
	if (ui->chromeVisible)
		drawChrome(ui);

	if (ui->background)
		ui->background(ui, backgroundBudget(ui), ui->backgroundUser);
	else
		drawModel(ui);

	GPUOrderedDMAChain *chain = ui->chain;

	/*
	 * Draw into the buffer that is not on screen, wait for the GPU to finish,
	 * and only then show it - at the start of a vertical blank, so the
	 * picture never changes part way down.
	 *
	 * This used to show the buffer first and then draw into it. Most screens
	 * are drawn before the picture gets far enough down to catch it, and the
	 * emulator draws instantly, but a busy one is not: on a console the hex
	 * editor - a full-screen clear and a screen of textured text, after a
	 * large texture upload - showed a band of half-drawn frame rolling down
	 * the screen.
	 *
	 * Waiting for the GPU here, rather than letting it draw while the next
	 * frame is composed, costs a menu nothing it would notice; and a screen
	 * that draws one frame and then blocks - "Loading..." - is seen at once,
	 * as it would not be if each frame were shown a frame late.
	 */
	sendGPULinkedList(&(chain->orderingTable)[GPU_ORDERING_TABLE_SIZE - 1]);
	waitForGPUDMADone();

	while (!(GPU_STAT & GPU_STAT_IDLE))
		__asm__ volatile("");

	tinromWaitNextVSync();

	GPU_GP1 = gp1_fbOffset(ui->bufferX, ui->bufferY);
	tinromVideoReveal();
}

void uiFillRect(
	UIContext *ui, int x, int y, int width, int height, uint32_t color
) {
	if (!uiHasRoom(ui, 4))
		return;

	uint32_t *ptr = allocateOrderedGP0Packet(ui->chain, nextUIBucket(ui), 3);

	ptr[0] = gp0_rgb(
		TINROM_RGB_R(color), TINROM_RGB_G(color), TINROM_RGB_B(color)
	) | gp0_rectangle(false, true, false);
	ptr[1] = gp0_xy(dx(x), y);
	ptr[2] = gp0_xy(dx(width), height);
}

void uiVeilRect(
	UIContext *ui, int x, int y, int width, int height, uint32_t color
) {
	if (!uiHasRoom(ui, 5))
		return;

	uint32_t *ptr = allocateOrderedGP0Packet(ui->chain, nextUIBucket(ui), 4);

	/*
	 * Semi-transparency for an untextured primitive takes its mode from the
	 * draw-mode register rather than from the primitive, so the packet sets
	 * it itself. Everything textured in tinrom - icons, glyphs and
	 * the text canvas - sets its own page before drawing, so
	 * leaving this mode behind disturbs nothing.
	 */
	ptr[0] = gp0_setPage(
		gp0_page(0, 0, GP0_BLEND_SEMITRANS, GP0_COLOR_4BPP), false, false
	);
	ptr[1] = gp0_rgb(
		TINROM_RGB_R(color), TINROM_RGB_G(color), TINROM_RGB_B(color)
	) | gp0_rectangle(false, true, true);
	ptr[2] = gp0_xy(dx(x), y);
	ptr[3] = gp0_xy(dx(width), height);
}

/* ------------------------------------------------------------------------ */

/*
 * A bitmap with one texel per layout unit across - the terminal's canvas -
 * drawn at the display's two pixels a unit. A sprite cannot scale, so it is
 * a textured quad, which can: `texels` wide from (u, v) in the sheet, onto
 * twice that many pixels from display x `x`. Raw texture, no blending.
 *
 * The right and bottom edges are exclusive for the GPU, in pixels and in
 * texels alike, so u + texels names the texel just past the last one drawn
 * and has to fit in u's eight bits: u + texels <= 255.
 */
#define QUAD_WORDS 9

static void stretchedQuad(
	uint32_t *ptr, int x, int y, unsigned int texels, unsigned int rows,
	unsigned int u, unsigned int v, uint16_t clut, uint16_t page
) {
	int right  = x + (int) (texels * TINROM_SCREEN_SCALE_X);
	int bottom = y + (int) rows;

	ptr[0] = gp0_rgb(128, 128, 128) | gp0_quad(true, false);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_uv(u, v, clut);
	ptr[3] = gp0_xy(right, y);
	ptr[4] = gp0_uv(u + texels, v, page);
	ptr[5] = gp0_xy(x, bottom);
	ptr[6] = gp0_uv(u, v + rows, 0);
	ptr[7] = gp0_xy(right, bottom);
	ptr[8] = gp0_uv(u + texels, v + rows, 0);
}

static unsigned int iconVramX(unsigned int slot) {
	unsigned int inRow = slot % ICON_PER_ROW;

	return ICON_VRAM_X
		+ ((inRow / ICON_PER_PAGE) * ICON_PAGE_WIDTH)
		+ ((inRow % ICON_PER_PAGE) * ICON_HALFWORDS);
}

static unsigned int iconVramY(unsigned int slot) {
	return ICON_VRAM_Y + (slot / ICON_PER_ROW) * UI_ICON_SIZE;
}

void uiIconsClear(UIContext *ui) {
	for (unsigned int i = 0; i < UI_ICON_SLOTS; i++)
		ui->icons[i].frames = 0;
}

void uiIconForget(UIContext *ui, unsigned int slot) {
	if (slot < UI_ICON_SLOTS)
		ui->icons[slot].frames = 0;
}

bool uiIconUpload(
	UIContext      *ui,
	unsigned int    slot,
	const uint8_t  *sheet,
	const uint16_t *palette,
	unsigned int    frames
) {
	if ((slot >= UI_ICON_SLOTS) || !sheet || !palette || !frames)
		return false;

	if (frames > UI_ICON_MAX_FRAMES)
		frames = UI_ICON_MAX_FRAMES;

	/*
	 * Each texel twice: a 4bpp byte holds two, the left one in the low
	 * nibble, so each byte of the card's becomes two, one per texel.
	 */
	static uint8_t doubled[UI_ICON_SIZE * ICON_SHEET_WIDTH / 2]
		__attribute__((aligned(4)));

	for (unsigned int i = 0; i < (sizeof(doubled) / 2); i++) {
		uint8_t left  = sheet[i] & 15;
		uint8_t right = sheet[i] >> 4;

		doubled[i * 2]     = (uint8_t) (left | (left << 4));
		doubled[i * 2 + 1] = (uint8_t) (right | (right << 4));
	}

	uploadIndexedTexture(
		&ui->icons[slot].texture,
		doubled,
		palette,
		iconVramX(slot),
		iconVramY(slot),
		ICON_CLUT_X,
		ICON_CLUT_Y + slot,
		ICON_SHEET_WIDTH,
		UI_ICON_SIZE,
		GP0_COLOR_4BPP
	);

	ui->icons[slot].frames = (uint8_t) frames;

	return true;
}

bool uiIconPresent(const UIContext *ui, unsigned int slot) {
	return (slot < UI_ICON_SLOTS) && (ui->icons[slot].frames != 0);
}

/* A texture page command and a sprite. */
#define ICON_WORDS 5

void uiIconDraw(UIContext *ui, unsigned int slot, int x, int y) {
	if (!uiIconPresent(ui, slot))
		return;

	/* One tagged packet: the strip's texture page, then the sprite. */
	if (!uiHasRoom(ui, ICON_WORDS + 1))
		return;

	const UIIcon *icon  = &ui->icons[slot];
	unsigned int  frame = 0;

	if (icon->frames > 1) {
		unsigned int period = (icon->frames == 2)
			? ICON_PERIOD_2
			: ICON_PERIOD_3;

		frame = (tinromFrameCounter() / period) % icon->frames;
	}

	uint32_t *ptr = allocateOrderedGP0Packet(
		ui->chain, nextUIBucket(ui), ICON_WORDS
	);

	/*
	 * Raw texture, semi-transparency off. The card's palette is written for
	 * exactly this: 0000h reads as transparent and 8000h as solid black. A
	 * strip starts at most 40 halfwords into its page (see ICON_PER_PAGE),
	 * so the last frame ends at u = 255 at most.
	 */
	ptr[0] = gp0_setPage(icon->texture.page, false, false);
	ptr[1] = gp0_rectangle(true, true, false);
	ptr[2] = gp0_xy(dx(x), y);
	ptr[3] = gp0_uv(icon->texture.u + frame * UI_ICON_SIZE * 2,
		icon->texture.v, icon->texture.clut);
	ptr[4] = gp0_xy(UI_ICON_SIZE * 2, UI_ICON_SIZE);
}

void uiCanvasDraw(
	UIContext *ui, const UICanvas *canvas, int x, int y,
	int width, unsigned int rows
) {
	if (rows > canvas->rows)
		rows = canvas->rows;

	uiBitmapDraw(ui, canvas->clut, x, y, width,
		(int) (rows * UI_CANVAS_LINE_H), false);
}

/* An 8bpp texture page is 64 words: 128 texels. */
#define PAGE_TEXELS 128

static uint16_t canvasPage(unsigned int texel) {
	return gp0_page(
		(UI_CANVAS_VRAM_X + (texel / 2)) / 64, UI_CANVAS_VRAM_Y / 256,
		GP0_BLEND_SEMITRANS, GP0_COLOR_8BPP
	);
}

void uiBitmapDraw(
	UIContext *ui, uint16_t clut, int x, int y, int width, int height,
	bool stretched
) {
	if (width > UI_CANVAS_WIDTH)
		width = UI_CANVAS_WIDTH;
	if ((width <= 0) || (height <= 0) || !uiHasRoom(ui, 21))
		return;

	unsigned int z      = nextUIBucket(ui);
	unsigned int texels = (unsigned int) width
		* (stretched ? 1u : UI_CANVAS_SCALE_X);
	unsigned int v      = UI_CANVAS_VRAM_Y % 256;

	if (stretched) {
		/*
		 * Quads, as long as u allows: each starts on its own page and runs
		 * to u = 255 at most. 320 texels is two of them.
		 */
		for (unsigned int left = 0; left < texels;) {
			unsigned int base  = left & ~(PAGE_TEXELS - 1u);
			unsigned int u     = left - base;
			unsigned int slice = texels - left;

			if (slice > (255 - u))
				slice = 255 - u;

			uint32_t *ptr = allocateOrderedGP0Packet(ui->chain, z, QUAD_WORDS);

			stretchedQuad(ptr, dx(x) + (int) (left * TINROM_SCREEN_SCALE_X),
				y, slice, (unsigned int) height, u, v, clut,
				canvasPage(base));
			left += slice;
		}

		return;
	}

	/*
	 * One texel to a pixel: sprites. A sprite's u is eight bits, so the 640
	 * texels of a full-width canvas are three of them, each from its own
	 * texture page. One bucket holds them all: it is LIFO, so allocating
	 * sprite then page, each time, sends each page command just ahead of
	 * its own sprite.
	 */
	for (unsigned int left = 0; left < texels; left += 256) {
		unsigned int slice = ((texels - left) > 256) ? 256 : (texels - left);
		uint32_t    *ptr   = allocateOrderedGP0Packet(ui->chain, z, 4);

		/* Raw texture, no blending: the transparent texel stays transparent. */
		ptr[0] = gp0_rectangle(true, true, false);
		ptr[1] = gp0_xy(dx(x) + (int) left, y);
		ptr[2] = gp0_uv(0, v, clut);
		ptr[3] = gp0_xy((int) slice, height);

		ptr    = allocateOrderedGP0Packet(ui->chain, z, 1);
		ptr[0] = gp0_setPage(canvasPage(left), false, false);
	}
}

/* ------------------------------------------------------------------------ */

/*
 * The GPU multiplies a texel by the primitive's colour and divides by 128.
 * The font's brightest texel is 31 of 31, so to make that come out as
 * `channel` the factor is channel * 128 / 248 - 0x80 for 0xf8, and white
 * slightly over, which the GPU clamps.
 */
static uint8_t tintLevel(uint8_t channel) {
	unsigned int level = ((unsigned int) channel * 128) / 248;

	return (uint8_t) ((level > 255) ? 255 : level);
}

/* Glyph packets for one string, filled a node at a time. */
typedef struct {
	UIContext   *ui;
	unsigned int z;
	uint32_t     command;
	unsigned int remaining;
	unsigned int slots;
	uint32_t    *ptr;
} GlyphWriter;

static void emitGlyph(GlyphWriter *out, int x, int y, const UIFontGlyph *g) {
	if (!out->remaining)
		return;

	if (!out->slots) {
		out->slots = (out->remaining < GLYPHS_PER_PACKET)
			? out->remaining : GLYPHS_PER_PACKET;
		out->ptr   = allocateOrderedGP0Packet(
			out->ui->chain, out->z, out->slots * 4
		);
	}

	const TextureInfo *font = &out->ui->font;

	out->ptr[0] = out->command;
	out->ptr[1] = gp0_xy(x, y);
	out->ptr[2] = gp0_uv(font->u + g->u, font->v + g->v, font->clut);
	out->ptr[3] = gp0_xy(g->width, UI_FONT_HEIGHT);
	out->ptr   += 4;
	out->slots--;
	out->remaining--;
}

/*
 * Walks the glyphs a string draws: wrapped at `wrap` units if that is above
 * zero, or only its first line, cut at `clip` units, if that is. Counts
 * them, and emits them too if `out` is given. `*lines` gets the line count.
 *
 * The pen moves in texels, which are display pixels, so a glyph lands on
 * the column the font was drawn for rather than the nearest even one.
 */
static unsigned int walkText(
	const char *text, int x, int y, int wrap, int clip, GlyphWriter *out,
	unsigned int *lines
) {
	unsigned int glyphs = 0;
	int          left   = dx(x);

	clip   *= UI_FONT_SCALE_X;
	*lines  = 0;

	while (text) {
		size_t      length;
		int         width;
		const char *line = text;

		text = uiFontNextLine(text, wrap, &length, &width);
		(*lines)++;

		int pen = 0;

		for (size_t i = 0; i < length; i++) {
			char c = line[i];

			if (c == '\t') {
				int tab = UI_FONT_TAB_WIDTH * UI_FONT_SCALE_X;

				pen = ((pen / tab) + 1) * tab;
				continue;
			}

			const UIFontGlyph *glyph = uiFontGlyph(c);

			if ((clip > 0) && ((pen + glyph->width) > clip))
				break;

			if (glyph->width) {
				if (out)
					emitGlyph(out, left + pen, y, glyph);

				glyphs++;
			}

			pen += glyph->advance;
		}

		if (clip > 0)
			break;

		y += UI_FONT_LINE_HEIGHT;
	}

	return glyphs;
}

static unsigned int drawText(
	UIContext *ui, int x, int y, int wrap, int clip, uint32_t color,
	const char *text
) {
	unsigned int lines;
	unsigned int glyphs = walkText(text, x, y, wrap, clip, NULL, &lines);

	if (!glyphs || !uiHasRoom(ui, glyphsCost(glyphs)))
		return lines;

	GlyphWriter out = {
		.ui        = ui,
		.z         = nextUIBucket(ui),
		/* Textured, modulated by the colour, not blended. */
		.command   = gp0_rgb(
			tintLevel(TINROM_RGB_R(color)),
			tintLevel(TINROM_RGB_G(color)),
			tintLevel(TINROM_RGB_B(color))
		) | gp0_rectangle(true, false, false),
		.remaining = glyphs,
		.slots     = 0,
		.ptr       = NULL
	};

	walkText(text, x, y, wrap, clip, &out, &lines);

	/*
	 * LIFO within a bucket, so the page command is allocated last. Glyphs
	 * overlapping by their shared outline column do not care which of them
	 * lands on top.
	 */
	uint32_t *ptr = allocateOrderedGP0Packet(ui->chain, out.z, 1);

	ptr[0] = gp0_setPage(ui->font.page, false, false);

	return lines;
}

void uiText(UIContext *ui, int x, int y, const char *text) {
	drawText(ui, x, y, 0, 0, TINROM_COLOR_TEXT, text);
}

void uiTextColored(
	UIContext *ui, int x, int y, uint32_t color, const char *text
) {
	drawText(ui, x, y, 0, 0, color, text);
}

void uiTextf(UIContext *ui, int x, int y, const char *format, ...) {
	char    buffer[96];
	va_list args;

	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	uiText(ui, x, y, buffer);
}

unsigned int uiTextWrapped(
	UIContext *ui, int x, int y, int width, uint32_t color, const char *text
) {
	/* At least one pixel: a width of 0 would mean "do not wrap". */
	return drawText(ui, x, y, (width > 0) ? width : 1, 0, color, text);
}

unsigned int uiTextLines(const char *text, int width) {
	return uiFontLineCount(text, (width > 0) ? width : 1);
}

void uiTextClipped(
	UIContext *ui, int x, int y, int width, uint32_t color, const char *text
) {
	if (width > 0)
		drawText(ui, x, y, 0, width, color, text);
}

int uiTextWidth(const char *text) {
	return uiFontTextWidth(text);
}
