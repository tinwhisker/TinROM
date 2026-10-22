/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The settings screen, including the tier selector - the thing that decides
 * how much of tinrom a given user is shown.
 */

#include "core/profile.h"
#include "core/savefile.h"
#include "modules/modules.h"

/*
 * Choosing a value is not the same as opening a screen. Feature level, video
 * mode and serial rate each have a handful of options and a current one, so
 * they are context menus over the settings list rather than screens of their
 * own: the row you are changing stays visible above the choices, and the
 * cursor starts on the value that is already set.
 *
 * Modules is still a screen. It is as long as the module registry, and a
 * popup that needs scrolling has stopped being a popup.
 */

static const UIMenuItem *settingsItems;
static size_t            settingsCount;
static size_t            settingsSelected;

/* Repainted under every picker, so the list behind looks untouched. */
static void drawSettingsBehind(void *user) {
	TinromContext *ctx = (TinromContext *) user;

	uiMenuDraw(&ctx->ui, settingsItems, settingsCount, settingsSelected);
}

static TinromResult chooseTier(TinromContext *ctx, int x, int y) {
	UIMenuItem items[TINROM_TIER_COUNT];

	for (int i = 0; i < TINROM_TIER_COUNT; i++) {
		items[i].label   = tinromTierName((TinromTier) i);
		items[i].summary = tinromTierDescription((TinromTier) i);
		items[i].enabled = true;
		items[i].tag     = (uintptr_t) i;
	}

	size_t cursor = (size_t) ctx->settings.tier;
	size_t choice = uiPopupRunAt(
		ctx, "Feature level", items, TINROM_TIER_COUNT, x, y,
		drawSettingsBehind, ctx, &cursor
	);

	if (choice == UI_MENU_CANCELLED)
		return TINROM_ERR_ABORTED;

	TinromTier tier = (TinromTier) choice;

	if (tier == ctx->settings.tier)
		return TINROM_OK;

	if (!uiConfirm(
		ctx, "Feature level",
		"Switching level resets which modules are enabled. "
		"Any per-module overrides you set will be lost.\n\nContinue?",
		false
	))
		return TINROM_ERR_ABORTED;

	tinromSettingsApplyTier(&ctx->settings, tier);
	ctx->settingsDirty = true;

	TINROM_LOG("tier -> %s", tinromTierName(tier));

	return TINROM_OK;
}

static TinromResult chooseModules(TinromContext *ctx) {
	UIMenuItem items[TINROM_MAX_MODULES];
	size_t     indices[TINROM_MAX_MODULES];
	char       labels[TINROM_MAX_MODULES][40];

	/*
	 * Kept across the loop, not across the screen: toggling a module comes
	 * straight back here, and the row that was just changed should still be
	 * under the cursor.
	 */
	size_t cursor = 0;

	for (;;) {
		size_t count = 0;

		for (size_t i = 0; i < tinromModuleCount(); i++) {
			const TinromModule *module = tinromModuleAt(i);

			if (!module || (module->flags & TINROM_MODULE_ESSENTIAL))
				continue;
			if (module->minTier > ctx->settings.tier)
				continue;

			snprintf(
				labels[count], sizeof(labels[count]), "[%c] %s",
				tinromSettingsModuleEnabled(&ctx->settings, i) ? 'x' : ' ',
				module->name
			);

			items[count].label   = labels[count];
			items[count].summary = module->summary;
			items[count].enabled = tinromModuleAvailable(i);
			items[count].tag     = i;
			indices[count]       = i;
			count++;
		}

		size_t choice = uiMenuRunAt(ctx, "Modules", items, count, &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		size_t index = indices[choice];

		tinromSettingsSetModuleEnabled(
			&ctx->settings, index,
			!tinromSettingsModuleEnabled(&ctx->settings, index)
		);
		ctx->settingsDirty = true;
	}
}

static TinromResult chooseBaud(TinromContext *ctx, int x, int y) {
	UIMenuItem items[8];

	for (unsigned int i = 0; i < tinromSioBaudCount && i < 8; i++) {
		items[i].label   = tinromSioBaudTable[i].label;
		items[i].summary = "Rate used until the host negotiates another";
		items[i].enabled = true;
		items[i].tag     = i;
	}

	size_t cursor = ctx->settings.serialBaudIndex;

	if (cursor >= tinromSioBaudCount)
		cursor = 0;

	size_t choice = uiPopupRunAt(
		ctx, "Serial rate", items, tinromSioBaudCount, x, y,
		drawSettingsBehind, ctx, &cursor
	);

	if (choice == UI_MENU_CANCELLED)
		return TINROM_ERR_ABORTED;

	ctx->settings.serialBaudIndex = (uint8_t) choice;
	ctx->settingsDirty            = true;

	return tinromSioSetBaud(tinromSioBaudTable[choice].baud);
}

static TinromResult chooseVideo(TinromContext *ctx, int x, int y) {
	static const UIMenuItem items[] = {
		{ "Auto",  "Follow whatever the console booted in", true, TINROM_VIDEO_AUTO },
		{ "NTSC",  "Force 60Hz timing",                     true, TINROM_VIDEO_NTSC },
		{ "PAL",   "Force 50Hz timing",                     true, TINROM_VIDEO_PAL  }
	};

	/* By tag rather than by position, so reordering the rows cannot silently
	 * open the picker on the wrong one. */
	size_t cursor = 0;

	for (size_t i = 0; i < 3; i++) {
		if (items[i].tag == ctx->settings.videoPreference)
			cursor = i;
	}

	size_t choice = uiPopupRunAt(
		ctx, "Video mode", items, 3, x, y, drawSettingsBehind, ctx, &cursor
	);

	if (choice == UI_MENU_CANCELLED)
		return TINROM_ERR_ABORTED;

	ctx->settings.videoPreference = (uint8_t) items[choice].tag;
	ctx->settingsDirty            = true;

	tinromVideoInit((TinromVideoPreference) ctx->settings.videoPreference);

	return TINROM_OK;
}

/* --- autoboot ------------------------------------------------------------ */

static const char *autobootName(const TinromContext *ctx) {
	const TinromModule *module = (ctx->settings.autobootModule == 0xff) ? NULL
		: tinromModuleAt(ctx->settings.autobootModule);

	return module ? module->name : "nothing";
}

/*
 * Only modules that say they can be started cold (TINROM_MODULE_AUTOSTART),
 * and only ones this profile shows: autobooting into something switched off
 * would be a surprise, and app.c would refuse it anyway.
 */
static void chooseAutoboot(TinromContext *ctx, int x, int y) {
	UIMenuItem items[TINROM_MAX_MODULES + 1];
	size_t     count  = 0;
	size_t     cursor = 0;

	items[count++] = (UIMenuItem) { "Nothing", "Start in the main menu",
		true, 0xff };

	for (size_t i = 0; (i < tinromModuleCount()) && (count < TINROM_MAX_MODULES);
		i++) {
		const TinromModule *module = tinromModuleAt(i);

		if (!module || !(module->flags & TINROM_MODULE_AUTOSTART)
			|| !tinromModuleVisible(ctx, i))
			continue;

		if (ctx->settings.autobootModule == i)
			cursor = count;

		items[count++] = (UIMenuItem) { module->name,
			"Hold Select and Start at power-on to skip it",
			tinromModuleAvailable(i), i };
	}

	size_t choice = uiPopupRunAt(ctx, "Autoboot", items, count, x, y,
		drawSettingsBehind, ctx, &cursor);

	if (choice == UI_MENU_CANCELLED)
		return;

	ctx->settings.autobootModule = (uint8_t) items[choice].tag;
	ctx->settingsDirty           = true;
}

/* --- the cart switch ------------------------------------------------------- */

static void chooseSwitchSource(TinromContext *ctx, int x, int y) {
	static const UIMenuItem items[] = {
		{ "Not at all", "One set of choices, whatever the switch says", true,
			TINROM_SWITCH_NONE },
		{ "Xplorer-style", "1F060000h bit 0: Xplorer carts and pcsx-redux",
			true, TINROM_SWITCH_XPLORER },
		{ "Action Replay-style", "1F020018h bit 0: Action Replay, GameShark",
			true, TINROM_SWITCH_DATEL }
	};
	size_t cursor = (size_t) tinromSwitchSource();

	size_t choice = uiPopupRunAt(ctx, "Read the switch", items, 3, x, y,
		NULL, NULL, &cursor);

	if ((choice == UI_MENU_CANCELLED)
		|| (items[choice].tag == (uintptr_t) tinromSwitchSource()))
		return;

	tinromSwitchSetSource(&ctx->settings, (TinromSwitchSource) items[choice].tag);
	ctx->settingsDirty = true;

	if (tinromSwitchSource() == TINROM_SWITCH_NONE)
		return;

	char body[200];

	snprintf(body, sizeof(body), "It reads %s now, so the level, modules and "
		"autoboot on screen are that position's. Flip it and choose \"Test "
		"the switch\": if the reading does not follow, the cart is the other "
		"kind, or has no readable switch.",
		(tinromProfileActive() == TINROM_PROFILE_ON) ? "ON" : "OFF");
	uiMessageBox(ctx, "Cart switch", body);
}

static void switchMenu(TinromContext *ctx) {
	size_t cursor = 0;

	for (;;) {
		bool        on     = tinromSwitchSource() != TINROM_SWITCH_NONE;
		const char *editing = (tinromProfileActive() == TINROM_PROFILE_ON)
			? "ON" : "OFF";
		char sourceLabel[48], liveLabel[48], editLabel[48];

		snprintf(sourceLabel, sizeof(sourceLabel), "Read it: %s",
			tinromSwitchSourceName(tinromSwitchSource()));
		snprintf(liveLabel, sizeof(liveLabel), "Swap when flipped: %s",
			tinromSwitchLive() ? "in the menu" : "at power-on");
		snprintf(editLabel, sizeof(editLabel), "Setting up: switch %s",
			editing);

		const UIMenuItem items[] = {
			{ sourceLabel, "Which kind of cart: there is no telling", true, 0 },
			{ liveLabel, "Or only when tinrom starts", on, 1 },
			{ editLabel, "Whose level, modules and autoboot are in use", on, 2 },
			{ "Copy to the other position", "Start the other from these", on, 3 },
			{ "Test the switch", "Read it now", on, 4 }
		};

		size_t choice = uiMenuRunAt(ctx, "Cart switch", items, 5, &cursor);
		int    x, y;

		uiMenuRowAnchor(cursor, &x, &y);

		switch (choice) {
			case 0:
				chooseSwitchSource(ctx, x, y);
				break;

			case 1:
				tinromSwitchSetLive(!tinromSwitchLive());
				ctx->settingsDirty = true;
				break;

			case 2:
				tinromProfileUse(&ctx->settings,
					(tinromProfileActive() == TINROM_PROFILE_ON)
						? TINROM_PROFILE_OFF : TINROM_PROFILE_ON);
				break;

			case 3: {
				char body[96];

				snprintf(body, sizeof(body), "Give switch %s the level, modules "
					"and autoboot switch %s has now?",
					(tinromProfileActive() == TINROM_PROFILE_ON) ? "OFF" : "ON",
					editing);

				if (uiConfirm(ctx, "Copy", body, false)) {
					tinromProfileCopyToOther(&ctx->settings);
					ctx->settingsDirty = true;
				}
				break;
			}

			case 4:
				uiMessageBox(ctx, "Cart switch",
					tinromExpSwitchOn(tinromSwitchSource())
						? "The switch reads ON." : "The switch reads OFF.");
				break;

			default:
				return;
		}
	}
}

/* After a save: a message saying where it went, or why not. */
static void reportSave(TinromContext *ctx, unsigned int port,
	TinromResult result) {
	char body[128];

	switch (result) {
		case TINROM_OK:
			snprintf(body, sizeof(body), "Saved to the card in slot %u. tinrom "
				"reads it from there at power-on - slot 1 first, then 2.",
				port + 1);
			uiMessageBox(ctx, "Saved", body);
			return;

		case TINROM_ERR_NO_SPACE:
			snprintf(body, sizeof(body), "Card %u has no free block. Save to the "
				"other card, or free one in Memory cards.", port + 1);
			break;

		case TINROM_ERR_NOT_PRESENT:
			snprintf(body, sizeof(body), "There is no card in slot %u.",
				port + 1);
			break;

		default:
			snprintf(body, sizeof(body), "Writing to card %u failed: %s.",
				port + 1, tinromResultString(result));
			break;
	}

	uiMessageBox(ctx, "Not saved", body);
}

static void saveTo(TinromContext *ctx, unsigned int port) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "Settings", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, "Saving...");
	uiEndFrame(&ctx->ui);

	TinromResult result = tinromSettingsSaveToCard(&ctx->settings, port);

	if (result == TINROM_OK)
		ctx->settingsDirty = false;

	reportSave(ctx, port, result);
}

static void saveToChosen(TinromContext *ctx, int x, int y) {
	char        labels[2][24];
	UIMenuItem  items[2];
	static size_t cursor;

	for (unsigned port = 0; port < 2; port++) {
		bool present = tinromSaveCardPresent(port);

		snprintf(labels[port], sizeof(labels[port]), "Card %u", port + 1);
		items[port] = (UIMenuItem) { labels[port],
			present ? ((tinromSettingsCard() == (int) port)
				? "Where they are saved now" : "Make this the one")
				: "No card in this slot",
			present, port };
	}

	size_t choice = uiPopupRunAt(ctx, "Save to", items, 2, x, y,
		drawSettingsBehind, ctx, &cursor);

	if (choice != UI_MENU_CANCELLED)
		saveTo(ctx, (unsigned) choice);
}

static TinromResult settingsEnter(TinromContext *ctx) {
	size_t cursor = 0;

	for (;;) {
		char tierLabel[48], baudLabel[48], videoLabel[48], ttyLabel[48];
		char saveSummary[48], autobootLabel[48], switchLabel[48];
		char profileSummary[48];
		int  saveCard = tinromSettingsSaveCard();

		if (saveCard >= 0)
			snprintf(saveSummary, sizeof(saveSummary), "To the card in slot %d",
				saveCard + 1);
		else
			snprintf(saveSummary, sizeof(saveSummary), "No memory card is in");

		snprintf(tierLabel, sizeof(tierLabel), "Feature level: %s",
			tinromTierName(ctx->settings.tier));
		snprintf(baudLabel, sizeof(baudLabel), "Serial rate: %s",
			tinromSioBaudTable[
				(ctx->settings.serialBaudIndex < tinromSioBaudCount)
					? ctx->settings.serialBaudIndex : 0
			].label);
		snprintf(videoLabel, sizeof(videoLabel), "Video mode: %s",
			(ctx->settings.videoPreference == TINROM_VIDEO_PAL)  ? "PAL"  :
			(ctx->settings.videoPreference == TINROM_VIDEO_NTSC) ? "NTSC" : "Auto");
		snprintf(ttyLabel, sizeof(ttyLabel), "Mirror log to serial: %s",
			ctx->settings.ttyToSerial ? "on" : "off");
		snprintf(autobootLabel, sizeof(autobootLabel), "Autoboot: %s",
			autobootName(ctx));

		const char *position = tinromSwitchLabel();

		if (position)
			snprintf(switchLabel, sizeof(switchLabel), "Cart switch: %s",
				(tinromProfileActive() == TINROM_PROFILE_ON) ? "on" : "off");
		else
			snprintf(switchLabel, sizeof(switchLabel), "Cart switch: not used");

		/* The three rows a profile holds say whose they are. */
		snprintf(profileSummary, sizeof(profileSummary), "For %s",
			position ? position : "every boot");

		const UIMenuItem items[] = {
			{ tierLabel,  position ? profileSummary
				: "Basic, Advanced or Developer",                     true, 0 },
			{ "Modules",  position ? profileSummary
				: "Turn individual features on and off",              true, 1 },
			{ videoLabel, "PAL/NTSC output timing",                    true, 2 },
			{ baudLabel,  "SIO1 rate before the host negotiates",      true, 3 },
			{ ttyLabel,   "Send the boot log out of the serial port",  true, 4 },
			{ autobootLabel, position ? profileSummary
				: "Jump straight into a module at power-on",          true, 5 },
			{ switchLabel, "A set of choices for each position",       true, 9 },
			{ "Save settings", saveSummary, saveCard >= 0, 6 },
			{ "Save settings to...", "Card 1 or card 2", true, 7 },
			{ "Reset to defaults", "Forget everything and start over", true, 8 }
		};
		const size_t count = sizeof(items) / sizeof(items[0]);

		size_t choice = uiMenuRunAt(ctx, "Settings", items, count, &cursor);

		/* What the pickers repaint behind themselves, and where they hang. */
		settingsItems    = items;
		settingsCount    = count;
		settingsSelected = cursor;

		int anchorX, anchorY;

		uiMenuRowAnchor(cursor, &anchorX, &anchorY);

		switch ((choice == UI_MENU_CANCELLED) ? UI_MENU_CANCELLED
			: items[choice].tag) {
			case 0: chooseTier(ctx, anchorX, anchorY);  break;
			case 1: chooseModules(ctx);                 break;
			case 2: chooseVideo(ctx, anchorX, anchorY); break;
			case 3: chooseBaud(ctx, anchorX, anchorY);  break;

			case 4:
				ctx->settings.ttyToSerial = !ctx->settings.ttyToSerial;
				tinromLogSetSerialMirror(ctx->settings.ttyToSerial);
				ctx->settingsDirty = true;
				break;

			case 5:
				chooseAutoboot(ctx, anchorX, anchorY);
				break;

			case 9:
				switchMenu(ctx);
				break;

			case 6:
				saveTo(ctx, (unsigned) saveCard);
				break;

			case 7:
				saveToChosen(ctx, anchorX, anchorY);
				break;

			case 8:
				if (uiConfirm(
					ctx, "Reset settings",
					"Everything goes back to the build defaults.\n\nContinue?",
					true
				)) {
					tinromSettingsDefaults(&ctx->settings);
					tinromProfilesReset(&ctx->settings);
					ctx->settingsDirty = true;
				}
				break;

			default:
				return TINROM_OK;
		}
	}
}

const TinromModule tinromModule_settingsui = {
	.id      = "settingsui",
	.name    = "Settings",
	.summary = "Feature level, modules, video, serial and autoboot",
	.minTier = TINROM_TIER_BASIC,
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_ESSENTIAL,
	.init    = NULL,
	.enter   = settingsEnter,
	.tick    = NULL,
	.shutdown = NULL
};
