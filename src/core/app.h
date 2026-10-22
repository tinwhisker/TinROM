/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/module.h"
#include "core/result.h"
#include "core/settings.h"
#include "ui/ui.h"

/** How tinrom got control. Some modules behave differently per entry point. */
typedef enum {
	TINROM_BOOT_UNKNOWN = 0,
	TINROM_BOOT_PSEXE   = 1, /* uploaded over serial or run from an emulator */
	TINROM_BOOT_CART    = 2, /* EXP1 ROM, post-boot entry */
	TINROM_BOOT_PREBOOT = 3, /* EXP1 ROM, pre-boot entry */
	TINROM_BOOT_CD      = 4,
	TINROM_BOOT_MEMCARD = 5,
	/*
	 * Seized control from the kernel via a COP0 breakpoint on the shell
	 * entry point, armed by the cart's pre-boot handler. The kernel is fully
	 * initialised and the CD-ROM has not been touched.
	 */
	TINROM_BOOT_MIDBOOT = 6
} TinromBootSource;

struct TinromContext {
	TinromSettings   settings;
	TinromBootSource bootSource;
	UIContext        ui;

	uint32_t frame;
	bool     settingsDirty;
	bool     exitRequested;

	/*
	 * Set by a module that wants control handed to something else when the
	 * main loop unwinds: a game, an uploaded EXE, a re-entered module.
	 */
	void (*handoff)(struct TinromContext *ctx);
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The whole of tinrom after the platform is up. Called by every entry point
 * in src/boot. Does not return unless handoff is set.
 */
void tinromMain(TinromBootSource source);

/** The single global context. Modules should take it as a parameter instead. */
TinromContext *tinromContext(void);

#ifdef __cplusplus
}
#endif
