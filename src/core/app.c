/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Boot sequence and main loop. Kept deliberately small: everything
 * interesting belongs in a module.
 */

#include <stdbool.h>
#include <stddef.h>
#include "core/app.h"
#include "core/log.h"
#include "core/module.h"
#include "core/profile.h"
#include "core/result.h"
#include "core/settings.h"
#include "hal/exp.h"
#include "hal/hal.h"
#include "tinrom/version.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "ui/widgets.h"

static TinromContext context;

TinromContext *tinromContext(void) {
	return &context;
}

static void bootBanner(const TinromContext *ctx) {
	TINROM_LOG("%s %s", TINROM_NAME, tinromVersionFull);
	TINROM_LOG("built %s", tinromBuildTime);
	TINROM_LOG("boot source: %d, modules: %d",
		(int) ctx->bootSource, (int) tinromModuleCount());
}

static const TinromModule *autobootTarget(const TinromContext *ctx) {
	if (ctx->settings.autobootModule == 0xff)
		return NULL;

	size_t index = ctx->settings.autobootModule;

	if (!tinromModuleAvailable(index))
		return NULL;

	const TinromModule *module = tinromModuleAt(index);

	if (!module || !(module->flags & TINROM_MODULE_AUTOSTART))
		return NULL;

	/* Switched off in this profile, or above its level: not this time. */
	if (!tinromModuleVisible(ctx, index))
		return NULL;

	return module;
}

void tinromMain(TinromBootSource source) {
	context.bootSource = source;
	context.frame      = 0;

	/* 1. Settings first: everything below reads them. */
	tinromSettingsDefaults(&context.settings);
	tinromProfilesReset(&context.settings);

	/* 2. Hardware. Video mode depends on the settings we just loaded. */
	tinromHalInit(&context.settings);
	tinromLogInit(context.settings.ttyToSerial);

	/* A cart's claim to have started tinrom holds only if this is its tinrom. */
	if (((source == TINROM_BOOT_CART) || (source == TINROM_BOOT_PREBOOT)
		|| (source == TINROM_BOOT_MIDBOOT)) && !tinromExpRunningFromCart())
		context.bootSource = TINROM_BOOT_PSEXE;

	/*
	 * 3. Persistent settings. This needs the HAL up (the stores live on the
	 *    cart or a memory card), so the defaults above are what we ran the
	 *    HAL with. What the HAL took from them is redone where the saved
	 *    settings differ.
	 */
	if (tinromSettingsLoad(&context.settings) == TINROM_OK) {
		TinromSettings defaults;

		tinromSettingsDefaults(&defaults);
		TINROM_LOG("settings: loaded");

		if (context.settings.videoPreference != defaults.videoPreference)
			tinromVideoInit(
				(TinromVideoPreference) context.settings.videoPreference);
		if ((context.settings.serialBaudIndex != defaults.serialBaudIndex) &&
			(context.settings.serialBaudIndex < tinromSioBaudCount))
			tinromSioSetBaud(
				tinromSioBaudTable[context.settings.serialBaudIndex].baud);
		if (context.settings.ttyToSerial != defaults.ttyToSerial)
			tinromLogSetSerialMirror(context.settings.ttyToSerial);
	}

	/* The cart switch, if one is set up, picks which profile this is. */
	tinromSwitchBoot(&context.settings);

	/* 4. UI, then modules - modules may want to draw during init. */
	uiInit(&context.ui);

	/*
	 * Said before anything else can go wrong because of it. The text is in
	 * .text for the same reason as hal/exp.c's: past the window, .rodata
	 * is not ours, and this may well be drawn in a damaged font.
	 */
	if (tinromExpTruncatedWindow()) {
		static const char title[] __attribute__((section(".text.app_strings"),
			aligned(4))) = "Cart image too big";
		static const char body[] __attribute__((section(".text.app_strings"),
			aligned(4))) = "This cart image is bigger than the EXP1 window "
			"it was copied from - pcsx-redux maps only 256K - so the end of "
			"tinrom in RAM is not tinrom, and anything may fail. Build the "
			"cart with -DTINROM_ROM_SIZE=0x40000 to be told what to leave "
			"out, or boot tinrom.psexe instead.";

		uiMessageBox(&context, title, body);
	}
	tinromModulesInit(&context);

	bootBanner(&context);

	/* 5. Autoboot, if configured and not overridden by a held button. */
	const TinromModule *target = autobootTarget(&context);

	if (target && !tinromPadAbortHeld()) {
		TINROM_LOG("autoboot: %s", target->id);

		if (target->enter)
			target->enter(&context);
	}

	/* 6. Main menu until something asks to hand off. */
	while (!context.exitRequested) {
		uiMenuRunMain(&context);

		if (context.handoff) {
			tinromModulesShutdown(&context);
			context.handoff(&context);
			/* If the handoff returns, fall back into the menu. */
			context.handoff = NULL;
			tinromModulesInit(&context);
		}
	}

	tinromModulesShutdown(&context);
	tinromHalShutdown();

	/* Nothing to return to on a bare-metal target. */
	for (;;)
		tinromHalIdle();
}
