/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Loading the resident monitor and talking to it. See hal/debugger.h.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "common/cache.h"
#include "core/log.h"
#include "core/pcdrv.h"
#include "hal/dbgcon.h"
#include "hal/debugger.h"
#include "libc/string.h"

/*
 * The monitor, built on its own and embedded by cmake/monitor.cmake - unless
 * this build leaves it out (TINROM_DEBUGGER, TINROM_MCR_DEBUGGER in
 * cmake/options.cmake). Then there is no blob to name, tinromDebugLoad()
 * says so, and everything else here sees no monitor and does nothing.
 */
#ifndef TINROM_WITH_DEBUGGER
#define TINROM_WITH_DEBUGGER 1
#endif

#if TINROM_WITH_DEBUGGER
extern const uint8_t  monitorBlob[];
extern const uint32_t monitorBlobSize;
#endif

/* ps1-bare-metal's linker script. */
extern uint8_t _textStart[];
extern uint8_t _bssEnd[];
extern uint8_t _gp[];

static TinromMonitorHeader *monitor;

/* Why there is no monitor, for screens to say without sending anyone to the
 * log. Empty while one is loaded. */
static char problem[96] = "tinromDebugLoad() has not run";

static void setProblem(const char *format, ...) {
	va_list args;

	va_start(args, format);
	vsnprintf(problem, sizeof(problem), format, args);
	va_end(args);

	TINROM_LOG("dbg: %s", problem);
}

const char *tinromDebugProblem(void) {
	return monitor ? "" : problem;
}

#if TINROM_WITH_DEBUGGER
static uint32_t imageSum;

/*
 * The kernel's four words at 80000080h, kept here as well as in the monitor.
 * A launch can overwrite the monitor (it sits where large payloads and the
 * staging area land), and the vector still has to be given back after that.
 */
#define VECTOR_GENERAL 0x80000080u
#define VECTOR_WORDS   4

static uint32_t kernelVector[VECTOR_WORDS];

static uint32_t sumImage(void) {
	const uint32_t *words = (const uint32_t *) TINROM_MONITOR_BASE;
	uint32_t        sum   = 0;

	for (uint32_t i = 0; i < (monitorBlobSize / 4); i++)
		sum += words[i];

	return sum;
}

/*
 * PCDRV breaks are still tinrom's to service: the file server lives here, not
 * in the monitor. The monitor only calls this while tinrom is intact.
 */
static int pcdrvHook(TinromExceptionFrame *frame, uint32_t function) {
	TinromPcdrvResult result = tinromPcdrvCall(
		function, frame->r[5], frame->r[6], frame->r[7]
	);

	frame->r[2] = (uint32_t) result.v0;
	frame->r[3] = (uint32_t) result.v1;

	return 1;
}

#endif

TinromResult tinromDebugLoad(void) {
#if !TINROM_WITH_DEBUGGER
	setProblem("this build has no resident monitor (TINROM_DEBUGGER=OFF)");

	return TINROM_ERR_NOT_PRESENT;
#else
	TinromMonitorHeader *header = TINROM_MONITOR;

	/*
	 * Coming back into tinrom with an older copy still holding the vector
	 * (a reset through the menu, say): give the vector back before the
	 * handler it points at is overwritten.
	 */
	if (
		(header->magic == TINROM_MONITOR_MAGIC)
		&& (header->version == TINROM_MONITOR_VERSION)
		&& header->state && header->state->installed
	)
		header->remove();

	if ((monitorBlobSize < sizeof(TinromMonitorHeader))
		|| (monitorBlobSize > TINROM_MONITOR_SIZE)) {
		setProblem("the monitor blob is %u bytes, more than its %u",
			(unsigned int) monitorBlobSize,
			(unsigned int) TINROM_MONITOR_SIZE);

		return TINROM_ERR_UNSUPPORTED;
	}

	volatile uint32_t *vector = (volatile uint32_t *) VECTOR_GENERAL;

	for (unsigned int i = 0; i < VECTOR_WORDS; i++)
		kernelVector[i] = vector[i];

	memcpy((void *) TINROM_MONITOR_BASE, monitorBlob, monitorBlobSize);

	uint32_t imageSize = (uint32_t) (header->imageEnd - (uint8_t *) TINROM_MONITOR_BASE);
	uint32_t totalSize = (uint32_t) (header->bssEnd   - (uint8_t *) TINROM_MONITOR_BASE);

	if (
		(header->magic != TINROM_MONITOR_MAGIC)
		|| (header->version != TINROM_MONITOR_VERSION)
		|| (imageSize != monitorBlobSize)
		|| (totalSize > TINROM_MONITOR_SIZE)
	) {
		/* Every value, since which one is wrong is the whole story. */
		setProblem("copied to %08x it read back as magic %08x version %u, "
			"%u+%u bytes; wanted version %u, %u bytes",
			(unsigned int) TINROM_MONITOR_BASE, (unsigned int) header->magic,
			(unsigned int) header->version, (unsigned int) imageSize,
			(unsigned int) (totalSize - imageSize),
			(unsigned int) TINROM_MONITOR_VERSION,
			(unsigned int) monitorBlobSize);

		/* Make sure nothing mistakes the leftovers for a monitor later. */
		header->magic = 0;

		return TINROM_ERR_UNSUPPORTED;
	}

	memset(header->imageEnd, 0, totalSize - imageSize);

	/* It is code: it must not be served stale from the instruction cache. */
	flushCache();

	TinromMonitorState *state = header->state;

	state->tinromStart  = (uint32_t) (uintptr_t) _textStart & 0x1fffffffu;
	state->tinromEnd    = (uint32_t) (uintptr_t) _bssEnd    & 0x1fffffffu;
	state->tinromIntact = 1;
	state->pcdrv        = pcdrvHook;
	state->tinromGp     = (uint32_t) (uintptr_t) _gp;
	state->dbgcon       = tinromDbgConsolePresent() ? 1 : 0;

	monitor  = header;
	imageSum = sumImage();

	TINROM_LOG("dbg: monitor at %08x, %u+%u bytes",
		(unsigned int) TINROM_MONITOR_BASE, (unsigned int) imageSize,
		(unsigned int) (totalSize - imageSize));

	return TINROM_OK;
#endif
}

bool tinromDebugRemoveHandler(void) {
#if TINROM_WITH_DEBUGGER
	if (!monitor)
		return false;

	/*
	 * The data words change as it runs, so this is a sum over the whole
	 * image only because the monitor keeps no initialised data: everything
	 * that changes is in .bss, which is not summed.
	 */
	if ((monitor->magic == TINROM_MONITOR_MAGIC) && (sumImage() == imageSum)) {
		if (!monitor->state->installed)
			return false;

		monitor->remove();

		return true;
	}

	/* Overwritten: put the kernel's words back without its help. */
	volatile uint32_t *vector = (volatile uint32_t *) VECTOR_GENERAL;

	for (unsigned int i = 0; i < VECTOR_WORDS; i++)
		vector[i] = kernelVector[i];

	flushCache();
	monitor = NULL;
	setProblem("the monitor at %08x was overwritten after loading",
		(unsigned int) TINROM_MONITOR_BASE);

	return true;
#else
	return false;
#endif
}

void tinromDebugRelease(void) {
	tinromDebugRemoveHandler();

	/*
	 * Forget it: whatever is loaded next lands on top of it, and from then on
	 * its header and state are someone else's data. Nothing may follow its
	 * pointers again - not even to ask whether it is installed.
	 */
	if (monitor)
		setProblem("the monitor was released for a program load");

	monitor = NULL;
}

bool tinromDebugAvailable(void) {
	return monitor != NULL;
}

const TinromMonitorState *tinromDebugState(void) {
	return monitor ? monitor->state : NULL;
}

void tinromDebugArm(bool armed) {
	if (!monitor)
		return;

	if (armed) {
		monitor->install();
		monitor->state->armed = 1;

		return;
	}

	monitor->state->armed           = 0;
	monitor->state->attachOnContact = 0;
	monitor->clearBreakpoints();
}

bool tinromDebugArmed(void) {
	return monitor && monitor->state->armed;
}

void tinromDebugSetAttachOnContact(bool enabled) {
	if (!monitor)
		return;

	if (enabled)
		tinromDebugArm(true);

	monitor->state->attachOnContact = enabled ? 1 : 0;
}

bool tinromDebugAttachOnContact(void) {
	return monitor && monitor->state->attachOnContact;
}

void tinromDebugHalt(TinromBreakIn reason, uint8_t byte) {
	if (!monitor)
		return;

	monitor->state->attachOnContact = 0;
	tinromDebugArm(true);
	monitor->breakIn((uint32_t) reason, byte);
}

bool tinromDebugNopsCommand(uint32_t token) {
	return monitor && monitor->nopsCommand(token);
}

void tinromDebugClearBreakpoints(void) {
	if (monitor)
		monitor->clearBreakpoints();
}

unsigned int tinromDebugBreakpointCount(void) {
	if (!monitor)
		return 0;

	unsigned int count = 0;

	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		if (monitor->state->breakpoints[i].kind != TINROM_BP_FREE)
			count++;
	}

	return count;
}
