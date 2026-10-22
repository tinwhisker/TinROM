/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The screens the terminal and the IRC client share. See commsui.h.
 */

#include <stdio.h>
#include "comms/commsui.h"
#include "comms/config.h"
#include "core/app.h"
#include "core/savefile.h"
#include "libc/string.h"
#include "core/result.h"
#include "ui/menu.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "ui/widgets.h"
#include "ui/keyboard.h"

/* --- the modem's settings ------------------------------------------------- */

static const uint32_t rates[] = {
	1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
};

static void chooseRate(TinromContext *ctx, CommsModemProfile *modem) {
	UIMenuItem items[sizeof(rates) / sizeof(rates[0])];
	char       labels[sizeof(rates) / sizeof(rates[0])][12];
	size_t     cursor = 0;

	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		snprintf(labels[i], sizeof(labels[i]), "%u", (unsigned int) rates[i]);
		items[i] = (UIMenuItem) { labels[i],
			(rates[i] > 19200) ? "Wants hardware flow control"
				: "Safe without flow control", true, i };

		if (rates[i] == modem->baud)
			cursor = i;
	}

	size_t choice = uiPopupRunAt(ctx, "Rate", items,
		sizeof(items) / sizeof(items[0]), 150, 50, NULL, NULL, &cursor);

	if (choice != UI_MENU_CANCELLED)
		modem->baud = rates[choice];
}

void commsUiModemSettings(TinromContext *ctx) {
	CommsModemProfile *modem  = &commsConfig()->modem;
	size_t             cursor = 0;

	for (;;) {
		char rate[32], init[COMMS_INIT_MAX + 8], prefix[24], hangup[32];
		char flow[32], carrier[32];

		snprintf(rate, sizeof(rate), "Rate: %u", (unsigned int) modem->baud);
		snprintf(init, sizeof(init), "Init: %s",
			modem->init[0] ? modem->init : "(none)");
		snprintf(prefix, sizeof(prefix), "Dial: %s", modem->dialPrefix);
		snprintf(hangup, sizeof(hangup), "Hang up: %s",
			(modem->hangup == COMMS_HANGUP_DTR) ? "drop DTR" : "+++ ATH");
		snprintf(flow, sizeof(flow), "Wait for CTS: %s",
			modem->ctsFlow ? "yes" : "no");
		snprintf(carrier, sizeof(carrier), "Carrier on DSR: %s",
			modem->carrierOnDsr ? "yes" : "no");

		const UIMenuItem items[] = {
			{ rate,    "The adapter's rate; set it there to match", true, 0 },
			{ init,    "Sent before every dial", true, 1 },
			{ prefix,  "Put in front of the address", true, 2 },
			{ hangup,  "DTR needs AT&D2 and a cable that wires it", true, 3 },
			{ flow,    "Only for a cable that wires CTS", true, 4 },
			{ carrier, "For a cable taking DCD to DSR", true, 5 }
		};

		size_t choice = uiMenuRunAt(ctx, "Modem", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		switch (choice) {
			case UI_MENU_CANCELLED:
				return;

			case 0:
				chooseRate(ctx, modem);
				break;

			case 1:
				uiEditText(ctx, "Init string",
					"Sent before every dial, followed by CR. ATE0V1 turns echo "
					"off and asks for words. Empty sends nothing.",
					modem->init, sizeof(modem->init));
				break;

			case 2:
				uiEditText(ctx, "Dial prefix",
					"Put in front of the address. ATDT on most adapters; some "
					"want ATD.", modem->dialPrefix, sizeof(modem->dialPrefix));
				break;

			case 3:
				modem->hangup = (modem->hangup == COMMS_HANGUP_DTR)
					? COMMS_HANGUP_ESCAPE : COMMS_HANGUP_DTR;
				break;

			case 4:
				modem->ctsFlow = !modem->ctsFlow;
				break;

			case 5:
				modem->carrierOnDsr = !modem->carrierOnDsr;
				break;
		}
	}
}

/* --- saving and loading ---------------------------------------------------- */

void commsUiBusy(TinromContext *ctx, const char *title, const char *text) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, title, "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, text);
	uiEndFrame(&ctx->ui);
}

void commsUiLoadOnce(TinromContext *ctx, const char *title) {
	static bool loaded;

	if (loaded)
		return;

	loaded = true;
	commsUiBusy(ctx, title, "Reading memory cards...");
	commsConfigLoadOnce();
}

static void reportSave(TinromContext *ctx, CommsPlace place,
	TinromResult result) {
	char body[160];

	switch (result) {
		case TINROM_OK:
			snprintf(body, sizeof(body), "Bookmarks, phrases and modem settings "
				"saved to %s.", commsPlaceName(place));
			uiMessageBox(ctx, "Saved", body);
			return;

		case TINROM_ERR_NO_SPACE:
			snprintf(body, sizeof(body), "Card %u has no free block. Save to "
				"the other card, or free one in Memory cards.", place.port + 1u);
			break;

		case TINROM_ERR_NOT_PRESENT:
			snprintf(body, sizeof(body), "There is no card in slot %u.",
				place.port + 1u);
			break;

		default:
			snprintf(body, sizeof(body), "Writing to card %u failed: %s.",
				place.port + 1u, tinromResultString(result));
			break;
	}

	uiMessageBox(ctx, "Not saved", body);
}

static void saveHere(TinromContext *ctx, const char *title, CommsPlace place) {
	commsUiBusy(ctx, title, "Saving...");
	reportSave(ctx, place, commsConfigSaveTo(place));
}

/*
 * The four places, for saving to or loading from. Loading offers only the
 * ones that hold something.
 */
static bool choosePlace(TinromContext *ctx, const char *screen,
	const char *title, bool loading, CommsPlace *place) {
	UIMenuItem   items[4];
	char         labels[4][40];
	CommsPlace   places[4];
	size_t       count = 0;
	static size_t cursor;

	commsUiBusy(ctx, screen, "Reading memory cards...");

	for (uint8_t port = 0; port < 2; port++) {
		bool present = tinromSaveCardPresent(port);

		for (uint8_t kind = 0; kind < 2; kind++) {
			CommsPlace here  = { port, kind };
			bool       saved = present && commsConfigSavedAt(here);
			bool       usable = loading ? saved : present;

			snprintf(labels[count], sizeof(labels[count]), "%s",
				commsPlaceName(here));
			places[count] = here;
			items[count]  = (UIMenuItem) { labels[count],
				!present ? "No card in this slot"
					: saved ? "Something is saved here"
					: loading ? "Nothing saved here" : "Nothing saved here yet",
				usable, count };
			count++;
		}
	}

	size_t choice = uiPopupRunAt(ctx, title, items, count, 120, 60, NULL, NULL,
		&cursor);

	if (choice == UI_MENU_CANCELLED)
		return false;

	*place = places[choice];

	return true;
}

static void loadFrom(TinromContext *ctx, const char *screen) {
	CommsPlace place;

	if (!choosePlace(ctx, screen, "Load from", true, &place))
		return;

	if (!uiConfirm(ctx, "Load", "Replace the bookmarks, phrases and modem "
		"settings with the saved ones?", false))
		return;

	commsUiBusy(ctx, screen, "Loading...");

	if (commsConfigLoadFrom(place) == TINROM_OK)
		return;

	uiMessageBox(ctx, "Not loaded", "That save could not be read.");
}

size_t commsUiSaveItems(UIMenuItem *items, size_t count,
	char summary[COMMS_UI_SUMMARY_MAX]) {
	CommsPlace place;
	bool       canSave = commsConfigDefaultPlace(&place);

	snprintf(summary, COMMS_UI_SUMMARY_MAX, "To %s",
		canSave ? commsPlaceName(place) : "a card: none is in");

	items[count++] = (UIMenuItem) { "Save", summary, canSave,
		COMMS_UI_TAG_SAVE };
	items[count++] = (UIMenuItem) { "Save as...",
		"The other card, or a save of their own", true, COMMS_UI_TAG_SAVE_AS };
	items[count++] = (UIMenuItem) { "Load from...",
		"Bookmarks saved on either card", true, COMMS_UI_TAG_LOAD };

	return count;
}

bool commsUiSaveAction(TinromContext *ctx, uintptr_t tag, const char *title) {
	CommsPlace place;

	switch (tag) {
		case COMMS_UI_TAG_SAVE:
			if (commsConfigDefaultPlace(&place))
				saveHere(ctx, title, place);
			return true;

		case COMMS_UI_TAG_SAVE_AS:
			if (choosePlace(ctx, title, "Save to", false, &place))
				saveHere(ctx, title, place);
			return true;

		case COMMS_UI_TAG_LOAD:
			loadFrom(ctx, title);
			return true;

		default:
			return false;
	}
}
