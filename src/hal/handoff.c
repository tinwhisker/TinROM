/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * See handoff.S for the stub this relocates, and handoff.h for the shape of
 * the problem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "hal/handoff.h"
#include "libc/string.h"

/* Laid down by ps1-bare-metal's linker script. */
extern uint8_t _bssEnd[];

/* handoff.S. Only their addresses matter. */
extern const uint8_t tinromHandoffStub[], tinromHandoffStubEnd[];

/*
 * Clearance above tinrom's own image for the heap, which grows up from there.
 * The stack grows down from the top of RAM and is left alone by everything
 * below.
 */
#define HEAP_CLEARANCE 0x10000u

#define ALIGN_UP(value, to) \
	(((value) + ((to) - 1)) & ~((uint32_t) (to) - 1))
#define ALIGN_DOWN(value, to) ((value) & ~((uint32_t) (to) - 1))

uint32_t tinromHandoffProtectedEnd(void) {
	return ALIGN_UP(
		(uint32_t) (uintptr_t) _bssEnd + HEAP_CLEARANCE, TINROM_HANDOFF_GRANULE
	);
}

TinromResult tinromHandoffPlan(
	uint32_t destination, uint32_t length, TinromHandoffPlan *plan
) {
	if (!plan || !length)
		return TINROM_ERR_BAD_ARG;

	memset(plan, 0, sizeof(*plan));

	uint32_t protectedEnd = tinromHandoffProtectedEnd();
	uint32_t total        = ALIGN_UP(length, TINROM_HANDOFF_GRANULE);
	uint32_t end          = destination + total;

	if (end > TINROM_HANDOFF_STUB_ADDR)
		return TINROM_ERR_NO_SPACE;

	plan->destination = destination;
	plan->totalLength = total;

	/*
	 * Nothing loads below tinrom - 80010000h is the lowest address a payload
	 * can use - so the part that collides is always a prefix.
	 */
	uint32_t staged = 0;

	if (destination < protectedEnd) {
		staged = protectedEnd - destination;

		if (staged > total)
			staged = total;

		staged = ALIGN_UP(staged, TINROM_HANDOFF_GRANULE);

		if (staged > total)
			staged = total;
	}

	plan->stagedLength  = staged;
	plan->directAddress = destination + staged;
	plan->directLength  = total - staged;

	if (!staged)
		return TINROM_OK;

	/*
	 * Park the prefix as high as it will go, just under the stub. Only the
	 * collision has to fit there, not the executable, which is why a game
	 * several times tinrom's size still boots.
	 */
	plan->stagingAddress = ALIGN_DOWN(
		TINROM_HANDOFF_STUB_ADDR - staged, TINROM_HANDOFF_GRANULE
	);

	/* It must not land on the part being read straight to its destination. */
	if (plan->stagingAddress < end)
		return TINROM_ERR_NO_SPACE;

	/* Nor below tinrom's own image, which is still executing. */
	if (plan->stagingAddress < protectedEnd)
		return TINROM_ERR_NO_SPACE;

	return TINROM_OK;
}

void tinromHandoffRun(
	const TinromHandoffPlan *plan, uint32_t pc, uint32_t gp, uint32_t sp
) {
	size_t stubSize = (size_t) (tinromHandoffStubEnd - tinromHandoffStub);

	memcpy((void *) (uintptr_t) TINROM_HANDOFF_STUB_ADDR,
		tinromHandoffStub, stubSize);

	uint32_t *parameters = (uint32_t *) (uintptr_t) TINROM_HANDOFF_PARAM_ADDR;

	parameters[0] = plan->destination;
	parameters[1] = plan->stagingAddress;
	parameters[2] = plan->stagedLength;
	parameters[3] = pc;
	parameters[4] = gp;
	parameters[5] = sp ? sp : TINROM_HANDOFF_STACK;

	TINROM_LOG("handoff: %u KB staged at %08x, %u KB already placed",
		(unsigned int) (plan->stagedLength / 1024),
		(unsigned int) plan->stagingAddress,
		(unsigned int) (plan->directLength / 1024));

#ifdef __mips__
	/*
	 * Enter through the uncached mirror. The stub has just been written with
	 * stores, so its cached alias may still hold whatever used to be there.
	 */
	void (*entry)(uint32_t *) = (void (*)(uint32_t *)) (uintptr_t)
		(TINROM_HANDOFF_STUB_ADDR | 0x20000000u);

	entry(parameters);
#endif

	for (;;)
		__asm__ volatile("");
}
