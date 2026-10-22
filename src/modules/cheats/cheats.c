/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Cheat engine: code entry, per-disc storage, and application at boot.
 *
 * STATUS: skeleton. The module registers, appears in the menu at the right
 * tier and presents its real action list, but every action is a stub. The
 * work belongs behind the HAL calls named in each case below.
 */

#include "modules/modules.h"

/*
 * Implementation notes: the code format is address/value pairs with a type
 * nibble. Continuous application needs a hook that survives a game running,
 * which is the same vblank residency the exception module needs - build that
 * first and share it.
 */

static TinromResult cheatsEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Codes for this disc",               "Whatever is stored against the current serial", true, 0 },
		{ "Add a code",                        "On-screen keypad entry", true, 1 },
		{ "Import codes from the host",        "Over serial, in the usual text format", true, 2 },
		{ "Apply now",                         "Write the enabled codes into RAM", true, 3 },
		{ "Apply continuously",                "Re-apply every frame via the vblank hook", true, 4 },
		{ "Clear all codes",                   "Forget everything stored for this disc", true, 5 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Cheats", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		uiNotImplemented(ctx, items[choice].label);
	}
}

const TinromModule tinromModule_cheats = {
	.id      = "cheats",
	.name    = "Cheats",
	.summary = "Enter, store and apply cheat codes",
	.minTier = TINROM_TIER_ADVANCED,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = cheatsEnter,
	.tick    = NULL,
	.shutdown = NULL
};
