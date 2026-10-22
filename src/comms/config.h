/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * What the terminal and IRC clients remember: the modem's profile, the
 * bookmark list and the canned phrases.
 *
 * It can be saved to either memory card, in one of two places on each: in
 * tinrom's settings save, or in a save of its own (core/savefile.h). When
 * there are several, the one saved most recently is the one loaded.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "comms/modem.h"
#include "core/result.h"

#define COMMS_BOOKMARKS     16
#define COMMS_NAME_MAX      24
#define COMMS_TARGET_MAX    64
#define COMMS_PHRASES       8
#define COMMS_PHRASE_MAX    48
#define COMMS_NICK_MAX      32
#define COMMS_REALNAME_MAX  40
#define COMMS_CHANNEL_MAX   64
/* A password's bytes: a length, then up to 31 characters. */
#define COMMS_SECRET_MAX    32

typedef enum {
	COMMS_KIND_TERMINAL = 0,
	COMMS_KIND_IRC      = 1
} CommsKind;

/* Whether to speak telnet to the far end. */
typedef enum {
	COMMS_TELNET_AUTO = 0, /* yes for port 23, or no port at all */
	COMMS_TELNET_ON   = 1,
	COMMS_TELNET_OFF  = 2
} CommsTelnetMode;

typedef enum {
	COMMS_SECRET_NONE   = 0,
	COMMS_SECRET_PLAIN  = 1,
	/* Encrypted with the password lock's key: see comms/vault.h. */
	COMMS_SECRET_SEALED = 2
} CommsSecretState;

/*
 * A stored password. Sealed, `data` is ChaCha20 ciphertext under `nonce` -
 * the length included, so a sealed password does not give away how long it
 * is. Use comms/vault.h rather than reading it directly.
 */
typedef struct {
	uint8_t state;                   /* CommsSecretState */
	uint8_t reserved[3];
	uint8_t nonce[12];
	uint8_t data[COMMS_SECRET_MAX];
} CommsSecret;

/* What an IRC bookmark adds; unused by the terminal's. */
typedef struct {
	char        nick[COMMS_NICK_MAX];
	char        realName[COMMS_REALNAME_MAX];
	char        channel[COMMS_CHANNEL_MAX]; /* "#chan" or "#chan key" */
	CommsSecret serverPassword;              /* sent as PASS */
	CommsSecret nickServPassword;            /* IDENTIFY, after welcome */
	uint8_t     showJoins;                   /* print joins, parts, quits */
	uint8_t     reserved[3];
} CommsIrcProfile;

typedef struct {
	char            name[COMMS_NAME_MAX];
	char            target[COMMS_TARGET_MAX]; /* what follows the dial prefix */
	uint8_t         kind;                     /* CommsKind */
	uint8_t         telnet;                   /* CommsTelnetMode */
	uint8_t         reserved[2];
	CommsIrcProfile irc;
} CommsBookmark;

/* The password lock. See comms/vault.h. */
typedef struct {
	uint8_t  enabled;
	uint8_t  reserved[3];
	uint32_t iterations; /* PBKDF2's */
	uint32_t counter;    /* goes into every nonce, so none repeats */
	uint8_t  salt[16];
	uint8_t  check[16];  /* tells a right master password from a wrong one */
} CommsVault;

typedef struct {
	CommsModemProfile modem;
	CommsBookmark     bookmarks[COMMS_BOOKMARKS];
	uint8_t           bookmarkCount;
	char              phrases[COMMS_PHRASES][COMMS_PHRASE_MAX];
	CommsVault        vault;
} CommsConfig;

/* Where the configuration is saved. */
typedef enum {
	COMMS_PLACE_SETTINGS = 0, /* a section of tinrom's settings save */
	COMMS_PLACE_OWN      = 1  /* a save of its own */
} CommsPlaceKind;

typedef struct {
	uint8_t port; /* 0 or 1 */
	uint8_t kind; /* CommsPlaceKind */
} CommsPlace;

#ifdef __cplusplus
extern "C" {
#endif

/** The one configuration, set to defaults the first time it is asked for. */
CommsConfig *commsConfig(void);
void         commsConfigDefaults(CommsConfig *config);

/** A bookmark of `kind` with the defaults filled in. */
void commsBookmarkDefaults(CommsBookmark *bookmark, CommsKind kind);

/** Adds one; false when the list is full. */
bool commsBookmarkAdd(CommsConfig *config, const CommsBookmark *bookmark);
void commsBookmarkRemove(CommsConfig *config, unsigned index);

/** Whether `bookmark` should speak telnet, its mode and port considered. */
bool commsBookmarkTelnet(const CommsBookmark *bookmark);

/**
 * Looks at all four places and loads the newest, once: later calls do
 * nothing. TINROM_ERR_NOT_PRESENT when there is nothing saved anywhere,
 * which leaves the defaults.
 */
TinromResult commsConfigLoadOnce(void);
TinromResult commsConfigLoadFrom(CommsPlace place);
/** Whether `place` holds a saved configuration, without loading it. */
bool         commsConfigSavedAt(CommsPlace place);
TinromResult commsConfigSaveTo(CommsPlace place);

/**
 * Where "Save" goes: with the settings, on the card tinrom's settings came
 * from (see tinromSettingsSaveCard()). False with no card in either slot.
 */
bool commsConfigDefaultPlace(CommsPlace *place);
/** Where it was last loaded from or saved to. False if neither, yet. */
bool commsConfigCurrentPlace(CommsPlace *place);

/** "Card 1, with the settings", for menus. */
const char *commsPlaceName(CommsPlace place);

#ifdef __cplusplus
}
#endif
