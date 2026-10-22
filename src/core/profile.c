/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The switch's two profiles. See profile.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include "core/log.h"
#include "core/profile.h"
#include "core/settings.h"
#include "core/tier.h"
#include "hal/exp.h"
#include "libc/string.h"

/* Four readings a second: quick to follow a flip, nothing to the bus. */
#define POLL_FRAMES 15

static TinromProfile         profiles[2];
static TinromProfilePosition active;
static TinromSwitchSource    source;
static bool                  live;
static int                   lastReading = -1;
static uint32_t              lastPoll;

static TinromProfile profileOf(const TinromSettings *settings) {
	TinromProfile profile = {
		.tier             = (uint8_t) settings->tier,
		.autobootModule   = settings->autobootModule,
		.autobootDelay    = settings->autobootDelay,
		.moduleEnableMask = settings->moduleEnableMask
	};

	return profile;
}

static void apply(TinromSettings *settings, const TinromProfile *profile) {
	settings->tier = (profile->tier < TINROM_TIER_COUNT)
		? (TinromTier) profile->tier : TINROM_TIER_BASIC;
	settings->autobootModule   = profile->autobootModule;
	settings->autobootDelay    = profile->autobootDelay;
	settings->moduleEnableMask = profile->moduleEnableMask;
}

void tinromProfilesReset(const TinromSettings *settings) {
	profiles[TINROM_PROFILE_OFF] = profileOf(settings);
	profiles[TINROM_PROFILE_ON]  = profileOf(settings);
	active      = TINROM_PROFILE_OFF;
	source      = TINROM_SWITCH_NONE;
	live        = false;
	lastReading = -1;
}

void tinromProfilesImport(const TinromSettings *settings,
	const TinromProfileStore *store) {
	tinromProfilesReset(settings);

	if (!store)
		return;

	source = (store->source < TINROM_SWITCH_COUNT)
		? (TinromSwitchSource) store->source : TINROM_SWITCH_NONE;
	live   = store->live != 0;
	profiles[TINROM_PROFILE_ON] = store->on;
}

void tinromProfilesExport(const TinromSettings *settings, TinromSettings *off,
	TinromProfileStore *store) {
	profiles[active] = profileOf(settings);

	*off = *settings;
	apply(off, &profiles[TINROM_PROFILE_OFF]);

	memset(store, 0, sizeof(*store));
	store->source = (uint8_t) source;
	store->live   = live ? 1 : 0;
	store->on     = profiles[TINROM_PROFILE_ON];
}

void tinromProfilesForgetModules(TinromSettings *settings) {
	for (unsigned int i = 0; i < 2; i++) {
		TinromSettings scratch = *settings;

		apply(&scratch, &profiles[i]);
		tinromSettingsApplyTier(&scratch, scratch.tier);

		/* Registry indices, renumbered like the mask. */
		scratch.autobootModule = 0xff;
		profiles[i] = profileOf(&scratch);
	}

	apply(settings, &profiles[active]);
}

TinromSwitchSource tinromSwitchSource(void) {
	return source;
}

bool tinromSwitchLive(void) {
	return live;
}

void tinromSwitchSetLive(bool enabled) {
	live = enabled;
}

TinromProfilePosition tinromProfileActive(void) {
	return active;
}

void tinromProfileUse(TinromSettings *settings,
	TinromProfilePosition position) {
	profiles[active] = profileOf(settings);
	active           = position;
	apply(settings, &profiles[position]);
}

void tinromProfileCopyToOther(const TinromSettings *settings) {
	profiles[active]  = profileOf(settings);
	profiles[!active] = profiles[active];
}

static TinromProfilePosition read(void) {
	return tinromExpSwitchOn(source) ? TINROM_PROFILE_ON : TINROM_PROFILE_OFF;
}

void tinromSwitchSetSource(TinromSettings *settings,
	TinromSwitchSource wanted) {
	if (wanted >= TINROM_SWITCH_COUNT)
		wanted = TINROM_SWITCH_NONE;

	if (wanted == TINROM_SWITCH_NONE) {
		/* What is on screen is what stays: it becomes the only profile. */
		profiles[active]             = profileOf(settings);
		profiles[TINROM_PROFILE_OFF] = profiles[active];
		active = TINROM_PROFILE_OFF;
		source = TINROM_SWITCH_NONE;
		return;
	}

	source = wanted;

	TinromProfilePosition now = read();

	lastReading = (int) now;
	tinromProfileUse(settings, now);
}

void tinromSwitchBoot(TinromSettings *settings) {
	if (source == TINROM_SWITCH_NONE)
		return;

	TinromProfilePosition now = read();

	lastReading = (int) now;
	active      = now;
	apply(settings, &profiles[now]);

	TINROM_LOG("switch: %s (%s), %s", (now == TINROM_PROFILE_ON) ? "on" : "off",
		tinromSwitchSourceName(source), tinromTierName(settings->tier));
}

bool tinromSwitchPoll(TinromSettings *settings, uint32_t frame) {
	if (!live || (source == TINROM_SWITCH_NONE))
		return false;
	if ((frame - lastPoll) < POLL_FRAMES)
		return false;

	lastPoll = frame;

	int  now     = (int) read();
	bool settled = (now == lastReading);

	lastReading = now;

	/* A reading of its own is not enough: the next must agree. */
	if (!settled || (now == (int) active))
		return false;

	tinromProfileUse(settings, (TinromProfilePosition) now);
	TINROM_LOG("switch: flipped %s, %s", now ? "on" : "off",
		tinromTierName(settings->tier));

	return true;
}

const char *tinromSwitchLabel(void) {
	if (source == TINROM_SWITCH_NONE)
		return NULL;

	return (active == TINROM_PROFILE_ON) ? "switch on" : "switch off";
}

const char *tinromSwitchSourceName(TinromSwitchSource which) {
	switch (which) {
		case TINROM_SWITCH_XPLORER: return "Xplorer-style";
		case TINROM_SWITCH_DATEL:   return "Action Replay-style";
		default:                    return "not read";
	}
}
