/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Handing the console to a loaded executable.
 *
 * The awkward part is that tinrom links at 80010000h and so does almost every
 * PS1 game, so the payload's destination and tinrom's own image are the same
 * memory. Only the part of the payload that actually lands on tinrom needs to
 * be kept out of the way; the rest can be read straight to where it belongs.
 * That is what a plan describes.
 *
 *   destination            protected end                 staging      stub
 *   |                      |                             |            |
 *   +-- staged prefix -----+---- read directly ----+     +-- prefix --+
 *   80010000               80041000                                801f0000
 *
 * Staging therefore only ever has to hold as much as tinrom occupies, not the
 * whole executable. See docs/cd-boot.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

/* The relocated stub and its parameters, at the top of RAM. */
#define TINROM_HANDOFF_STUB_ADDR  0x801f0000u
#define TINROM_HANDOFF_PARAM_ADDR 0x801f0200u
#define TINROM_HANDOFF_STACK      0x801ffff0u

/** Everything is split on sector boundaries so a loader can read into it. */
#define TINROM_HANDOFF_GRANULE 2048u

typedef struct {
	uint32_t destination;    /* where the whole payload belongs */
	uint32_t totalLength;

	/** Front of the payload that lands on tinrom, so must be parked first. */
	uint32_t stagedLength;
	uint32_t stagingAddress;

	/** The rest, which can be read straight to its final address. */
	uint32_t directAddress;
	uint32_t directLength;
} TinromHandoffPlan;

#ifdef __cplusplus
extern "C" {
#endif

/** First address a payload may be written to without hitting tinrom. */
uint32_t tinromHandoffProtectedEnd(void);

/**
 * Works out how to get `length` bytes to `destination` without tinrom
 * overwriting itself mid-copy. Does nothing irreversible, so a caller can
 * refuse with a useful message instead of locking the console.
 *
 * TINROM_ERR_NO_SPACE  the payload is large enough to reach the staging area
 * TINROM_ERR_BAD_ARG   the destination is not somewhere a payload can go
 */
TinromResult tinromHandoffPlan(
	uint32_t destination, uint32_t length, TinromHandoffPlan *plan
);

/**
 * Copies the staged prefix into place and enters `pc`. Does not return, and
 * cannot fail once started. The caller must already have loaded both parts
 * where the plan says.
 */
void tinromHandoffRun(
	const TinromHandoffPlan *plan, uint32_t pc, uint32_t gp, uint32_t sp
);

#ifdef __cplusplus
}
#endif
