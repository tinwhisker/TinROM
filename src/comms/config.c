/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The clients' configuration, and saving it. See config.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "comms/config.h"
#include "core/app.h"
#include "core/log.h"
#include "core/savefile.h"
#include "core/settings.h"
#include "libc/string.h"

static CommsConfig config;
static bool        configReady;

CommsConfig *commsConfig(void) {
	if (!configReady) {
		commsConfigDefaults(&config);
		configReady = true;
	}

	return &config;
}

void commsBookmarkDefaults(CommsBookmark *bookmark, CommsKind kind) {
	memset(bookmark, 0, sizeof(*bookmark));
	bookmark->kind   = (uint8_t) kind;
	bookmark->telnet = (kind == COMMS_KIND_IRC) ? COMMS_TELNET_OFF
		: COMMS_TELNET_AUTO;

	snprintf(bookmark->name, sizeof(bookmark->name), "New bookmark");

	if (kind == COMMS_KIND_IRC) {
		snprintf(bookmark->irc.nick, sizeof(bookmark->irc.nick), "psxuser");
		snprintf(bookmark->irc.realName, sizeof(bookmark->irc.realName),
			"tinrom on a PlayStation");
		bookmark->irc.showJoins = 1;
	}
}

static void addDefault(CommsConfig *c, const char *name, const char *target,
	CommsKind kind) {
	CommsBookmark bookmark;

	commsBookmarkDefaults(&bookmark, kind);
	snprintf(bookmark.name, sizeof(bookmark.name), "%s", name);
	snprintf(bookmark.target, sizeof(bookmark.target), "%s", target);
	commsBookmarkAdd(c, &bookmark);
}

void commsConfigDefaults(CommsConfig *c) {
	memset(c, 0, sizeof(*c));
	commsModemDefaults(&c->modem);

	/* Placeholders to edit: there is no address book to ship. */
	addDefault(c, "Some BBS", "somebbs.domain:23", COMMS_KIND_TERMINAL);
	addDefault(c, "Some IRC server", "irc.domain:6667", COMMS_KIND_IRC);
	snprintf(c->bookmarks[1].irc.channel, COMMS_CHANNEL_MAX, "#tinrom");

	/* Mostly for talking to the modem itself. */
	static const char *const phrases[] = {
		"ATI", "AT&V", "AT&W", "y", "n", "q"
	};

	for (unsigned i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++)
		snprintf(c->phrases[i], COMMS_PHRASE_MAX, "%s", phrases[i]);
}

bool commsBookmarkAdd(CommsConfig *c, const CommsBookmark *bookmark) {
	if (c->bookmarkCount >= COMMS_BOOKMARKS)
		return false;

	c->bookmarks[c->bookmarkCount++] = *bookmark;

	return true;
}

void commsBookmarkRemove(CommsConfig *c, unsigned index) {
	if (index >= c->bookmarkCount)
		return;

	memmove(&c->bookmarks[index], &c->bookmarks[index + 1],
		(c->bookmarkCount - index - 1) * sizeof(CommsBookmark));
	c->bookmarkCount--;

	/* The slot left over still held the last one - passwords and all. */
	memset(&c->bookmarks[c->bookmarkCount], 0, sizeof(CommsBookmark));
}

bool commsBookmarkTelnet(const CommsBookmark *bookmark) {
	switch (bookmark->telnet) {
		case COMMS_TELNET_ON:
			return true;

		case COMMS_TELNET_OFF:
			return false;

		default: {
			const char *colon = strchr(bookmark->target, ':');

			return !colon || !strcmp(colon + 1, "23");
		}
	}
}

/* --- on a memory card ----------------------------------------------------- */

/*
 * What is stored. `serial` counts up across every save, so that of several
 * copies the newest can be told; a different layout of CommsConfig is a
 * different `version`, and is not loaded.
 */
#define STORED_VERSION 2

typedef struct {
	uint32_t    version;
	uint32_t    size;
	uint32_t    serial;
	CommsConfig config;
} StoredComms;

_Static_assert(sizeof(StoredComms) + 64 <= TINROM_SAVE_CAPACITY,
	"the bookmarks and the settings no longer fit one save block");

/*
 * Version 1, before IRC bookmarks had anything of their own: what the
 * terminal saved first. Read and brought up to date, never written.
 */
typedef struct {
	char    name[COMMS_NAME_MAX];
	char    target[COMMS_TARGET_MAX];
	uint8_t kind;
	uint8_t telnet;
} BookmarkV1;

typedef struct {
	CommsModemProfile modem;
	BookmarkV1        bookmarks[COMMS_BOOKMARKS];
	uint8_t           bookmarkCount;
	char              phrases[COMMS_PHRASES][COMMS_PHRASE_MAX];
} ConfigV1;

typedef struct {
	uint32_t version;
	uint32_t size;
	uint32_t serial;
	ConfigV1 config;
} StoredV1;

static const StoredComms *upgradeV1(const StoredV1 *old) {
	static StoredComms upgraded;
	CommsConfig       *c = &upgraded.config;

	memset(&upgraded, 0, sizeof(upgraded));
	upgraded.version = STORED_VERSION;
	upgraded.size    = sizeof(CommsConfig);
	upgraded.serial  = old->serial;

	c->modem         = old->config.modem;
	c->bookmarkCount = old->config.bookmarkCount;
	memcpy(c->phrases, old->config.phrases, sizeof(c->phrases));

	for (unsigned i = 0; i < COMMS_BOOKMARKS; i++) {
		const BookmarkV1 *from = &old->config.bookmarks[i];
		CommsBookmark    *to   = &c->bookmarks[i];

		commsBookmarkDefaults(to, (CommsKind) from->kind);
		memcpy(to->name, from->name, sizeof(to->name));
		memcpy(to->target, from->target, sizeof(to->target));
		to->telnet = from->telnet;
	}

	return &upgraded;
}

/* The serial of a stored section of either version, or 0. */
static uint32_t storedSerial(const void *section, size_t length) {
	if (!section || (length < 12))
		return 0;

	uint32_t serial;

	memcpy(&serial, (const uint8_t *) section + 8, sizeof(serial));

	return serial;
}

static bool       loadTried;
static bool       havePlace;
static CommsPlace currentPlace;
static uint32_t   newestSerial;

static const TinromSaveFile *placeFile(CommsPlace place) {
	return (place.kind == COMMS_PLACE_OWN) ? &tinromSaveComms
		: &tinromSaveSettings;
}

const char *commsPlaceName(CommsPlace place) {
	static char name[40];

	snprintf(name, sizeof(name), "Card %u, %s", place.port + 1u,
		(place.kind == COMMS_PLACE_OWN) ? "its own save" : "with the settings");

	return name;
}

/* Reads `place` into the save buffer; the stored copy, or NULL. */
static const StoredComms *readPlace(CommsPlace place) {
	if ((place.port > 1) || !tinromSaveCardPresent(place.port))
		return NULL;
	if (tinromSaveLoad(place.port, placeFile(place)) != TINROM_OK)
		return NULL;

	size_t             length;
	const StoredComms *stored = tinromSaveSection(TINROM_SECTION_COMMS, &length);

	if (!stored || (length < 8))
		return NULL;

	/* The section is only 4-byte aligned in the buffer; so are these. */
	if ((stored->version == 1) && (length == sizeof(StoredV1))
		&& (stored->size == sizeof(ConfigV1)))
		return upgradeV1((const StoredV1 *) stored);

	if ((length != sizeof(StoredComms)) || (stored->version != STORED_VERSION)
		|| (stored->size != sizeof(CommsConfig)))
		return NULL;

	return stored;
}

static void adopt(const StoredComms *stored, CommsPlace place) {
	memcpy(&config, &stored->config, sizeof(config));
	configReady  = true;
	havePlace    = true;
	currentPlace = place;

	/* Strings come off a card: make sure they end. */
	config.bookmarkCount = (config.bookmarkCount > COMMS_BOOKMARKS)
		? COMMS_BOOKMARKS : config.bookmarkCount;

	/* Slots past the list are nobody's; an older build may have left one. */
	for (unsigned i = config.bookmarkCount; i < COMMS_BOOKMARKS; i++)
		memset(&config.bookmarks[i], 0, sizeof(CommsBookmark));

	for (unsigned i = 0; i < COMMS_BOOKMARKS; i++) {
		config.bookmarks[i].name[COMMS_NAME_MAX - 1]     = 0;
		config.bookmarks[i].target[COMMS_TARGET_MAX - 1] = 0;
	}

	for (unsigned i = 0; i < COMMS_PHRASES; i++)
		config.phrases[i][COMMS_PHRASE_MAX - 1] = 0;

	config.modem.init[COMMS_INIT_MAX - 1]         = 0;
	config.modem.dialPrefix[COMMS_PREFIX_MAX - 1] = 0;

	for (unsigned i = 0; i < COMMS_BOOKMARKS; i++) {
		CommsIrcProfile *irc = &config.bookmarks[i].irc;

		irc->nick[COMMS_NICK_MAX - 1]         = 0;
		irc->realName[COMMS_REALNAME_MAX - 1] = 0;
		irc->channel[COMMS_CHANNEL_MAX - 1]   = 0;
	}
}

bool commsConfigSavedAt(CommsPlace place) {
	return readPlace(place) != NULL;
}

TinromResult commsConfigLoadOnce(void) {
	if (loadTried)
		return havePlace ? TINROM_OK : TINROM_ERR_NOT_PRESENT;

	loadTried = true;

	/* All four places; the highest serial wins, card 1 on a tie. */
	static StoredComms best;
	CommsPlace  bestPlace = { 0, 0 };
	bool        found     = false;

	for (uint8_t port = 0; port < 2; port++) {
		for (uint8_t kind = 0; kind < 2; kind++) {
			CommsPlace         place  = { port, kind };
			const StoredComms *stored = readPlace(place);

			if (!stored)
				continue;

			if (stored->serial > newestSerial)
				newestSerial = stored->serial;

			if (!found || (stored->serial > best.serial)) {
				memcpy(&best, stored, sizeof(best));
				bestPlace = place;
				found     = true;
			}
		}
	}

	if (!found) {
		TINROM_LOG("comms: nothing saved, using defaults");

		return TINROM_ERR_NOT_PRESENT;
	}

	adopt(&best, bestPlace);
	TINROM_LOG("comms: loaded from %s", commsPlaceName(bestPlace));

	return TINROM_OK;
}

TinromResult commsConfigLoadFrom(CommsPlace place) {
	const StoredComms *stored = readPlace(place);

	if (!stored)
		return TINROM_ERR_NOT_PRESENT;

	if (stored->serial > newestSerial)
		newestSerial = stored->serial;

	adopt(stored, place);
	loadTried = true;

	return TINROM_OK;
}

/*
 * The serial is what decides which copy wins at the next boot, so it has to
 * beat every copy that exists now - including ones on a card put in after
 * the terminal last looked. Reading the other places costs a second or so,
 * which a save can afford and a stale boot cannot.
 */
static void learnNewestSerial(CommsPlace except) {
	for (uint8_t port = 0; port < 2; port++) {
		for (uint8_t kind = 0; kind < 2; kind++) {
			CommsPlace place = { port, kind };

			if ((place.port == except.port) && (place.kind == except.kind))
				continue;

			const StoredComms *stored = readPlace(place);

			if (stored && (stored->serial > newestSerial))
				newestSerial = stored->serial;
		}
	}
}

TinromResult commsConfigSaveTo(CommsPlace place) {
	if (place.port > 1)
		return TINROM_ERR_BAD_ARG;
	if (!tinromSaveCardPresent(place.port))
		return TINROM_ERR_NOT_PRESENT;

	/* Before the load below: both use the one save buffer. */
	learnNewestSerial(place);

	/* Whatever else the file holds - the settings - stays as it is. */
	TinromResult result = tinromSaveLoad(place.port, placeFile(place));

	if ((result != TINROM_OK) && (result != TINROM_ERR_NOT_PRESENT)
		&& (result != TINROM_ERR_CORRUPT))
		return result;

	/* The target's own copy counts too. */
	{
		size_t      length;
		const void *own    = tinromSaveSection(TINROM_SECTION_COMMS, &length);
		uint32_t    serial = storedSerial(own, length);

		if (serial > newestSerial)
			newestSerial = serial;
	}

	/*
	 * A settings file made just to hold bookmarks gets the settings too, so
	 * that it is a settings file like any other.
	 */
	bool newSettingsFile = (place.kind == COMMS_PLACE_SETTINGS)
		&& !tinromSaveSection(TINROM_SECTION_SETTINGS, NULL);

	if (newSettingsFile) {
		result = tinromSettingsPutSection(&tinromContext()->settings);

		if (result != TINROM_OK)
			return result;
	}

	static StoredComms stored;

	stored.version = STORED_VERSION;
	stored.size    = sizeof(CommsConfig);
	stored.serial  = ++newestSerial;
	memcpy(&stored.config, commsConfig(), sizeof(CommsConfig));

	result = tinromSaveSetSection(TINROM_SECTION_COMMS, &stored, sizeof(stored));

	if (result == TINROM_OK)
		result = tinromSaveStore(place.port, placeFile(place));

	if (result != TINROM_OK)
		return result;

	if (newSettingsFile && (tinromSettingsCard() < 0))
		tinromSettingsSetCard(place.port);

	havePlace    = true;
	currentPlace = place;
	TINROM_LOG("comms: saved to %s", commsPlaceName(place));

	return TINROM_OK;
}

bool commsConfigDefaultPlace(CommsPlace *place) {
	int port = tinromSettingsSaveCard();

	if (port < 0)
		return false;

	*place = (CommsPlace) { (uint8_t) port, COMMS_PLACE_SETTINGS };

	return true;
}

bool commsConfigCurrentPlace(CommsPlace *place) {
	if (havePlace)
		*place = currentPlace;

	return havePlace;
}
