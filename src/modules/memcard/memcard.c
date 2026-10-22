/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Memory card management.
 *
 * The grid is the screen: selecting "Memory cards" goes straight to it. There
 * is no list of actions in front, because every action needs a subject - a
 * save, or a card - and picking that first is the natural order.
 *
 *   Cross on a save        -> what can be done to that save
 *   Cross on a pane's "..." -> what can be done to that card
 *
 * hal/mcard.c probes both slots and parses their directories, so what the
 * grid shows is what is on them. Behind it, a save can be copied, deleted and
 * undeleted; of the card's actions only its details are real so far.
 */

#include "modules/modules.h"
#include "ui/grid.h"

#define CARD_PORTS 2

/*
 * A card's fifteen directory slots map one-to-one onto the fifteen cells of a
 * grid pane, which is why the grid is 3x5.
 */
typedef struct {
	TinromMcInfo info;
	TinromMcSlot slots[TINROM_MC_SAVE_SLOTS];
	UIGridCell   cells[UI_GRID_CELLS];
	char         labels[UI_GRID_CELLS][6];
	char         status[40];
	TinromResult result;

	/* What the last poll saw, so the next one can tell it apart. */
	bool         lastPresent;
	uint8_t      lastFlag;
} CardView;

/*
 * How often the browser re-checks the slots, in frames. A probe is four byte
 * exchanges and costs well under a millisecond with a card in; an empty slot
 * costs the /ACK timeout, so about two milliseconds for a pair of empty
 * slots. Four times a second is far more often than anyone can swap a card
 * and is invisible against a 16 ms frame.
 */
#define POLL_INTERVAL_FRAMES 15
/* Consecutive polls that must disagree with the known state before acting. */
#define POLL_AGREE_COUNT 2

static CardView    cards[CARD_PORTS];
static UIGridPane  panes[CARD_PORTS];
static char        paneTitles[CARD_PORTS][16];

/*
 * The cursor lives at file scope rather than inside the screen loop: the popup
 * menus repaint the grid behind themselves and need to know where it points.
 */
static UIGridCursor cursor;

/* One at a time: an icon goes straight to VRAM and is not kept in RAM. */
static TinromMcIcon staging;

/*
 * Icon handles are allocated statically, fifteen per port, so a handle always
 * belongs to the same slot on the same card.
 */
static unsigned int iconHandle(unsigned int port, unsigned int cell) {
	return (port * UI_GRID_CELLS) + cell;
}

/*
 * Reading both cards takes a second or so, so say what is happening rather
 * than going black. Naming the step is also the cheapest instrument there is:
 * if the read ever wedges, the screen says which stage it wedged in without
 * needing a serial cable attached.
 */
static void progress(TinromContext *ctx, unsigned int port, const char *step) {
	char line[40];

	snprintf(line, sizeof(line), "Reading card %u: %s", port + 1, step);

	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Memory cards", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, line);
	uiEndFrame(&ctx->ui);
}

/* Four characters of the save's title, enough to tell saves apart at a glance. */
static void shortLabel(char *out, size_t size, const TinromMcSlot *slot) {
	size_t i = 0;

	for (; (i + 1) < size; i++) {
		char ch = slot->title[i];

		/* The raw header is Shift-JIS; anything outside plain ASCII is not
		 * ours to interpret yet, so it becomes a dot rather than rubbish. */
		if (!ch)
			break;
		if ((ch < 0x20) || (ch > 0x7e))
			ch = '.';

		out[i] = ch;
	}

	out[i] = '\0';
}

/*
 * Card change detection.
 *
 * Presence is the signal that matters: a PS1 slot has to be emptied before it
 * can be refilled, so a swap is always a removal followed by an insertion and
 * polling four times a second sees the gap. The FLAG byte is checked too,
 * since psx-spx says it returns to 08h when a card is re-inserted, but on its
 * own it cannot tell two never-written cards apart - bit 3 is only cleared by
 * writing, and tinrom writes only when asked to. The dummy write to sector
 * 003Fh that games use would make FLAG a swap sense in its own right.
 */
static void rememberCard(unsigned int port) {
	CardView *card = &cards[port];

	card->lastPresent = (card->result == TINROM_OK) || card->info.present;
	card->lastFlag    = card->info.flag;
}

static void buildPane(TinromContext *ctx, unsigned int port) {
	CardView *card = &cards[port];

	snprintf(paneTitles[port], sizeof(paneTitles[port]), "Card %u", port + 1);

	panes[port].title             = paneTitles[port];
	panes[port].cells             = card->cells;
	panes[port].available         = false;
	panes[port].unavailableReason = card->status;
	panes[port].menuLabel         = "...";

	for (unsigned int i = 0; i < UI_GRID_CELLS; i++) {
		card->cells[i].kind  = UI_GRID_EMPTY;
		card->cells[i].label = NULL;
		card->cells[i].icon  = UI_GRID_NO_ICON;
		card->cells[i].faded = false;
	}

	progress(ctx, port, "probing");

	card->result = tinromMcProbe(port, &card->info);

	if (card->result != TINROM_OK) {
		snprintf(card->status, sizeof(card->status), "%s",
			(card->result == TINROM_ERR_NOT_PRESENT)
				? "no card"
				: tinromResultString(card->result));
		rememberCard(port);

		return;
	}
	if (!card->info.present) {
		snprintf(card->status, sizeof(card->status), "no card");
		rememberCard(port);

		return;
	}

	progress(ctx, port, "directory");

	card->result = tinromMcReadDirectory(port, card->slots);

	if (card->result != TINROM_OK) {
		snprintf(card->status, sizeof(card->status), "%s",
			tinromResultString(card->result));
		rememberCard(port);

		return;
	}

	for (unsigned int i = 0; i < UI_GRID_CELLS; i++) {
		const TinromMcSlot *slot = &card->slots[i];

		card->labels[i][0]  = '\0';
		card->cells[i].icon = UI_GRID_NO_ICON;
		/* Every block of a deleted save, so it fades as one entry. */
		card->cells[i].faded = slot->deleted;

		switch (slot->state) {
			case TINROM_MC_SLOT_USED:
				card->cells[i].kind = UI_GRID_ITEM;
				shortLabel(card->labels[i], sizeof(card->labels[i]), slot);
				break;

			case TINROM_MC_SLOT_LINK:
				card->cells[i].kind = UI_GRID_CONTINUED;
				break;

			case TINROM_MC_SLOT_DELETED:
				card->cells[i].kind = UI_GRID_DELETED;
				shortLabel(card->labels[i], sizeof(card->labels[i]), slot);
				break;

			default:
				card->cells[i].kind = UI_GRID_EMPTY;
				break;
		}

		card->cells[i].label =
			card->labels[i][0] ? card->labels[i] : NULL;

		/*
		 * Only a file's first block has a title frame, so continuations get
		 * no icon of their own - they are drawn joined to the cell that does.
		 * A save with no usable icon keeps its text label.
		 */
		if ((slot->state != TINROM_MC_SLOT_USED)
			&& (slot->state != TINROM_MC_SLOT_DELETED))
			continue;

		char step[16];

		snprintf(step, sizeof(step), "icon %u/%u", i + 1,
			(unsigned int) UI_GRID_CELLS);
		progress(ctx, port, step);

		if (tinromMcReadIcon(port, i, &staging) != TINROM_OK)
			continue;

		unsigned int handle = iconHandle(port, i);

		if (uiIconUpload(
			&ctx->ui, handle, staging.pixels, staging.palette, staging.frames
		))
			card->cells[i].icon = (int) handle;
	}

	panes[port].available = true;

	unsigned int used = 0;

	for (unsigned int i = 0; i < UI_GRID_CELLS; i++) {
		if (card->cells[i].kind != UI_GRID_EMPTY)
			used++;
	}

	/*
	 * The free counterfeit test: ask for a sector the card cannot have and
	 * see whether it refuses or wraps. It only catches clones that alias at
	 * the top of the address space, which is why the deep sweep still exists
	 * under the card menu, but it costs nothing and runs every time.
	 */
	bool aliases = false;

	if (tinromMcCheckAliasing(port, &aliases) == TINROM_OK)
		card->info.suspectCapacity = aliases;

	snprintf(card->status, sizeof(card->status), "%u/%u blocks used%s",
		used, (unsigned int) UI_GRID_CELLS,
		card->info.suspectCapacity ? "  - capacity suspect" : "");

	rememberCard(port);
}

/*
 * Runs once per frame from the grid. Cheap unless something actually changed,
 * in which case it rebuilds that card's pane - which is slow, draws its own
 * progress frames, and is exactly what the grid's poll hook is there for.
 */
/* Poll state. Reset on entry so a previous visit cannot leak into this one. */
static uint32_t lastPoll;
static uint8_t  disagreed[CARD_PORTS];

static void pollReset(void) {
	lastPoll = tinromFrameCounter();

	for (unsigned int port = 0; port < CARD_PORTS; port++)
		disagreed[port] = 0;
}

static bool pollCards(void *user) {
	TinromContext *ctx = (TinromContext *) user;

	uint32_t now = tinromFrameCounter();

	if ((now - lastPoll) < POLL_INTERVAL_FRAMES)
		return false;

	lastPoll = now;

	bool changed = false;

	for (unsigned int port = 0; port < CARD_PORTS; port++) {
		CardView    *card = &cards[port];
		TinromMcInfo info;

		bool present = (tinromMcProbe(port, &info) == TINROM_OK);
		bool same    = (present == card->lastPresent)
			&& (!present || (info.flag == card->lastFlag));

		if (same) {
			disagreed[port] = 0;

			continue;
		}

		/*
		 * Plugging anything into either port drags the shared data line low
		 * for a moment - psx-spx warns that connecting a pad in slot 1 can
		 * corrupt a card access in slot 2 - so a single odd reading is as
		 * likely to be a connector still making contact as a real change.
		 * Wait for two polls in a row to disagree with what we last knew.
		 */
		if (++disagreed[port] < POLL_AGREE_COUNT)
			continue;

		disagreed[port] = 0;

		/* Whatever was on this card is gone; do not draw its icons again. */
		for (unsigned int i = 0; i < UI_GRID_CELLS; i++)
			uiIconForget(&ctx->ui, iconHandle(port, i));

		/* buildPane re-probes, re-reads and records the new signature. */
		buildPane(ctx, port);

		changed = true;
	}

	return changed;
}

static const char *describeCell(
	const UIGridPane *pane, unsigned int cell, void *user
) {
	static char detail[64];

	(void) user;

	unsigned int port = (pane == &panes[1]) ? 1 : 0;
	CardView    *card = &cards[port];

	/* On the "..." button, describe the card rather than any one save. */
	if (cell >= UI_GRID_CELLS) {
		snprintf(detail, sizeof(detail), "Card %u: %s", port + 1,
			card->status);

		return detail;
	}

	if (!pane->available)
		return pane->unavailableReason;

	/* Follow a continuation back to the save it belongs to. */
	while (cell && (card->slots[cell].state == TINROM_MC_SLOT_LINK))
		cell--;

	const TinromMcSlot *slot = &card->slots[cell];

	switch (slot->state) {
		case TINROM_MC_SLOT_USED:
		case TINROM_MC_SLOT_DELETED:
			/*
			 * One line, and the title is the half worth reading. Fall back to
			 * the filename when a card has no title frame.
			 */
			snprintf(detail, sizeof(detail), "%s%s  %u block%s",
				slot->title[0] ? slot->title : slot->filename,
				(slot->state == TINROM_MC_SLOT_DELETED) ? "  (deleted)" : "",
				slot->blocks, (slot->blocks == 1) ? "" : "s");
			break;

		default:
			snprintf(detail, sizeof(detail), "block %u: free", cell + 1);
			break;
	}

	return detail;
}

/* Repainted under both popups, so the grid stays live behind them. */
static void drawGridBehind(void *user) {
	TinromContext *ctx = (TinromContext *) user;

	uiGridDraw(&ctx->ui, panes, CARD_PORTS, &cursor);
}

/* --- what can be done to one save ------------------------------------- */

/* --- the three things that write ---------------------------------------- */

static unsigned int otherPort(unsigned int port) {
	return (port + 1) % CARD_PORTS;
}

static unsigned int freeBlocks(const CardView *card) {
	unsigned int free = 0;

	/* The same blocks the driver's allocator will take: anything not part of
	 * a chain that is still in use, deleted chains included. */
	for (unsigned int i = 0; i < UI_GRID_CELLS; i++) {
		const TinromMcSlot *slot = &card->slots[i];

		if ((slot->state == TINROM_MC_SLOT_FREE) || slot->deleted)
			free++;
	}

	return free;
}

/*
 * A copy is 64 frames per block each way and takes a second or so per block,
 * which is long enough that a still screen looks like a hang. Every eighth
 * frame is often enough to look continuous and rare enough that the transfer
 * is not spending its time waiting for vsync.
 */
#define COPY_REDRAW_EVERY 8

static void copyProgress(void *user, unsigned int done, unsigned int total) {
	TinromContext *ctx = (TinromContext *) user;

	if ((done % COPY_REDRAW_EVERY) && (done != total))
		return;

	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Memory cards", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, "Copying save");
	uiProgressBar(
		&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 40, 200, done, total
	);
	uiEndFrame(&ctx->ui);
}

/* Both cards are re-read: a copy changes one, and a card may have been
 * swapped while a confirmation was on screen. */
static void refreshCards(TinromContext *ctx) {
	uiIconsClear(&ctx->ui);

	for (unsigned int port = 0; port < CARD_PORTS; port++)
		buildPane(ctx, port);

	pollReset();
}

static void doCopy(TinromContext *ctx, const TinromMcSlot *slot) {
	unsigned int    to    = otherPort(cursor.pane);
	const CardView *other = &cards[to];

	if (!other->info.present) {
		uiMessageBox(ctx, "Copy",
			"The other card is not there.\n\n"
			"tinrom copies between cards rather than within one: a card "
			"cannot hold two saves under the same filename, and that "
			"filename is how the game finds its save.");

		return;
	}

	unsigned int free = freeBlocks(other);

	if (free < slot->blocks) {
		char body[160];

		snprintf(body, sizeof(body),
			"This save needs %u block%s and card %u has %u free.\n\n"
			"Deleted saves there count as free space, and are only used "
			"once the untouched blocks run out.",
			slot->blocks, (slot->blocks == 1) ? "" : "s", to + 1, free);

		uiMessageBox(ctx, "Not enough room", body);

		return;
	}

	TinromResult result = tinromMcCopy(
		cursor.pane, cursor.cell, to, NULL, copyProgress, ctx
	);

	if (result == TINROM_ERR_BUSY) {
		char body[160];

		snprintf(body, sizeof(body),
			"Card %u already holds a save called\n\n  %s\n\n"
			"Delete that one first if you mean to replace it.",
			to + 1, slot->filename);

		uiMessageBox(ctx, "Already there", body);
	} else {
		uiShowResult(ctx, "Copy", result);
	}

	refreshCards(ctx);
}

static void doDelete(TinromContext *ctx, const TinromMcSlot *slot) {
	char body[224];

	snprintf(body, sizeof(body),
		"%s\n\n"
		"Frees %u block%s. The save's data is left alone, so Undelete "
		"brings it back until something writes over those blocks.",
		slot->title[0] ? slot->title : slot->filename,
		slot->blocks, (slot->blocks == 1) ? "" : "s");

	if (!uiConfirm(ctx, "Delete save", body, true))
		return;

	uiShowResult(ctx, "Delete", tinromMcDelete(cursor.pane, cursor.cell));
	refreshCards(ctx);
}

static void doUndelete(TinromContext *ctx) {
	TinromResult result = tinromMcUndelete(cursor.pane, cursor.cell);

	if (result == TINROM_ERR_BUSY)
		uiMessageBox(ctx, "Cannot undelete",
			"Another save has taken one of the blocks this one used, so it "
			"cannot be put back whole.\n\nNothing was changed.");
	else
		uiShowResult(ctx, "Undelete", result);

	refreshCards(ctx);
}

static void saveMenu(TinromContext *ctx) {
	CardView           *card = &cards[cursor.pane];
	const TinromMcSlot *slot = &card->slots[cursor.cell];
	bool                gone = (slot->state == TINROM_MC_SLOT_DELETED);

	/* The grid moves the cursor to an entry's first cell, so anything else
	 * here is an empty block and has nothing to offer. */
	if ((slot->state != TINROM_MC_SLOT_USED) && !gone)
		return;

	UIMenuItem items[3];
	size_t     count = 0;

	bool other = cards[otherPort(cursor.pane)].info.present;

	items[count++] = (UIMenuItem) {
		"Copy",
		other ? "Into the other card's free blocks"
			: "The other card is not there",
		other, 0
	};

	if (gone)
		items[count++] = (UIMenuItem) {
			"Undelete", "Puts the block chain back in use", true, 1
		};
	else
		items[count++] = (UIMenuItem) {
			"Delete", "Marks the block chain free; recoverable", true, 2
		};

	items[count++] = (UIMenuItem) {
		"Details", "Filename, size and block chain", true, 3
	};

	/*
	 * Anchored under the save it belongs to, so it is obvious what the menu
	 * is about. uiPopupRun nudges it back on screen near the edges.
	 */
	int anchorX, anchorY;

	uiGridCellAnchor(cursor.pane, CARD_PORTS, cursor.cell, &anchorX, &anchorY);

	/* A 32-character save title will not fit; the grid already shows it. */
	char title[16];

	snprintf(title, sizeof(title), "Block %u", cursor.cell + 1);

	size_t choice = uiPopupRun(
		ctx, title, items, count, anchorX, anchorY + 2,
		drawGridBehind, ctx
	);

	if (choice == UI_MENU_CANCELLED)
		return;

	if (items[choice].tag == 3) {
		char body[192];

		snprintf(body, sizeof(body),
			"%s\n\n"
			"file\t%s\n"
			"size\t%u bytes\n"
			"blocks\t%u, starting at %u\n"
			"state\t%s",
			slot->title[0] ? slot->title : "(no title frame)",
			slot->filename, (unsigned int) slot->size,
			slot->blocks, cursor.cell + 1,
			gone ? "deleted" : "in use");

		uiMessageBox(ctx, "Save details", body);

		return;
	}

	switch (items[choice].tag) {
		case 0:
			doCopy(ctx, slot);
			break;

		case 1:
			doUndelete(ctx);
			break;

		default:
			doDelete(ctx, slot);
			break;
	}
}

/* --- what can be done to one card ------------------------------------- */

static void cardMenu(TinromContext *ctx) {
	CardView *card    = &cards[cursor.pane];
	bool      present = card->info.present;

	const UIMenuItem items[] = {
		{ "Back up to host",   "Sends a .mcr image over serial", present, 0 },
		{ "Restore from host", "Receives a .mcr image over serial", true, 1 },
		{ "Format",            "Writes a fresh, empty directory", present, 2 },
		{ "Check capacity",    "Writes and verifies every block - destructive",
			present, 3 },
		{ "Card details",      "Status byte and what the clone test saw",
			present, 4 }
	};

	int anchorX, anchorY;

	uiGridCellAnchor(
		cursor.pane, CARD_PORTS, UI_GRID_CELL_MENU, &anchorX, &anchorY
	);

	char title[16];

	snprintf(title, sizeof(title), "Card %u", cursor.pane + 1);

	size_t choice = uiPopupRun(
		ctx, title, items, sizeof(items) / sizeof(items[0]),
		anchorX, anchorY + 2, drawGridBehind, ctx
	);

	if (choice == UI_MENU_CANCELLED)
		return;

	if (items[choice].tag == 4) {
		char body[224];

		snprintf(body, sizeof(body),
			"Status:\t%02x%s%s\n"
			"Size:\t%u bytes reported\n"
			"Behaviour:\t%s\n\n"
			"(The status byte's bit 3 stays set until something writes to "
			"the card, so a fresh card reads 08h.)",
			(unsigned int) card->info.flag,
			(card->info.flag & 0x08) ? "  untouched since power-up" : "",
			(card->info.flag & 0x04) ? "  last write failed" : "",
			(unsigned int) card->info.detectedSize,
			card->info.suspectCapacity
				? "Possibly counterfeit!" //aliases out of range
				: "Behaves like genuine card.");

		uiMessageBox(ctx, "Card details", body);

		return;
	}

	uiNotImplemented(ctx, items[choice].label);
}

/* ---------------------------------------------------------------------- */

static TinromResult memcardEnter(TinromContext *ctx) {
	/* Handles are reused every visit, so drop last time's before reloading. */
	uiIconsClear(&ctx->ui);

	/* The cursor is deliberately not reset: coming back lands where you left. */
	for (unsigned int port = 0; port < CARD_PORTS; port++)
		buildPane(ctx, port);

	pollReset();

	for (;;) {
		UIGridOutcome outcome = uiGridRun(
			ctx, "Memory cards", panes, CARD_PORTS, &cursor,
			describeCell, pollCards, ctx
		);

		switch (outcome) {
			case UI_GRID_OUTCOME_SELECTED:
				saveMenu(ctx);
				break;

			case UI_GRID_OUTCOME_MENU:
				cardMenu(ctx);
				break;

			default:
				return TINROM_OK;
		}
	}
}

const TinromModule tinromModule_memcard = {
	.id      = "memcard",
	.name    = "Memory cards",
	.summary = "Copy, delete, undelete and image saves",
	.minTier = TINROM_TIER_BASIC,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = memcardEnter,
	.tick    = NULL,
	.shutdown = NULL
};
