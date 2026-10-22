/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The HAL is the only place allowed to touch hardware registers directly.
 * Modules go through these headers so that a module can be unit-reasoned
 * about, and so the CD/memory-card/cart drivers can be swapped for native
 * implementations without touching feature code.
 */
#pragma once

#include "core/result.h"
#include "core/settings.h"
#include "hal/cdrom.h"
#include "hal/crash.h"
#include "hal/dbgcon.h"
#include "hal/debugger.h"
#include "hal/exception.h"
#include "hal/exp.h"
#include "hal/mcard.h"
#include "hal/pad.h"
#include "hal/sio0.h"
#include "hal/uart_sio.h"
#include "hal/timer.h"
#include "hal/video.h"

#ifdef __cplusplus
extern "C" {
#endif

void tinromHalInit(const TinromSettings *settings);
void tinromHalShutdown(void);
/** Burn a little time without spinning the bus flat out. */
void tinromHalIdle(void);

#ifdef __cplusplus
}
#endif
