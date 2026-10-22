/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PAL/NTSC soft patching.
 *
 * STATUS: skeleton. The module registers, appears in the menu at the right
 * tier and presents its real action list, but every action is a stub. The
 * work belongs behind the HAL calls named in each case below.
 */

#include "modules/modules.h"

/*
 * Implementation notes: the display half is just tinromVideoSetMode(). The
 * part that matters is patching the game after it loads but before it runs,
 * which needs the same post-load hook the cheat engine uses.
 */

static TinromResult regionPatchEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Auto",                              "Match the patch to the disc in the drive", true, 0 },
		{ "Force PAL output",                  "50Hz timing regardless of the disc", true, 1 },
		{ "Force NTSC output",                 "60Hz timing regardless of the disc", true, 2 },
		{ "Off",                               "Leave the game to set its own timing", true, 3 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Region patch", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		uiNotImplemented(ctx, items[choice].label);
	}
}

const TinromModule tinromModule_regionpatch = {
	.id      = "regionpatch",
	.name    = "Region patch",
	.summary = "Patch PAL/NTSC so imports run at speed",
	.minTier = TINROM_TIER_ADVANCED,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = regionPatchEnter,
	.tick    = NULL,
	.shutdown = NULL
};
