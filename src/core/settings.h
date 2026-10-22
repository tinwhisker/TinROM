/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"
#include "core/tier.h"

#define TINROM_SETTINGS_MAGIC   0x4d4e4954 /* 'TINM' little-endian */
#define TINROM_SETTINGS_VERSION 1
#define TINROM_MAX_MODULES      32

typedef enum {
	TINROM_VIDEO_AUTO = 0,
	TINROM_VIDEO_NTSC = 1,
	TINROM_VIDEO_PAL  = 2
} TinromVideoPreference;

/*
 * Where a settings block lives. The cart EEPROM is preferred when tinrom is
 * running from one (it survives a card swap); the memory card is the fallback
 * for disc and FreePSXBoot installs. Both backends are stubs today.
 */
typedef enum {
	TINROM_STORE_NONE    = 0,
	TINROM_STORE_EEPROM  = 1,
	TINROM_STORE_MEMCARD = 2
} TinromSettingsStore;

typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t size;

	TinromTier tier;

	uint8_t  videoPreference;  /* TinromVideoPreference */
	uint8_t  autobootModule;   /* registry index, or 0xFF for none */
	uint8_t  autobootDelay;    /* seconds; 0 = immediate */
	uint8_t  serialBaudIndex;  /* index into tinromSioBaudTable */

	bool     ttyToSerial;      /* mirror console output to SIO1 */
	bool     debuggerAtBoot;   /* arm the kernel-resident debugger */
	bool     skipSplash;
	bool     confirmDestructive;

	/* Runtime enable bit per registry index. Essential modules ignore it. */
	uint32_t moduleEnableMask;

	uint32_t checksum;         /* over everything above */
} TinromSettings;

#ifdef __cplusplus
extern "C" {
#endif

/** Fill with build-time defaults. Never fails. */
void tinromSettingsDefaults(TinromSettings *settings);

/**
 * Apply a tier preset: turns modules on/off to a sensible starting set for
 * that tier. The user can still override individual modules afterwards.
 */
void tinromSettingsApplyTier(TinromSettings *settings, TinromTier tier);

bool tinromSettingsModuleEnabled(const TinromSettings *settings, size_t index);
void tinromSettingsSetModuleEnabled(
	TinromSettings *settings, size_t index, bool enabled
);

uint32_t     tinromSettingsChecksum(const TinromSettings *settings);
bool         tinromSettingsValid(const TinromSettings *settings);

/**
 * Reads the settings from a memory card: card 1 first, then card 2 (see
 * core/savefile.h). The card they came from becomes the default for saving.
 * Settings saved by a build with a different set of modules keep everything
 * but which modules are on, which go back to the tier's preset.
 */
TinromResult tinromSettingsLoad(TinromSettings *settings);
/**
 * TINROM_STORE_MEMCARD writes to the default card: the one the settings were
 * loaded from or last saved to, or else card 1, or else card 2. The cart
 * EEPROM store is not written yet.
 */
TinromResult tinromSettingsSave(
	const TinromSettings *settings, TinromSettingsStore store
);
/** Writes to a particular card, 0 or 1, which becomes the default. */
TinromResult tinromSettingsSaveToCard(
	const TinromSettings *settings, unsigned int port
);
/**
 * Puts the settings into the save file in memory (core/savefile.h), for
 * something else storing the settings file - so that a settings file made
 * to hold bookmarks is a valid settings file too.
 */
TinromResult tinromSettingsPutSection(const TinromSettings *settings);
/** Makes `port` the default card, after something stored settings there. */
void tinromSettingsSetCard(unsigned int port);
/** The default card for saving, 0 or 1; -1 when none has been used. */
int tinromSettingsCard(void);
/**
 * The card a save goes to when nobody says which: the default if it is
 * there, else card 1, else card 2. -1 with no card in either slot.
 */
int tinromSettingsSaveCard(void);

#ifdef __cplusplus
}
#endif
