/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Browse and launch: executables and data off a disc or a memory card.
 *
 * The disc listing is a view over iso9660OpenDir()/iso9660NextEntry(); the
 * card listing is a view over tinromMcReadDirectory(). Launching goes through
 * ui/launch.h, which is the same path the cdboot module boots a disc with -
 * there is one loader, not two.
 *
 * Writing to a card belongs to the memcard module, so the card view here is
 * deliberately read-only. Copying OUT, to a host over PCDRV, is the one thing
 * this module does that the others do not.
 */

#include "core/iso9660.h"
#include "core/pcdrv.h"
#include "core/psexe.h"
#include "hal/cdrom.h"
#include "hal/mcard.h"
#include "libc/string.h"
#include "modules/modules.h"
#include "ui/launch.h"

/*
 * A PS1 disc directory rarely runs past a few dozen entries. Anything beyond
 * this is reported rather than silently dropped, because a listing that
 * quietly stops is worse than one that admits it stopped.
 */
#define FB_MAX_ENTRIES 128
#define FB_MAX_DEPTH   8
#define FB_PATH_MAX    128
#define FB_NAME_MAX    (ISO_NAME_MAX + 8)

/* Four sectors at a time: big enough that the host is not called per sector,
 * small enough to sit in .bss without arguing about it. */
#define FB_COPY_SECTORS 4
#define FB_COPY_BYTES   (FB_COPY_SECTORS * ISO_SECTOR_SIZE)

static Iso9660     disc;
static bool        discMounted;

static Iso9660Dir  walker;
static UIMenuItem  items[FB_MAX_ENTRIES];
static char        labels[FB_MAX_ENTRIES][FB_NAME_MAX];
static char        details[FB_MAX_ENTRIES][28];
static uint32_t    entryLba[FB_MAX_ENTRIES];
static uint32_t    entrySize[FB_MAX_ENTRIES];
static bool        entryIsDir[FB_MAX_ENTRIES];
static size_t      entryCount;
static bool        entriesTruncated;

static char        path[FB_PATH_MAX];
static size_t      cursors[FB_MAX_DEPTH];
static unsigned int depth;

static uint8_t     copyBuffer[FB_COPY_BYTES];
static uint8_t     sectorBuffer[ISO_SECTOR_SIZE];

/* --- plumbing ----------------------------------------------------------- */

static TinromResult readSectors(
	uint32_t lba, void *buffer, size_t count, void *user
) {
	(void) user;

	return tinromCdReadSectors(lba, buffer, count);
}

/* ui/launch.h takes its reader user-first; iso9660.h takes it last. */
/*
 * Nothing to stop before handing over: every read ends with the drive
 * paused and both of Pause's answers collected (stopReading() in
 * hal/cdrom.c), spinning and idle - as the BIOS leaves it for a game. A Stop
 * here used to spin the disc down and hand over before Stop's second answer,
 * which a real drive sends only once the disc has stopped; it then landed in
 * the middle of the game's own CD start-up, which waited it out - the disc
 * stopped, then about 15 seconds before the game booted.
 */
static TinromResult launchReader(
	void *user, uint32_t sector, void *buffer, size_t count
) {
	(void) user;

	return tinromCdReadSectors(sector, buffer, count);
}

/* Which row the popups hang off, and what they repaint behind themselves. */
static size_t listSelected;

static void drawListBehind(void *user) {
	TinromContext *ctx = (TinromContext *) user;

	uiMenuDraw(&ctx->ui, items, entryCount, listSelected);
}

static void step(TinromContext *ctx, const char *title, const char *message) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, title, "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, message);
	uiEndFrame(&ctx->ui);
}

/** "1234", "12K", "3M" - the column is narrow and the exact byte count is in
 * the details screen for anyone who needs it. */
static void describeSize(char *out, size_t size, uint32_t bytes) {
	if (bytes < 10000u)
		snprintf(out, size, "%u bytes", (unsigned int) bytes);
	else if (bytes < (10u * 1024u * 1024u))
		snprintf(out, size, "%u KB", (unsigned int) (bytes / 1024u));
	else
		snprintf(out, size, "%u MB", (unsigned int) (bytes / (1024u * 1024u)));
}

/** Strips the ISO ";1" version suffix, which no host wants in a filename. */
static void hostName(char *out, size_t size, const char *name) {
	size_t i = 0;

	while (name[i] && (name[i] != ';') && ((i + 1) < size)) {
		out[i] = name[i];
		i++;
	}

	out[i] = '\0';

	if (!i)
		snprintf(out, size, "unnamed.bin");
}

/* --- copying out to a host ---------------------------------------------- */

/*
 * PCDRV can write, and a disc or a card is exactly the sort of thing worth
 * getting off the console and onto a PC. This is the only writer in the
 * module, and it only ever writes to the host.
 */
static void copyProgress(
	TinromContext *ctx, const char *name, uint32_t done, uint32_t total
) {
	char line[64];

	snprintf(line, sizeof(line), "Copying %s to the host", name);

	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Copy to host", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, line);
	uiProgressBar(
		&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 40, 200, done, total
	);
	uiEndFrame(&ctx->ui);
}

/**
 * Opens the host file and writes `total` bytes, pulling each chunk through
 * `fill`. Keeping the transfer here means the disc and the card only have to
 * know how to fill a buffer.
 */
static TinromResult copyToHost(
	TinromContext *ctx,
	const char    *name,
	uint32_t       total,
	TinromResult (*fill)(uint32_t offset, void *buffer, uint32_t length),
	void          *unused
) {
	(void) unused;

	char hostPath[TINROM_PCDRV_PATH_MAX];

	hostName(hostPath, sizeof(hostPath), name);

	int32_t      handle = -1;
	TinromResult result = tinromPcdrvCreate(hostPath, 0, &handle);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Creating the host file", result);

		return result;
	}

	uint32_t done = 0;

	while (done < total) {
		uint32_t chunk = total - done;

		if (chunk > FB_COPY_BYTES)
			chunk = FB_COPY_BYTES;

		result = fill(done, copyBuffer, chunk);

		if (result != TINROM_OK)
			break;

		uint32_t written = 0;

		result = tinromPcdrvWrite(handle, copyBuffer, chunk, &written);

		if (result != TINROM_OK)
			break;

		if (written != chunk) {
			result = TINROM_ERR_IO;

			break;
		}

		done += chunk;

		copyProgress(ctx, hostPath, done, total);
	}

	(void) tinromPcdrvClose(handle);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Copying to the host", result);

		return result;
	}

	char body[160];

	snprintf(body, sizeof(body),
		"Wrote %u bytes to\n\n  %s\n\n"
		"relative to whatever directory the host is serving.",
		(unsigned int) total, hostPath);
	uiMessageBox(ctx, "Copied", body);

	return TINROM_OK;
}

/* The file currently being copied off the disc, for the fill callback. */
static uint32_t copyLba;

static TinromResult fillFromDisc(
	uint32_t offset, void *buffer, uint32_t length
) {
	uint32_t sector = copyLba + (offset / ISO_SECTOR_SIZE);
	uint32_t whole  = length / ISO_SECTOR_SIZE;

	if (whole) {
		TinromResult result = tinromCdReadSectors(sector, buffer, whole);

		if (result != TINROM_OK)
			return result;
	}

	/* A file's last sector is usually partial; read it whole and keep the
	 * part that belongs to the file. */
	uint32_t tail = length - (whole * ISO_SECTOR_SIZE);

	if (tail) {
		TinromResult result =
			tinromCdReadSectors(sector + whole, sectorBuffer, 1);

		if (result != TINROM_OK)
			return result;

		memcpy((uint8_t *) buffer + (whole * ISO_SECTOR_SIZE),
			sectorBuffer, tail);
	}

	return TINROM_OK;
}

/* --- the disc ----------------------------------------------------------- */

static bool mountDisc(TinromContext *ctx) {
	if (discMounted)
		return true;

	step(ctx, "File browser", "Reading the filesystem...");

	TinromDiscInfo info;
	TinromResult   result = tinromCdIdentify(&info);

	if ((result != TINROM_OK) || (info.type == TINROM_DISC_NONE)) {
		uiMessageBox(ctx, "No disc",
			"Nothing readable in the drive.\n\n"
			"Check the disc is seated and the lid is closed.");

		return false;
	}

	if (info.type == TINROM_DISC_AUDIO) {
		uiMessageBox(ctx, "Audio disc",
			"That is an audio CD. It has no filesystem to browse.");

		return false;
	}

	result = iso9660Mount(&disc, readSectors, NULL);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Mounting the disc", result);

		return false;
	}

	discMounted = true;

	return true;
}

static TinromResult listDirectory(void) {
	entryCount       = 0;
	entriesTruncated = false;

	TinromResult result = iso9660OpenDir(&disc, path, &walker);

	if (result != TINROM_OK)
		return result;

	/* ".." first, and only below the root, so the list reads like a path. */
	if (depth) {
		snprintf(labels[entryCount], sizeof(labels[0]), "..");
		snprintf(details[entryCount], sizeof(details[0]), "up one level");

		entryIsDir[entryCount] = true;
		entryLba[entryCount]   = 0;
		entrySize[entryCount]  = 0;
		entryCount++;
	}

	for (;;) {
		Iso9660Entry entry;

		result = iso9660NextEntry(&walker, &entry);

		if (result == TINROM_ERR_NOT_PRESENT)
			break;
		if (result != TINROM_OK)
			return result;

		if (entryCount >= FB_MAX_ENTRIES) {
			entriesTruncated = true;

			break;
		}

		/* The "." and ".." records are the walker's business, not ours. */
		if (!entry.name[0] || !strcmp(entry.name, ".")
			|| !strcmp(entry.name, ".."))
			continue;

		snprintf(labels[entryCount], sizeof(labels[0]), "%s%s",
			entry.isDirectory ? "/" : " ", entry.name);

		if (entry.isDirectory)
			snprintf(details[entryCount], sizeof(details[0]), "directory");
		else
			describeSize(details[entryCount], sizeof(details[0]), entry.size);

		entryIsDir[entryCount] = entry.isDirectory;
		entryLba[entryCount]   = entry.lba;
		entrySize[entryCount]  = entry.size;
		entryCount++;
	}

	for (size_t i = 0; i < entryCount; i++) {
		items[i].label   = labels[i];
		items[i].summary = details[i];
		items[i].enabled = true;
		items[i].tag     = i;
	}

	return TINROM_OK;
}

/** Is the file at `lba` a PS-EXE? Costs one sector read. */
static bool readExeHeader(uint32_t lba, TinromExeHeader *header) {
	if (tinromCdReadSectors(lba, sectorBuffer, 1) != TINROM_OK)
		return false;

	return tinromExeParse(sectorBuffer, header) == TINROM_OK;
}

static void fileMenu(TinromContext *ctx, size_t index, int anchorX, int anchorY) {
	const char     *name = labels[index] + 1; /* past the type marker */
	TinromExeHeader header;
	bool            isExe = readExeHeader(entryLba[index], &header);
	bool            host  = tinromPcdrvHostPresent();

	UIMenuItem actions[3];

	actions[0] = (UIMenuItem) { "Details", "Size, LBA and header", true, 0 };
	actions[1] = (UIMenuItem) {
		"Launch",
		isExe ? "Hand the console to this executable"
			: "Not a PS-EXE",
		isExe, 1
	};
	actions[2] = (UIMenuItem) {
		"Copy to host",
		host ? "Write it out over PCDRV" : "No host is listening",
		host, 2
	};

	size_t choice = uiPopupRun(
		ctx, "File", actions, 3, anchorX, anchorY, drawListBehind, ctx
	);

	if (choice == UI_MENU_CANCELLED)
		return;

	if (choice == 0) {
		char body[288];
		char exeLines[128];

		if (isExe)
			snprintf(exeLines, sizeof(exeLines),
				"\n\nPS-EXE\n"
				"  entry\t%08x\n"
				"  loads\t%08x + %u bytes\n"
				"  stack\t%08x",
				(unsigned int) header.pc,
				(unsigned int) header.loadAddress,
				(unsigned int) header.loadSize,
				(unsigned int) (header.spBase + header.spOffset));
		else
			exeLines[0] = '\0';

		snprintf(body, sizeof(body),
			"%s\n\n"
			"size\t%u bytes\n"
			"sector\t%u%s",
			name, (unsigned int) entrySize[index],
			(unsigned int) entryLba[index], exeLines);

		uiMessageBox(ctx, "File details", body);

		return;
	}

	if (choice == 1) {
		uiLaunchExe(ctx, name, launchReader, NULL, entryLba[index], NULL);

		return;
	}

	copyLba = entryLba[index];

	(void) copyToHost(ctx, name, entrySize[index], fillFromDisc, NULL);
}

static void browseDisc(TinromContext *ctx) {
	if (!mountDisc(ctx))
		return;

	depth      = 0;
	path[0]    = '\0';
	cursors[0] = 0;

	for (;;) {
		TinromResult result = listDirectory();

		if (result != TINROM_OK) {
			uiShowResult(ctx, "Reading the directory", result);

			return;
		}

		if (!entryCount) {
			uiMessageBox(ctx, "Empty", "This directory has nothing in it.");

			if (!depth)
				return;
		}

		if (entriesTruncated) {
			char body[160];

			snprintf(body, sizeof(body),
				"This directory has more than %d entries.\n\n"
				"The first %d are listed; the rest are not shown.",
				FB_MAX_ENTRIES, FB_MAX_ENTRIES);
			uiMessageBox(ctx, "Long directory", body);
		}

		char title[64];

		snprintf(title, sizeof(title), "Disc: %s%s",
			disc.label[0] ? disc.label : "(no label)", path);

		size_t choice = uiMenuRunAt(
			ctx, title, items, entryCount, &cursors[depth]
		);

		bool goUp = (choice == UI_MENU_CANCELLED);

		if (!goUp && entryIsDir[choice] && !strcmp(labels[choice], ".."))
			goUp = true;

		if (goUp) {
			if (!depth)
				return;

			char *slash = strrchr(path, '\\');

			if (slash)
				*slash = '\0';

			depth--;

			continue;
		}

		if (entryIsDir[choice]) {
			if ((depth + 1) >= FB_MAX_DEPTH) {
				uiMessageBox(ctx, "Too deep",
					"tinrom stops descending here. ISO9660 itself only "
					"promises eight levels, and anything deeper is a disc "
					"doing something unusual.");

				continue;
			}

			size_t used = strlen(path);

			if ((used + strlen(labels[choice]) + 2) >= sizeof(path)) {
				uiMessageBox(ctx, "Path too long",
					"The path to this directory does not fit.");

				continue;
			}

			/* labels[] carries the type marker in its first byte. */
			snprintf(path + used, sizeof(path) - used, "\\%s",
				labels[choice] + 1);

			depth++;
			cursors[depth] = 0;

			continue;
		}

		int anchorX, anchorY;

		listSelected = cursors[depth];

		uiMenuRowAnchor(cursors[depth], &anchorX, &anchorY);
		fileMenu(ctx, choice, anchorX, anchorY);
	}
}

/* --- memory cards ------------------------------------------------------- */

static TinromMcSlot cardSlots[TINROM_MC_SAVE_SLOTS];
static uint8_t      cardChain[TINROM_MC_SAVE_SLOTS];
static unsigned int cardPort;

static TinromResult fillFromCard(
	uint32_t offset, void *buffer, uint32_t length
) {
	uint8_t *out = (uint8_t *) buffer;

	for (uint32_t done = 0; done < length; done += TINROM_MC_FRAME_SIZE) {
		uint32_t at    = offset + done;
		uint32_t block = at / TINROM_MC_BLOCK_SIZE;
		uint32_t frame = (at % TINROM_MC_BLOCK_SIZE) / TINROM_MC_FRAME_SIZE;

		TinromResult result = tinromMcReadFrame(
			cardPort,
			(uint16_t) (((cardChain[block] + 1)
				* (TINROM_MC_BLOCK_SIZE / TINROM_MC_FRAME_SIZE)) + frame),
			out + done
		);

		if (result != TINROM_OK)
			return result;
	}

	return TINROM_OK;
}

static void saveMenu(
	TinromContext *ctx, unsigned int port, size_t slot,
	int anchorX, int anchorY
) {
	const TinromMcSlot *save = &cardSlots[slot];
	bool                host = tinromPcdrvHostPresent();

	UIMenuItem actions[2];

	actions[0] = (UIMenuItem) { "Details", "Filename, size and chain", true, 0 };
	actions[1] = (UIMenuItem) {
		"Copy to host",
		host ? "Write the save's blocks out over PCDRV"
			: "No host is listening",
		host, 1
	};

	size_t choice = uiPopupRun(
		ctx, "Save", actions, 2, anchorX, anchorY, drawListBehind, ctx
	);

	if (choice == UI_MENU_CANCELLED)
		return;

	if (choice == 0) {
		char body[256];

		snprintf(body, sizeof(body),
			"%s\n\n"
			"file\t%s\n"
			"size\t%u bytes\n"
			"blocks\t%u, starting at %u\n"
			"state\t%s",
			save->title[0] ? save->title : "(no title frame)",
			save->filename, (unsigned int) save->size,
			save->blocks, (unsigned int) slot + 1,
			save->deleted ? "deleted" : "in use");
		uiMessageBox(ctx, "Save details", body);

		return;
	}

	unsigned int blocks = 0;

	TinromResult result =
		tinromMcSaveChain(port, slot, cardChain, &blocks);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Reading the block chain", result);

		return;
	}

	cardPort = port;

	(void) copyToHost(
		ctx, save->filename, blocks * TINROM_MC_BLOCK_SIZE, fillFromCard, NULL
	);
}

static void browseCard(TinromContext *ctx, unsigned int port) {
	char title[32];

	snprintf(title, sizeof(title), "Memory card %u", port + 1);

	size_t cursor = 0;

	for (;;) {
		step(ctx, title, "Reading the directory...");

		TinromMcInfo info;
		TinromResult result = tinromMcProbe(port, &info);

		if ((result != TINROM_OK) || !info.present) {
			uiMessageBox(ctx, title,
				"No card in that slot.\n\n"
				"Put one in and try again.");

			return;
		}

		result = tinromMcReadDirectory(port, cardSlots);

		if (result != TINROM_OK) {
			uiShowResult(ctx, "Reading the directory", result);

			return;
		}

		entryCount = 0;

		for (size_t i = 0; i < TINROM_MC_SAVE_SLOTS; i++) {
			const TinromMcSlot *save = &cardSlots[i];

			if ((save->state != TINROM_MC_SLOT_USED)
				&& (save->state != TINROM_MC_SLOT_DELETED))
				continue;

			snprintf(labels[entryCount], sizeof(labels[0]), "%s",
				save->title[0] ? save->title : save->filename);
			snprintf(details[entryCount], sizeof(details[0]),
				"%u block%s%s", save->blocks,
				(save->blocks == 1) ? "" : "s",
				save->deleted ? ", deleted" : "");

			items[entryCount].label   = labels[entryCount];
			items[entryCount].summary = details[entryCount];
			items[entryCount].enabled = true;
			items[entryCount].tag     = i;
			entryCount++;
		}

		if (!entryCount) {
			uiMessageBox(ctx, title, "This card has no saves on it.");

			return;
		}

		size_t choice = uiMenuRunAt(ctx, title, items, entryCount, &cursor);

		if (choice == UI_MENU_CANCELLED)
			return;

		int anchorX, anchorY;

		listSelected = cursor;

		uiMenuRowAnchor(cursor, &anchorX, &anchorY);
		saveMenu(ctx, port, items[choice].tag, anchorX, anchorY);
	}
}

/* ---------------------------------------------------------------------- */

static TinromResult fileBrowserEnter(TinromContext *ctx) {
	/* Named apart from the module's own `items`, which the listings use. */
	static const UIMenuItem sources[] = {
		{ "Browse the disc",      "ISO9660 listing of whatever is in the drive", true, 0 },
		{ "Browse memory card 1", "Saves, read only: write from Memory cards", true, 1 },
		{ "Browse memory card 2", "Saves, read only: write from Memory cards", true, 2 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "File browser", sources,
			sizeof(sources) / sizeof(sources[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (sources[choice].tag) {
			case 0:
				/* A disc may have been swapped since the last visit. */
				discMounted = false;
				browseDisc(ctx);
				break;

			default:
				browseCard(ctx, (unsigned int) sources[choice].tag - 1);
				break;
		}
	}
}

const TinromModule tinromModule_filebrowser = {
	.id      = "filebrowser",
	.name    = "File browser",
	.summary = "Browse a disc or card and run executables",
	.minTier = TINROM_TIER_ADVANCED,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = fileBrowserEnter,
	.tick    = NULL,
	.shutdown = NULL
};
