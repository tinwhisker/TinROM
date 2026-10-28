/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Booting a disc.
 *
 * Three things have to happen, and they are separable: persuade the drive to
 * read the disc at all, find the executable the disc names, and hand the
 * console over to it.
 *
 *   unlock   hal/cdrom.c, the documented 50h..56h sequence
 *   find     core/iso9660.c and core/psexe.c, both host-tested
 *   hand off hal/handoff.c, which relocates the last few instructions
 *
 * See docs/cd-boot.md for what each covers and where the limits are.
 */

#include "core/iso9660.h"
#include "core/psexe.h"
#include "ui/launch.h"
#include "libc/string.h"
#include "modules/modules.h"

/* SYSTEM.CNF is a few short lines, but reads land whole sectors. */
static uint8_t  configBuffer[ISO_SECTOR_SIZE];
static Iso9660  disc;

static TinromResult readSectors(
	uint32_t lba, void *buffer, size_t count, void *user
) {
	(void) user;

	return tinromCdReadSectors(lba, buffer, count);
}

/* ui/launch.h takes its reader user-first; iso9660.h takes it last. */
static TinromResult launchReader(
	void *user, uint32_t sector, void *buffer, size_t count
) {
	(void) user;

	return tinromCdReadSectors(sector, buffer, count);
}

static void step(TinromContext *ctx, const char *message) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Boot disc", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, message);
	uiEndFrame(&ctx->ui);
}

/* --- unlocking ---------------------------------------------------------- */

static bool ensureReadable(TinromContext *ctx, const TinromDiscInfo *info) {
	if (info->licenceStringPresent || tinromCdUnlocked())
		return true;

	step(ctx, "Unlocking the drive...");

	TinromResult result = tinromCdUnlock();

	if (result == TINROM_OK)
		return true;

	if (result == TINROM_ERR_UNSUPPORTED) {
		uiMessageBox(ctx, "Cannot unlock",
			"This console's drive does not support the unlock "
			"commands, or ignores them.\n\n"
			"That is every Japanese and Asian console, and any "
			"drive firmware older than vC1.\n\n"
			"The door-swap sequence is the only route on those, "
			"and it is not implemented yet.");

		return false;
	}

	uiShowResult(ctx, "Unlock", result);

	return false;
}

/* --- booting ------------------------------------------------------------ */

static void bootDisc(TinromContext *ctx) {
	step(ctx, "Identifying the disc...");

	TinromDiscInfo info;
	TinromResult   result = tinromCdIdentify(&info);

	if ((result != TINROM_OK) || (info.type == TINROM_DISC_NONE)) {
		uiMessageBox(ctx, "No disc",
			"Nothing readable in the drive.\n\n"
			"Check the disc is seated and the lid is closed.");

		return;
	}

	if (info.type == TINROM_DISC_AUDIO) {
		uiMessageBox(ctx, "Audio disc",
			"That is an audio CD. There is nothing to boot.");

		return;
	}

	if (!ensureReadable(ctx, &info))
		return;

	step(ctx, "Reading the filesystem...");

	result = iso9660Mount(&disc, readSectors, NULL);

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Mounting the disc", result);

		return;
	}

	uint32_t size = 0;

	result = iso9660ReadFile(
		&disc, "\\SYSTEM.CNF", configBuffer, sizeof(configBuffer), &size
	);

	char bootPath[64];
	bool fromConfig = (result == TINROM_OK);

	if (result == TINROM_ERR_NOT_PRESENT) {
		/*
		 * A disc with no SYSTEM.CNF boots \PSX.EXE by convention - the same
		 * fallback the console's own shell uses.
		 */
		TINROM_LOG("cdboot: no SYSTEM.CNF, trying PSX.EXE");
		strcpy(bootPath, "\\PSX.EXE");
	} else if (result != TINROM_OK) {
		/*
		 * One that is there but would not read is a reading problem, not a
		 * disc without one: guessing PSX.EXE then only produces a message
		 * about the wrong file.
		 */
		char body[128];

		TINROM_LOG("cdboot: SYSTEM.CNF did not read (%s)",
			tinromResultString(result));
		snprintf(body, sizeof(body),
			"SYSTEM.CNF is on the disc but could not be read (%s).",
			tinromResultString(result));
		uiMessageBox(ctx, "Cannot boot", body);

		return;
	} else if (!tinromParseBootLine(
		(const char *) configBuffer, size, bootPath, sizeof(bootPath)
	)) {
		uiMessageBox(ctx, "Cannot boot",
			"SYSTEM.CNF has no BOOT line, so the disc does not "
			"say which executable is its game.");

		return;
	}

	TINROM_LOG("cdboot: boot = %s", bootPath);

	uint32_t lba, fileSize;

	result = iso9660Find(&disc, bootPath, &lba, &fileSize);

	if (result != TINROM_OK) {
		char body[160];

		if (fromConfig)
			snprintf(body, sizeof(body),
				"SYSTEM.CNF points at\n\n  %s\n\nand that is not on the disc "
				"(%s).", bootPath, tinromResultString(result));
		else
			snprintf(body, sizeof(body),
				"The disc has no SYSTEM.CNF, and no %s to boot "
				"instead (%s).", bootPath + 1, tinromResultString(result));
		uiMessageBox(ctx, "Cannot boot", body);

		return;
	}

	/*
	 * Everything from here - parsing the header, planning around tinrom's own
	 * footprint, refusing with an explanation, loading and entering - is
	 * shared with the file browser. See ui/launch.h.
	 */
	/* Nothing to quiesce: see "Nothing to stop" in modules/filebrowser. */
	uiLaunchExe(ctx, bootPath, launchReader, NULL, lba, NULL);
}

/* ------------------------------------------------------------------------ */

static TinromResult cdbootEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Boot the disc",      "Unlock if needed, then hand over to it", true, 0 },
		{ "Unlock the drive",   "Stop it refusing CD-Rs and imports", true, 1 },
		{ "Re-lock the drive",  "Puts the region check back", true, 2 },
		{ "Swap trick",         "For consoles the unlock does not cover", true, 4 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Boot disc", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (items[choice].tag) {
			case 0:
				bootDisc(ctx);
				break;

			case 1: {
				TinromResult result = tinromCdUnlock();

				if (result == TINROM_ERR_UNSUPPORTED)
					uiMessageBox(ctx, "Cannot unlock",
						"This console ignores the unlock commands.\n\n"
						"Japanese and Asian consoles contain the code "
						"but do not act on the result, and firmware "
						"older than vC1 has no such commands at all.");
				else
					uiShowResult(ctx, "Unlock", result);

				break;
			}

			case 2:
				uiShowResult(ctx, "Re-lock", tinromCdLock());
				break;

			default:
				uiNotImplemented(ctx, items[choice].label);
				break;
		}
	}
}

const TinromModule tinromModule_cdboot = {
	.id      = "cdboot",
	.name    = "Boot disc",
	.summary = "Unlock the drive and start what is in it",
	.minTier = TINROM_TIER_BASIC,
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_AUTOSTART,
	.init    = NULL,
	.enter   = cdbootEnter,
	.tick    = NULL,
	.shutdown = NULL
};
