/* tinrom - host test for the handoff planner.
 *
 * Runs the real tinromHandoffPlan() against a fake linker layout, so the
 * split between "stage this" and "read this straight there" can be checked
 * without a console. See scripts/test-handoff.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "hal/handoff.h"

/* Stands in for the linker symbol. tinrom's image ends here. */
uint8_t *fake_bssEnd = (uint8_t *) 0x80031000;

static int failures;

static void check(const char *what, int ok) {
	printf("      %-44s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

static void report(const char *name, uint32_t dest, uint32_t size) {
	TinromHandoffPlan plan;
	TinromResult r = tinromHandoffPlan(dest, size, &plan);

	printf("  %s: %08x + %u KB\n", name, dest, size / 1024);

	if (r != TINROM_OK) {
		printf("      refused: %s\n", tinromResultString(r));
		return;
	}

	printf("      stage  %u KB at %08x\n",
		plan.stagedLength / 1024, plan.stagingAddress);
	printf("      direct %u KB at %08x\n",
		plan.directLength / 1024, plan.directAddress);

	check("staged + direct covers the payload",
		plan.stagedLength + plan.directLength >= size);
	check("split is sector aligned",
		!(plan.stagedLength % 2048) && !(plan.directLength % 2048));
	check("direct part starts past tinrom",
		!plan.directLength
			|| plan.directAddress >= tinromHandoffProtectedEnd());
	if (plan.stagedLength) {
		check("staging clears the payload's own tail",
			plan.stagingAddress >= dest + plan.totalLength);
		check("staging clears tinrom",
			plan.stagingAddress >= tinromHandoffProtectedEnd());
		check("staging clears the stub",
			plan.stagingAddress + plan.stagedLength
				<= TINROM_HANDOFF_STUB_ADDR);
	}
}

int main(void) {
	printf("tinrom image ends at %08x (protected to %08x)\n\n",
		(unsigned int) (uintptr_t) fake_bssEnd,
		tinromHandoffProtectedEnd());

	report("the case that was refused", 0x80010000, 282 * 1024);
	report("small game, wholly inside tinrom", 0x80010000, 64 * 1024);
	report("exactly up to the protected end", 0x80010000, 196 * 1024);
	report("large game", 0x80010000, 1024 * 1024);
	report("very large game", 0x80010000, 1600 * 1024);
	report("loads high already", 0x80100000, 256 * 1024);

	TinromHandoffPlan plan;
	printf("  refusals\n");
	check("payload reaching the stub is refused",
		tinromHandoffPlan(0x80010000, 1980 * 1024, &plan)
			== TINROM_ERR_NO_SPACE);
	check("zero length is refused",
		tinromHandoffPlan(0x80010000, 0, &plan) == TINROM_ERR_BAD_ARG);

	printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
	return failures ? 1 : 0;
}
