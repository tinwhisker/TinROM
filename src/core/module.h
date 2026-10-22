/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"
#include "core/tier.h"

typedef struct TinromContext TinromContext;
typedef struct TinromModule  TinromModule;

typedef enum {
	/** Show an entry for this module in the main menu. */
	TINROM_MODULE_MENU       = 1 << 0,
	/** Call tick() every frame while any other screen is up. */
	TINROM_MODULE_BACKGROUND = 1 << 1,
	/** May be selected as the autoboot target in settings. */
	TINROM_MODULE_AUTOSTART  = 1 << 2,
	/** Cannot be switched off at runtime (about, settings). */
	TINROM_MODULE_ESSENTIAL  = 1 << 3,
	/** Takes over the serial port; only one such module may be active. */
	TINROM_MODULE_OWNS_SIO   = 1 << 4
} TinromModuleFlags;

/*
 * A module is a plain struct of function pointers. There is no dynamic
 * loading and no allocation: the registry is a static array built at compile
 * time from the enabled set (see cmake/modules.cmake).
 *
 * Lifecycle:
 *   init()     once at boot, in registry order, before the UI comes up.
 *              Must be cheap and must not block. Returning an error marks the
 *              module unavailable; it stays listed but greyed out.
 *   enter()    when the user picks the menu entry. Runs its own frame loop
 *              and returns when the user backs out.
 *   tick()     once per frame while some other screen is up, for modules
 *              with TINROM_MODULE_BACKGROUND (the serial server, mostly).
 *   shutdown() before handing control to a game or another executable.
 */
struct TinromModule {
	const char *id;       /* stable, lowercase, matches the directory name */
	const char *name;     /* menu label */
	const char *summary;  /* one line shown under the menu */

	TinromTier minTier;
	uint16_t   flags;

	TinromResult (*init)(TinromContext *ctx);
	TinromResult (*enter)(TinromContext *ctx);
	void         (*tick)(TinromContext *ctx);
	void         (*shutdown)(TinromContext *ctx);
};

#ifdef __cplusplus
extern "C" {
#endif

/** Number of modules compiled into this build. */
size_t              tinromModuleCount(void);
/** Registry entry by index, or NULL. Index is stable for a given build. */
const TinromModule *tinromModuleAt(size_t index);
/** Registry entry by id, or NULL. */
const TinromModule *tinromModuleFind(const char *id);
/** Index of a module, or SIZE_MAX. */
size_t              tinromModuleIndex(const TinromModule *module);

/** True if init() succeeded for this module. */
bool tinromModuleAvailable(size_t index);
/** True if the module should be offered right now (tier + runtime enable). */
bool tinromModuleVisible(const TinromContext *ctx, size_t index);

TinromResult tinromModulesInit(TinromContext *ctx);
void         tinromModulesTick(TinromContext *ctx);
void         tinromModulesShutdown(TinromContext *ctx);

#ifdef __cplusplus
}
#endif
