/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The exception handler's public face. The handler itself - the vector, the
 * register save and the dispatcher - lives in the resident monitor
 * (src/monitor), because it has to survive a program being loaded over
 * tinrom by a debugger. This file keeps the API the rest of tinrom already
 * used, and answers it from the monitor's state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "hal/debugger.h"
#include "hal/exception.h"

TinromResult tinromExceptionInstall(void) {
	if (!tinromDebugAvailable())
		return TINROM_ERR_NOT_PRESENT;

	TinromMonitorHeader *monitor = TINROM_MONITOR;
	bool                 was     = monitor->state->installed;

	monitor->install();

	if (!was)
		TINROM_LOG("exc: installed, handler in the monitor at %08x",
			(unsigned int) TINROM_MONITOR_BASE);

	return TINROM_OK;
}

TinromResult tinromExceptionRemove(void) {
	if (!tinromDebugAvailable())
		return TINROM_OK;

	/*
	 * Not gated on tinromExceptionInstalled(), and that is not asked first:
	 * after a load has overwritten the monitor its state pointer is someone
	 * else's data, and following it reads wherever that data points.
	 * tinromDebugRemoveHandler() checks the image before trusting anything.
	 */
	if (tinromDebugRemoveHandler())
		TINROM_LOG("exc: removed");

	return TINROM_OK;
}

bool tinromExceptionInstalled(void) {
	const TinromMonitorState *state = tinromDebugState();

	return state && state->installed;
}

uint32_t tinromExceptionBreakCount(void) {
	const TinromMonitorState *state = tinromDebugState();

	return state ? state->breakCount : 0;
}
