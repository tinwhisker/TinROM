/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include "core/settings.h"
#include "hal/hal.h"
#include "ps1/registers.h"

#ifndef TINROM_DEFAULT_BAUD
#define TINROM_DEFAULT_BAUD 115200
#endif

void tinromHalInit(const TinromSettings *settings) {
	/*
	 * Mask every interrupt before touching anything. tinrom polls; it does
	 * not want the BIOS kernel's handlers running behind its back.
	 */
	IRQ_MASK = 0;
	IRQ_STAT = 0;

	/*
	 * First, so that everything below can log. Costs one register write and
	 * one read when there is no emulator listening.
	 */
	tinromDbgConsoleInit();

	/*
	 * The resident monitor, which owns the exception handler. Copied into
	 * place before anything could want to install it; it does nothing until
	 * something does.
	 */
	tinromDebugLoad();

	tinromTimerInit();
	tinromVideoInit((TinromVideoPreference) settings->videoPreference);

	/* One bus, two drivers: bring it up once, before either of them. */
	tinromSio0Init();
	tinromPadInit();
	tinromMcInit();
	tinromExpInit();

	uint32_t baud = TINROM_DEFAULT_BAUD;

	if (settings->serialBaudIndex < tinromSioBaudCount)
		baud = tinromSioBaudTable[settings->serialBaudIndex].baud;

	tinromSioInit(baud);

	/* CD init is allowed to fail: there may be no disc, or no drive. */
	tinromCdInit();
}

void tinromHalShutdown(void) {
	/*
	 * The vector goes back first. Whatever runs next has its own idea of what
	 * lives at 80000080h, and tinrom's handler is about to be overwritten by
	 * it - a trampoline pointing into a loaded game is the worst kind of
	 * crash, because it looks like the game's fault.
	 */
	tinromExceptionRemove();

	/*
	 * And a game that crashes gets the cart's crash screen rather than a
	 * frozen picture, if there is a cart with one in the slot.
	 */
	tinromCrashScreenArm();

	/* STUB: should also quiesce DMA and the GPU. */
}

void tinromHalIdle(void) {
	tinromWaitVSync();
}
