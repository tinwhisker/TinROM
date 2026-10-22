/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The IRC client: a server list in front of ircSession(), each server's
 * nick, channel and passwords, and the password lock. See docs/irc.md.
 */

#include <stdio.h>
#include "comms/commsui.h"
#include "comms/config.h"
#include "comms/vault.h"
#include "libc/string.h"
#include "modules/irc/client.h"
#include "modules/modules.h"
#include "ui/keyboard.h"

#define TITLE "IRC"

/* --- the password lock ------------------------------------------------------ */

static TinromContext *progressCtx;
static const char    *progressText;

/* Deriving the key takes a second; this keeps the screen saying so. */
static void showProgress(uint32_t done, uint32_t total) {
	UIContext *ui = &progressCtx->ui;

	uiBeginFrame(ui);
	uiSetChrome(ui, TITLE, "");
	uiText(ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, progressText);
	uiProgressBar(ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 40,
		(int) ui->width - 2 * (TINROM_MARGIN_X + 4), done, total);
	uiEndFrame(ui);
}

static CommsVaultProgress progressFor(TinromContext *ctx, const char *text) {
	progressCtx  = ctx;
	progressText = text;
	showProgress(0, 1);

	return showProgress;
}

/** Asks for the master password until it is right or the user gives up. */
static bool unlock(TinromContext *ctx) {
	CommsConfig *config = commsConfig();

	if (!commsVaultEnabled(config) || commsVaultUnlocked(config))
		return true;

	for (;;) {
		char password[64] = "";

		if (!uiEditSecret(ctx, "Master password", "The IRC passwords are "
			"locked. Type the master password to unlock them until power-off.",
			password, sizeof(password)))
			return false;

		TinromResult result = commsVaultUnlock(config, password,
			progressFor(ctx, "Unlocking..."));

		memset(password, 0, sizeof(password));

		if (result == TINROM_OK)
			return true;

		if (!uiConfirm(ctx, "Wrong password", "That is not the master "
			"password. Try again?", false))
			return false;
	}
}

/* Asks for a new master password twice. False if abandoned or mismatched. */
static bool newMaster(TinromContext *ctx, char *out, size_t size) {
	char again[64] = "";

	out[0] = 0;

	if (!uiEditSecret(ctx, "New master password", "Long is good: a copy of the "
		"card can be guessed at on a PC. There is no way to recover it.",
		out, size) || !out[0])
		return false;

	bool ok = uiEditSecret(ctx, "Once more", "The same again, to be sure.",
		again, sizeof(again)) && !strcmp(out, again);

	memset(again, 0, sizeof(again));

	if (!ok) {
		memset(out, 0, size);
		uiMessageBox(ctx, "Not changed", "The two did not match.");
	}

	return ok;
}

static void vaultMenu(TinromContext *ctx) {
	CommsConfig *config = commsConfig();
	char         password[64];

	if (!commsVaultEnabled(config)) {
		if (!uiConfirm(ctx, "Password lock", "Seal the server and NickServ "
			"passwords with a master password? You will be asked for it once "
			"per power-on, before connecting.", false))
			return;
		if (!newMaster(ctx, password, sizeof(password)))
			return;

		commsVaultEnable(config, password, progressFor(ctx, "Locking..."));
		memset(password, 0, sizeof(password));
		uiMessageBox(ctx, "Locked", "The passwords are sealed. Save to keep "
			"it that way on the card.");
		return;
	}

	bool open = commsVaultUnlocked(config);
	UIMenuItem items[] = {
		{ open ? "Lock now" : "Unlock", open ? "Forget the key until asked"
			: "Type the master password", true, 0 },
		{ "Change master password", "The passwords are sealed again", true, 1 },
		{ "Turn the lock off", "Store the passwords as typed", true, 2 },
		{ "Forget the passwords", "For a lost master password", true, 3 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(ctx, "Password lock", items,
		sizeof(items) / sizeof(items[0]), 120, 60, NULL, NULL, &cursor);

	switch (choice) {
		case 0:
			if (open)
				commsVaultLock();
			else
				unlock(ctx);
			break;

		case 1:
			if (!unlock(ctx) || !newMaster(ctx, password, sizeof(password)))
				break;

			commsVaultChange(config, password, progressFor(ctx, "Locking..."));
			memset(password, 0, sizeof(password));
			uiMessageBox(ctx, "Changed", "Save to keep the new master password "
				"on the card.");
			break;

		case 2:
			if (!unlock(ctx))
				break;
			if (!uiConfirm(ctx, "Lock off", "Store the passwords as typed, "
				"readable by anything that reads the card?", true))
				break;

			commsVaultDisable(config);
			uiMessageBox(ctx, "Unlocked", "Save to store them that way.");
			break;

		case 3:
			if (uiConfirm(ctx, "Forget", "Delete every sealed password and turn "
				"the lock off? Nicks and servers stay.", true))
				commsVaultForget(config);
			break;
	}
}

/* --- a server ------------------------------------------------------------------ */

static const char *secretLabel(const CommsConfig *config,
	const CommsSecret *secret) {
	if (!commsSecretPresent(secret))
		return "none";
	if ((secret->state == COMMS_SECRET_SEALED) && !commsVaultUnlocked(config))
		return "set, locked";

	return "set";
}

static void editSecret(TinromContext *ctx, CommsSecret *secret,
	const char *title, const char *prompt) {
	CommsConfig *config = commsConfig();
	char         text[COMMS_SECRET_MAX] = "";

	if (!unlock(ctx))
		return;

	commsSecretGet(config, secret, text, sizeof(text));

	if (uiEditSecret(ctx, title, prompt, text, sizeof(text)))
		commsSecretSet(config, secret, text);

	memset(text, 0, sizeof(text));
}

static bool editServer(TinromContext *ctx, CommsBookmark *bookmark) {
	CommsConfig  *config = commsConfig();
	CommsBookmark edited = *bookmark;
	size_t        cursor = 0;

	for (;;) {
		CommsIrcProfile *irc = &edited.irc;
		char name[COMMS_NAME_MAX + 8], target[COMMS_TARGET_MAX + 10];
		char nick[COMMS_NICK_MAX + 8], real[COMMS_REALNAME_MAX + 12];
		char channel[COMMS_CHANNEL_MAX + 10], pass[40], nickServ[40];
		char joins[40];

		snprintf(name, sizeof(name), "Name: %s", edited.name);
		snprintf(target, sizeof(target), "Address: %s", edited.target);
		snprintf(nick, sizeof(nick), "Nick: %s", irc->nick);
		snprintf(real, sizeof(real), "Real name: %s", irc->realName);
		snprintf(channel, sizeof(channel), "Channel: %s",
			irc->channel[0] ? irc->channel : "(none)");
		snprintf(pass, sizeof(pass), "Server password: %s",
			secretLabel(config, &irc->serverPassword));
		snprintf(nickServ, sizeof(nickServ), "NickServ password: %s",
			secretLabel(config, &irc->nickServPassword));
		snprintf(joins, sizeof(joins), "Show joins and quits: %s",
			irc->showJoins ? "yes" : "no");

		const UIMenuItem items[] = {
			{ name,     "What the list calls it", true, 0 },
			{ target,   "host:port, as the modem dials it", true, 1 },
			{ nick,     "Who to be; another is tried if it is taken", true, 2 },
			{ real,     "What /whois shows", true, 3 },
			{ channel,  "Joined on connecting: #chan, or #chan key", true, 4 },
			{ pass,     "Sent as PASS; most servers want none", true, 5 },
			{ nickServ, "IDENTIFY, sent to NickServ on connecting", true, 6 },
			{ joins,    "Off quietens a busy channel", true, 7 },
			{ "Save",   "Keep these changes", true, 8 }
		};

		size_t choice = uiMenuRunAt(ctx, "Server", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		switch (choice) {
			case UI_MENU_CANCELLED:
				return false;

			case 0:
				uiEditText(ctx, "Name", NULL, edited.name, sizeof(edited.name));
				break;

			case 1:
				uiEditText(ctx, "Address", "host:port - irc.libera.chat:6667, "
					"say. Plain text only: the modem has no TLS.",
					edited.target, sizeof(edited.target));
				break;

			case 2:
				uiEditText(ctx, "Nick", "Letters, digits and -_[]\\`^{}|; most "
					"networks allow 16 or more.", irc->nick, sizeof(irc->nick));
				break;

			case 3:
				uiEditText(ctx, "Real name", NULL, irc->realName,
					sizeof(irc->realName));
				break;

			case 4:
				uiEditText(ctx, "Channel", "One channel: tinrom talks in one at "
					"a time. A key, if it has one, after a space.",
					irc->channel, sizeof(irc->channel));
				break;

			case 5:
				editSecret(ctx, &irc->serverPassword, "Server password",
					"Empty for none.");
				break;

			case 6:
				editSecret(ctx, &irc->nickServPassword, "NickServ password",
					"Empty for none. Sent as IDENTIFY once the server welcomes "
					"you.");
				break;

			case 7:
				irc->showJoins = !irc->showJoins;
				break;

			case 8:
				*bookmark = edited;
				return true;
		}
	}
}

/* USER's username: the nick, as far as it is letters and digits. */
static void userFromNick(const char *nick, char *out, size_t size) {
	size_t used = 0;

	for (; *nick && (used + 1 < size) && (used < 9); nick++) {
		char c = *nick;

		if (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))
			|| ((c >= '0') && (c <= '9')))
			out[used++] = (char) (((c >= 'A') && (c <= 'Z')) ? c + 32 : c);
	}

	out[used] = 0;

	if (!used)
		snprintf(out, size, "tinrom");
}

static void connectTo(TinromContext *ctx, CommsBookmark *bookmark) {
	CommsConfig            *config = commsConfig();
	const CommsIrcProfile  *irc    = &bookmark->irc;
	static IrcSessionLogin  login;

	if (!irc->nick[0]) {
		uiMessageBox(ctx, "No nick", "Give this server a nick first: Edit.");
		return;
	}

	bool sealed = (irc->serverPassword.state == COMMS_SECRET_SEALED)
		|| (irc->nickServPassword.state == COMMS_SECRET_SEALED);

	if (sealed && !unlock(ctx))
		return;

	memset(&login, 0, sizeof(login));
	snprintf(login.nick, sizeof(login.nick), "%s", irc->nick);
	snprintf(login.realName, sizeof(login.realName), "%s",
		irc->realName[0] ? irc->realName : irc->nick);
	snprintf(login.channel, sizeof(login.channel), "%s", irc->channel);
	userFromNick(irc->nick, login.user, sizeof(login.user));
	commsSecretGet(config, &irc->serverPassword, login.serverPassword,
		sizeof(login.serverPassword));
	commsSecretGet(config, &irc->nickServPassword, login.nickServPassword,
		sizeof(login.nickServPassword));
	login.showJoins = irc->showJoins != 0;

	ircSession(ctx, bookmark, &login);
	memset(&login, 0, sizeof(login));
}

static void serverActions(TinromContext *ctx, unsigned index, int x, int y) {
	CommsConfig *config = commsConfig();

	static const UIMenuItem items[] = {
		{ "Connect", "Dial it and log in", true, 0 },
		{ "Edit",    "Address, nick, channel, passwords", true, 1 },
		{ "Delete",  "Take it off the list", true, 2 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(ctx, config->bookmarks[index].name, items,
		3, x, y, NULL, NULL, &cursor);

	switch (choice) {
		case 0:
			connectTo(ctx, &config->bookmarks[index]);
			break;

		case 1:
			editServer(ctx, &config->bookmarks[index]);
			break;

		case 2:
			if (uiConfirm(ctx, "Delete", "Take this server off the list?", true))
				commsBookmarkRemove(config, index);
			break;
	}
}

/* --- the list ------------------------------------------------------------------ */

enum {
	TAG_NEW = 0x100,
	TAG_LOCK,
	TAG_MODEM
};

static TinromResult ircEnter(TinromContext *ctx) {
	static size_t cursor;

	commsUiLoadOnce(ctx, TITLE);

	/* Typing - a master password most of all - stirs the entropy pool. */
	uiKeyboardSetKeyHook(commsEntropyStir);

	for (;;) {
		CommsConfig *config = commsConfig();
		UIMenuItem   items[COMMS_BOOKMARKS + 6];
		char         summaries[COMMS_BOOKMARKS][48];
		char         saveSummary[COMMS_UI_SUMMARY_MAX];
		char         lockLabel[40];
		size_t       count = 0;

		for (unsigned i = 0; i < config->bookmarkCount; i++) {
			const CommsBookmark *b = &config->bookmarks[i];

			if (b->kind != COMMS_KIND_IRC)
				continue;

			snprintf(summaries[i], sizeof(summaries[i]), "%s %s %s",
				b->irc.nick, b->irc.channel[0] ? b->irc.channel : "on",
				b->target);
			items[count++] = (UIMenuItem) { b->name, summaries[i], true, i };
		}

		snprintf(lockLabel, sizeof(lockLabel), "Password lock: %s",
			!commsVaultEnabled(config) ? "off"
				: commsVaultUnlocked(config) ? "on, unlocked" : "on, locked");

		items[count++] = (UIMenuItem) { "New server", "Add one to this list",
			config->bookmarkCount < COMMS_BOOKMARKS, TAG_NEW };
		items[count++] = (UIMenuItem) { lockLabel,
			"Seal the passwords with a master password", true, TAG_LOCK };
		items[count++] = (UIMenuItem) { "Modem settings",
			"Rate, init string, how to hang up", true, TAG_MODEM };
		count = commsUiSaveItems(items, count, saveSummary);

		size_t choice = uiMenuRunAt(ctx, TITLE, items, count, &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (items[choice].tag) {
			case TAG_NEW: {
				CommsBookmark bookmark;

				commsBookmarkDefaults(&bookmark, COMMS_KIND_IRC);
				snprintf(bookmark.name, sizeof(bookmark.name), "New server");
				snprintf(bookmark.target, sizeof(bookmark.target),
					"irc.domain:6667");

				if (editServer(ctx, &bookmark))
					commsBookmarkAdd(config, &bookmark);
				break;
			}

			case TAG_LOCK:
				vaultMenu(ctx);
				break;

			case TAG_MODEM:
				commsUiModemSettings(ctx);
				break;

			default: {
				if (commsUiSaveAction(ctx, items[choice].tag, TITLE))
					break;

				int x, y;

				uiMenuRowAnchor(choice, &x, &y);
				serverActions(ctx, (unsigned) items[choice].tag, x + 40, y);
				break;
			}
		}
	}
}

const TinromModule tinromModule_irc = {
	.id      = "irc",
	.name    = "IRC",
	.summary = "Chat on IRC through a WiFi modem on SIO1",
	.minTier = TINROM_TIER_ADVANCED,
	/* The port is claimed for a session only, as the terminal does. */
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_AUTOSTART,
	.init    = NULL,
	.enter   = ircEnter,
	.tick    = NULL,
	.shutdown = NULL
};
