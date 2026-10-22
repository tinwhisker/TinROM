/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The serial terminal: a bookmark list in front of terminalSession(). See
 * docs/terminal.md.
 */

#include <stdio.h>
#include "comms/commsui.h"
#include "comms/config.h"
#include "hal/uart_sio.h"
#include "libc/string.h"
#include "modules/modules.h"
#include "modules/terminal/terminal.h"
#include "ui/keyboard.h"

/* --- bookmarks ------------------------------------------------------------ */

static const char *telnetLabel(uint8_t mode) {
	switch (mode) {
		case COMMS_TELNET_ON:  return "on";
		case COMMS_TELNET_OFF: return "off";
		default:               return "auto (port 23)";
	}
}

bool terminalEditBookmark(TinromContext *ctx, CommsBookmark *bookmark) {
	CommsBookmark edited = *bookmark;
	size_t        cursor = 0;

	for (;;) {
		char name[COMMS_NAME_MAX + 8], target[COMMS_TARGET_MAX + 10];
		char telnet[32];

		snprintf(name, sizeof(name), "Name: %s", edited.name);
		snprintf(target, sizeof(target), "Address: %s", edited.target);
		snprintf(telnet, sizeof(telnet), "Telnet: %s",
			telnetLabel(edited.telnet));

		const UIMenuItem items[] = {
			{ name,   "What the list calls it", true, 0 },
			{ target, "host:port, as the dial command takes it", true, 1 },
			{ telnet, "Answer a BBS's telnet negotiation", true, 2 },
			{ "Save", "Keep these changes", true, 3 }
		};

		size_t choice = uiMenuRunAt(ctx, "Bookmark", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		switch (choice) {
			case UI_MENU_CANCELLED:
				return false;

			case 0:
				uiEditText(ctx, "Name", NULL, edited.name, sizeof(edited.name));
				break;

			case 1:
				uiEditText(ctx, "Address",
					"host:port - somebbs.domain:23 - or whatever else the "
					"adapter dials.", edited.target, sizeof(edited.target));
				break;

			case 2:
				edited.telnet = (uint8_t) ((edited.telnet + 1) % 3);
				break;

			case 3:
				*bookmark = edited;

				return true;
		}
	}
}

static void bookmarkActions(TinromContext *ctx, unsigned index, int x,
	int y) {
	CommsConfig *config = commsConfig();

	static const UIMenuItem items[] = {
		{ "Connect", "Dial it", true, 0 },
		{ "Edit",    "Name, address, telnet", true, 1 },
		{ "Delete",  "Take it off the list", true, 2 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(ctx, config->bookmarks[index].name, items,
		3, x, y, NULL, NULL, &cursor);

	switch (choice) {
		case 0:
			terminalSession(ctx, config->bookmarks[index].name,
				&config->bookmarks[index]);
			break;

		case 1:
			terminalEditBookmark(ctx, &config->bookmarks[index]);
			break;

		case 2:
			if (uiConfirm(ctx, "Delete", "Take this bookmark off the list?",
				true))
				commsBookmarkRemove(config, index);
			break;
	}
}

/* --- the list --------------------------------------------------------------- */

enum {
	TAG_DIAL   = 0x100,
	TAG_DIRECT,
	TAG_NEW,
	TAG_MODEM
};

static TinromResult terminalEnter(TinromContext *ctx) {
	static size_t cursor;

	/* The first time in: whatever was saved most recently, on either card. */
	commsUiLoadOnce(ctx, "Terminal");

	for (;;) {
		CommsConfig *config = commsConfig();
		UIMenuItem   items[COMMS_BOOKMARKS + 7];
		size_t       count = 0;
		char         saveSummary[COMMS_UI_SUMMARY_MAX];

		for (unsigned i = 0; i < config->bookmarkCount; i++) {
			if (config->bookmarks[i].kind != COMMS_KIND_TERMINAL)
				continue;

			items[count++] = (UIMenuItem) { config->bookmarks[i].name,
				config->bookmarks[i].target, true, i };
		}

		items[count++] = (UIMenuItem) { "Dial an address...",
			"Type one in, without saving it", true, TAG_DIAL };
		items[count++] = (UIMenuItem) { "Talk to the modem",
			"Type AT commands at it yourself", true, TAG_DIRECT };
		items[count++] = (UIMenuItem) { "New bookmark",
			"Add one to this list", config->bookmarkCount < COMMS_BOOKMARKS,
			TAG_NEW };
		items[count++] = (UIMenuItem) { "Modem settings",
			"Rate, init string, how to hang up", true, TAG_MODEM };
		count = commsUiSaveItems(items, count, saveSummary);

		size_t choice = uiMenuRunAt(ctx, "Terminal", items, count, &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (items[choice].tag) {
			case TAG_DIAL: {
				static CommsBookmark typed = {
					.name = "Typed address", .kind = COMMS_KIND_TERMINAL,
					.telnet = COMMS_TELNET_AUTO
				};

				if (uiEditText(ctx, "Dial", "host:port to dial.",
					typed.target, sizeof(typed.target)) && typed.target[0])
					terminalSession(ctx, typed.target, &typed);
				break;
			}

			case TAG_DIRECT:
				terminalSession(ctx, "Modem", NULL);
				break;

			case TAG_NEW: {
				CommsBookmark bookmark;

				commsBookmarkDefaults(&bookmark, COMMS_KIND_TERMINAL);

				if (terminalEditBookmark(ctx, &bookmark))
					commsBookmarkAdd(config, &bookmark);
				break;
			}

			case TAG_MODEM:
				commsUiModemSettings(ctx);
				break;

			default: {
				if (commsUiSaveAction(ctx, items[choice].tag, "Terminal"))
					break;

				int x, y;

				uiMenuRowAnchor(choice, &x, &y);
				bookmarkActions(ctx, (unsigned) items[choice].tag, x + 40, y);
				break;
			}
		}
	}
}

const TinromModule tinromModule_terminal = {
	.id      = "terminal",
	.name    = "Terminal",
	.summary = "Dial a BBS through a WiFi modem on SIO1",
	.minTier = TINROM_TIER_ADVANCED,
	/*
	 * Not OWNS_SIO: the port is claimed for a session only, by buffered
	 * receive (hal/uart_sio.h), and the serial console carries on otherwise.
	 */
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_AUTOSTART,
	.init    = NULL,
	.enter   = terminalEnter,
	.tick    = NULL,
	.shutdown = NULL
};
