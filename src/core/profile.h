/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Two sets of choices, picked by the cart's switch: one for each position.
 *
 * A set - a "profile" - is what decides which tinrom you get: the feature
 * level, which modules are on, and what to boot straight into. The rest of
 * the settings (video, serial, ...) are the console's, and stay one set.
 *
 * TinromSettings holds the profile in use, as it always has, so nothing
 * that reads settings needs to know about this. This file keeps the other
 * one, swaps them over when the switch says, and splits them back out for
 * saving: the switch-off profile in TinromSettings, where a build without
 * any of this reads it, and the rest in a section of its own.
 *
 * Which register holds the switch is the user's to say (hal/exp.h). With
 * none set there is one profile, and everything is as it was.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/settings.h"
#include "hal/exp.h"

typedef enum {
	TINROM_PROFILE_OFF = 0,
	TINROM_PROFILE_ON  = 1
} TinromProfilePosition;

typedef struct {
	uint8_t  tier;            /* TinromTier */
	uint8_t  autobootModule;  /* registry index, or 0xFF */
	uint8_t  autobootDelay;
	uint8_t  reserved;
	uint32_t moduleEnableMask;
} TinromProfile;

/* What is saved beside the settings. */
typedef struct {
	uint8_t       source;     /* TinromSwitchSource */
	uint8_t       live;       /* swap in the main menu when flipped */
	uint8_t       reserved[2];
	TinromProfile on;         /* the switch-off one is in TinromSettings */
} TinromProfileStore;

#ifdef __cplusplus
extern "C" {
#endif

/** Both positions as `settings` has them; no switch; the off one in use. */
void tinromProfilesReset(const TinromSettings *settings);

/** After loading `settings`: `store` is NULL if there was none saved. */
void tinromProfilesImport(const TinromSettings *settings,
	const TinromProfileStore *store);
/** For saving: `off` gets `active` with the switch-off profile in it. */
void tinromProfilesExport(const TinromSettings *active, TinromSettings *off,
	TinromProfileStore *store);
/** A build with other modules saved them: redo the module choices of both. */
void tinromProfilesForgetModules(TinromSettings *settings);

TinromSwitchSource    tinromSwitchSource(void);
/**
 * Changes where the switch is read. Turning it on reads it and uses that
 * position's profile; turning it off keeps the profile in use as the one.
 */
void                  tinromSwitchSetSource(TinromSettings *settings,
	TinromSwitchSource source);
bool                  tinromSwitchLive(void);
void                  tinromSwitchSetLive(bool live);

/** The profile `settings` holds: which position's it is. */
TinromProfilePosition tinromProfileActive(void);
/** Puts `position`'s profile into `settings`, keeping the current one. */
void                  tinromProfileUse(TinromSettings *settings,
	TinromProfilePosition position);
/** The other position gets a copy of the profile in use. */
void                  tinromProfileCopyToOther(const TinromSettings *settings);

/** At power-on: reads the switch, if there is one, and uses its profile. */
void tinromSwitchBoot(TinromSettings *settings);
/**
 * Once a frame from the main menu. With live swapping on, a few times a
 * second it reads the switch; when two readings running agree on the other
 * position it swaps, and returns true for the menu to rebuild itself.
 */
bool tinromSwitchPoll(TinromSettings *settings, uint32_t frame);

/** "switch on"/"switch off" for titles, or NULL with no switch set. */
const char *tinromSwitchLabel(void);
/** The source as words, for settings. */
const char *tinromSwitchSourceName(TinromSwitchSource source);

#ifdef __cplusplus
}
#endif
