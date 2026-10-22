/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * tinrom's side of the resident debug monitor (src/monitor). The monitor
 * owns the exception vector, the debugger and a serial driver of its own; it
 * is copied to TINROM_MONITOR_BASE at boot so it survives a program being
 * loaded over tinrom. See docs/debugger.md.
 *
 * Nothing here blocks except tinromDebugHalt(), which by design does not
 * return until a debugger resumes the console.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/result.h"
#include "monitor/monitor_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Copies the monitor into place and checks it. Called once by
 * tinromHalInit(), before anything can install the handler.
 */
TinromResult tinromDebugLoad(void);
/**
 * Gives 80000080h back to the kernel. Works even if a load has overwritten
 * the monitor, which the monitor's own remove() could not. True if the vector
 * was the monitor's and has been given back.
 */
bool         tinromDebugRemoveHandler(void);
/**
 * Gives the vector back and forgets the monitor entirely, for a load that is
 * about to overwrite it. Afterwards the debugger reports itself unavailable
 * until tinromDebugLoad() runs again.
 */
void         tinromDebugRelease(void);
/** False if the blob was missing, the wrong version, or would not fit. */
bool         tinromDebugAvailable(void);
/**
 * Why tinromDebugAvailable() is false, as a sentence fragment for a screen:
 * "this build has no resident monitor", say. "" while it is available.
 */
const char  *tinromDebugProblem(void);

/** Read-only view of what the monitor is doing, for status screens. */
const TinromMonitorState *tinromDebugState(void);

/**
 * Armed, a break nobody registered stops in the debugger instead of being
 * reported as a crash. Arming installs the handler; disarming drops every
 * breakpoint but leaves the handler where it is.
 */
void tinromDebugArm(bool armed);
bool tinromDebugArmed(void);

/** Halt at the first sign of a debugger: nops' DEBG or a GDB packet. */
void tinromDebugSetAttachOnContact(bool enabled);
bool tinromDebugAttachOnContact(void);

/** Halt tinrom itself in the debugger until a host resumes it. */
void tinromDebugHalt(TinromBreakIn reason, uint8_t byte);

/**
 * For the serial server: offer the monitor a nops token tinrom has read.
 * True if it was a debug command and has been answered.
 */
bool tinromDebugNopsCommand(uint32_t token);

void tinromDebugClearBreakpoints(void);
unsigned int tinromDebugBreakpointCount(void);

#ifdef __cplusplus
}
#endif
