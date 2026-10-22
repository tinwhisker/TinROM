/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The cart's crash screen, from tinrom's side: finding it, and pointing the
 * kernel at it before a game gets the console.
 *
 * The screen itself is src/crash, a separate link that runs out of the
 * cart's ROM. A game that crashes - an exception nothing handled, which the
 * kernel answers by calling A(40h) SystemErrorUnresolvedException and
 * hanging - gets it instead of a frozen picture. It needs a tinrom cart in
 * the slot, built with TINROM_ROM_CRASH_SCREEN; whether this tinrom came
 * from that cart, a disc or a memory card makes no difference. See
 * docs/exceptions.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The crash screen's entry point on the cart in EXP1, after checking its
 * header against the cart's descriptor; 0 if there is no tinrom cart, or
 * one without a crash screen.
 */
uint32_t     tinromCrashScreenEntry(void);
/**
 * Points the kernel's A(40h) at the crash screen. False, and nothing
 * changed, without one - or when the kernel's table does not look like the
 * kernel's. Called by tinromHalShutdown(), so every handoff does it.
 */
bool         tinromCrashScreenArm(void);
/**
 * Crashes the console on purpose, through the kernel, to show the screen a
 * game would get: arms it, gives the kernel back its exception vector and
 * makes an unaligned load. Returns only when there is no crash screen.
 */
TinromResult tinromCrashScreenTest(void);

#ifdef __cplusplus
}
#endif
