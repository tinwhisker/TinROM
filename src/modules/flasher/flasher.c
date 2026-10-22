/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Cart EEPROM identification and flashing.
 *
 * STATUS: skeleton. The module registers, appears in the menu at the right
 * tier and presents its real action list, but every action is a stub. The
 * work belongs behind the HAL calls named in each case below.
 */

#include "modules/modules.h"

/*
 * Implementation notes: tinromFlashIdentify() first, then pick a command
 * sequence from a table keyed on manufacturer/device id. Never erase before
 * the replacement image has been received and checksummed - a failed flash
 * with the old image already gone is how people end up with a dead cart.
 */

static TinromResult flasherEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Identify the cart",                 "JEDEC software ID, size and sector layout", true, 0 },
		{ "Flash an image from the host",      "Receive a .rom over serial and write it", true, 1 },
		{ "Back up the cart to the host",      "Read the whole EEPROM out over serial", true, 2 },
		{ "Verify against the host",           "Compare without writing anything", true, 3 },
		{ "Erase the cart",                    "The cart will not boot until reflashed", true, 4 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Cart flasher", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		uiNotImplemented(ctx, items[choice].label);
	}
}

const TinromModule tinromModule_flasher = {
	.id      = "flasher",
	.name    = "Cart flasher",
	.summary = "Identify the cart EEPROM and flash it",
	.minTier = TINROM_TIER_ADVANCED,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = flasherEnter,
	.tick    = NULL,
	.shutdown = NULL
};
