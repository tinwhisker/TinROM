/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "core/module.h"
#include "core/savefile.h"
#include "core/profile.h"
#include "core/settings.h"
#include "libc/string.h"

#ifndef TINROM_LOG_SERIAL_DEFAULT
#define TINROM_LOG_SERIAL_DEFAULT 0
#endif

#ifndef TINROM_DEFAULT_TIER
#define TINROM_DEFAULT_TIER TINROM_TIER_ADVANCED
#endif

const char *tinromTierName(TinromTier tier) {
	switch (tier) {
		case TINROM_TIER_BASIC:     return "Basic Mode";
		case TINROM_TIER_ADVANCED:  return "Advanced Mode";
		case TINROM_TIER_DEVELOPER: return "Developer Mode";
		default:                    return "?";
	}
}

const char *tinromTierDescription(TinromTier tier) {
	switch (tier) {
		case TINROM_TIER_BASIC:
			return "Boot discs and manage saves.";
		case TINROM_TIER_ADVANCED:
			return "Adds cheats, region patching and the flasher.";
		case TINROM_TIER_DEVELOPER:
			return "All of it: serial, debugger, hex editor, PCDRV.";
		default:
			return "";
	}
}

void tinromSettingsDefaults(TinromSettings *settings) {
	memset(settings, 0, sizeof(*settings));

	settings->magic   = TINROM_SETTINGS_MAGIC;
	settings->version = TINROM_SETTINGS_VERSION;
	settings->size    = (uint16_t) sizeof(TinromSettings);

	settings->videoPreference    = TINROM_VIDEO_AUTO;
	settings->autobootModule     = 0xff;
	settings->autobootDelay      = 3;
	settings->serialBaudIndex    = 0;
	settings->ttyToSerial        = (TINROM_LOG_SERIAL_DEFAULT != 0);
	settings->debuggerAtBoot     = false;
	settings->skipSplash         = false;
	settings->confirmDestructive = true;

	tinromSettingsApplyTier(settings, TINROM_DEFAULT_TIER);

	settings->checksum = tinromSettingsChecksum(settings);
}

void tinromSettingsApplyTier(TinromSettings *settings, TinromTier tier) {
	settings->tier = tier;

	/*
	 * The preset enables every compiled-in module whose minTier the chosen
	 * tier reaches. Modules above the tier are left disabled so that raising
	 * the tier later reveals them in a predictable state.
	 */
	settings->moduleEnableMask = 0;

	for (size_t i = 0; i < tinromModuleCount(); i++) {
		const TinromModule *module = tinromModuleAt(i);

		if (module && module->minTier <= tier)
			settings->moduleEnableMask |= 1u << i;
	}
}

bool tinromSettingsModuleEnabled(const TinromSettings *settings, size_t index) {
	if (index >= TINROM_MAX_MODULES)
		return false;

	return (settings->moduleEnableMask & (1u << index)) != 0;
}

void tinromSettingsSetModuleEnabled(
	TinromSettings *settings, size_t index, bool enabled
) {
	if (index >= TINROM_MAX_MODULES)
		return;

	if (enabled)
		settings->moduleEnableMask |=  (1u << index);
	else
		settings->moduleEnableMask &= ~(1u << index);
}

uint32_t tinromSettingsChecksum(const TinromSettings *settings) {
	/* FNV-1a over the struct with the checksum field excluded. */
	const uint8_t *bytes  = (const uint8_t *) settings;
	size_t         length = offsetof(TinromSettings, checksum);
	uint32_t       hash   = 0x811c9dc5;

	for (size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= 0x01000193;
	}

	return hash;
}

bool tinromSettingsValid(const TinromSettings *settings) {
	if (settings->magic != TINROM_SETTINGS_MAGIC)
		return false;
	if (settings->version != TINROM_SETTINGS_VERSION)
		return false;
	if (settings->size != sizeof(TinromSettings))
		return false;

	return settings->checksum == tinromSettingsChecksum(settings);
}

/*
 * What is stored: the struct, and a hash of the build's module ids in
 * registry order. moduleEnableMask is indexed by registry position, which a
 * different module set renumbers, so a mask from another build would turn
 * the wrong things on. A mismatch keeps everything else and redoes the mask.
 */
typedef struct {
	TinromSettings settings;
	uint32_t       modules;
} StoredSettings;

/*
 * The cart switch's choices and the other position's profile (core/
 * profile.h), in a section of their own so that the settings section stays
 * what older builds read. Saved with the same module hash, for the same
 * reason.
 */
#define SWITCH_VERSION 1

typedef struct {
	uint32_t           version;
	uint32_t           modules;
	TinromProfileStore store;
} StoredSwitch;

static int settingsCard = -1;

static uint32_t moduleHash(void) {
	uint32_t hash = 0x811c9dc5;

	for (size_t i = 0; i < tinromModuleCount(); i++) {
		const TinromModule *module = tinromModuleAt(i);

		for (const char *c = module ? module->id : ""; *c; c++) {
			hash ^= (uint8_t) *c;
			hash *= 0x01000193;
		}

		hash ^= ';';
		hash *= 0x01000193;
	}

	return hash;
}

int tinromSettingsCard(void) {
	return settingsCard;
}

int tinromSettingsSaveCard(void) {
	if ((settingsCard >= 0) && tinromSaveCardPresent((unsigned) settingsCard))
		return settingsCard;

	for (unsigned port = 0; port < 2; port++) {
		if (tinromSaveCardPresent(port))
			return (int) port;
	}

	return -1;
}

TinromResult tinromSettingsLoad(TinromSettings *settings) {
	TinromResult last = TINROM_ERR_NOT_PRESENT;

	for (unsigned port = 0; port < 2; port++) {
		if (!tinromSaveCardPresent(port))
			continue;

		last = tinromSaveLoad(port, &tinromSaveSettings);

		if (last != TINROM_OK)
			continue;

		size_t               length;
		const StoredSettings *stored = tinromSaveSection(
			TINROM_SECTION_SETTINGS, &length
		);

		if (!stored || (length != sizeof(StoredSettings))) {
			last = TINROM_ERR_CORRUPT;
			continue;
		}

		StoredSettings copy;

		memcpy(&copy, stored, sizeof(copy));

		if (!tinromSettingsValid(&copy.settings)) {
			TINROM_LOG("settings: card %u's are from another version", port + 1);
			last = TINROM_ERR_CORRUPT;
			continue;
		}

		*settings    = copy.settings;
		settingsCard = (int) port;

		size_t              switchLength;
		const StoredSwitch *stored2 = tinromSaveSection(TINROM_SECTION_SWITCH,
			&switchLength);
		StoredSwitch        sw;
		bool                haveSwitch = stored2
			&& (switchLength == sizeof(StoredSwitch));

		if (haveSwitch)
			memcpy(&sw, stored2, sizeof(sw));

		haveSwitch = haveSwitch && (sw.version == SWITCH_VERSION)
			&& (sw.modules == copy.modules);
		tinromProfilesImport(settings, haveSwitch ? &sw.store : NULL);

		if (copy.modules != moduleHash()) {
			TINROM_LOG("settings: other modules then; module choices reset");
			tinromProfilesForgetModules(settings);
			settings->checksum = tinromSettingsChecksum(settings);
		}

		TINROM_LOG("settings: from card %u", port + 1);

		return TINROM_OK;
	}

	if (last == TINROM_ERR_NOT_PRESENT)
		TINROM_LOG("settings: none saved, using defaults");

	return last;
}

TinromResult tinromSettingsPutSection(const TinromSettings *settings) {
	StoredSettings stored = { .modules = moduleHash() };
	StoredSwitch   sw     = { SWITCH_VERSION, stored.modules, { 0 } };

	/* The switch-off profile goes where every build looks for settings. */
	tinromProfilesExport(settings, &stored.settings, &sw.store);
	stored.settings.checksum = tinromSettingsChecksum(&stored.settings);

	TinromResult result = tinromSaveSetSection(TINROM_SECTION_SETTINGS,
		&stored, sizeof(stored));

	if (result == TINROM_OK)
		result = tinromSaveSetSection(TINROM_SECTION_SWITCH, &sw, sizeof(sw));

	return result;
}

void tinromSettingsSetCard(unsigned int port) {
	settingsCard = (port < 2) ? (int) port : -1;
}

TinromResult tinromSettingsSaveToCard(
	const TinromSettings *settings, unsigned int port
) {
	if (port > 1)
		return TINROM_ERR_BAD_ARG;
	if (!tinromSaveCardPresent(port))
		return TINROM_ERR_NOT_PRESENT;

	/* Whatever else the file holds - bookmarks, say - stays. */
	TinromResult result = tinromSaveLoad(port, &tinromSaveSettings);

	if ((result != TINROM_OK) && (result != TINROM_ERR_NOT_PRESENT)
		&& (result != TINROM_ERR_CORRUPT))
		return result;

	result = tinromSettingsPutSection(settings);

	if (result == TINROM_OK)
		result = tinromSaveStore(port, &tinromSaveSettings);

	if (result == TINROM_OK) {
		settingsCard = (int) port;
		TINROM_LOG("settings: saved to card %u", port + 1);
	}

	return result;
}

TinromResult tinromSettingsSave(
	const TinromSettings *settings, TinromSettingsStore store
) {
	if (store != TINROM_STORE_MEMCARD)
		return TINROM_ERR_UNIMPLEMENTED;

	int port = tinromSettingsSaveCard();

	if (port < 0)
		return TINROM_ERR_NOT_PRESENT;

	return tinromSettingsSaveToCard(settings, (unsigned) port);
}
