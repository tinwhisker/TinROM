/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PCDRV: the host PC's filesystem, over the serial cable.
 *
 * Two halves, and both work.
 *
 * tinrom as a CLIENT works: core/pcdrv.c speaks the protocol, so any module
 * can open, read and write files on the host - flashing a cart image straight
 * off the PC, writing a card dump back to it, and so on.
 *
 * tinrom as a SERVER for a running program now works too: hal/exception.c
 * owns 80000080h while installed and dispatches break instructions 101h..107h
 * straight into tinromPcdrvCall(), putting V0 and V1 back into the saved
 * frame so the program gets them on the way out.
 *
 * See docs/pcdrv.md.
 */

#include "core/pcdrv.h"
#include "hal/exception.h"
#include "modules/modules.h"

#define PROBE_FILE "TINROM.TXT"

static void checkHost(TinromContext *ctx) {
	TinromPcdrvResult result = tinromPcdrvCall(TINROM_PCDRV_INIT, 0, 0, 0);

	if (!tinromPcdrvHostPresent()) {
		uiMessageBox(ctx, "No host",
			"Nothing answered on the serial link.\n\n"
			"On the PC:\n\n"
			"  python -m tinrom_host pcdrv <port> <directory>\n\n"
			"PCDRV is the one thing here the console asks for "
			"rather than the host, so the host has to be "
			"listening before you ask.");

		return;
	}

	if (result.v0 != 0) {
		uiMessageBox(ctx, "Host refused",
			"A host answered but would not initialise.");

		return;
	}

	/* Answering is one thing; actually serving a file is another. */
	int32_t handle = 0;

	TinromResult opened = tinromPcdrvOpen(
		PROBE_FILE, TINROM_PCDRV_O_RDONLY, &handle
	);

	char body[224];

	if (opened == TINROM_OK) {
		uint32_t length = 0;

		(void) tinromPcdrvLength(handle, &length);
		(void) tinromPcdrvClose(handle);

		snprintf(body, sizeof(body),
			"Host is serving.\n\n"
			"  " PROBE_FILE " opened, %u bytes\n\n"
			"Reads and writes from tinrom's own modules will "
			"work, and a running program's calls too once the "
			"trap is armed.",
			(unsigned int) length);
	} else {
		snprintf(body, sizeof(body),
			"Host is listening.\n\n"
			"  " PROBE_FILE " was not there (%s)\n\n"
			"That is only the probe file. Put one in the served "
			"directory if you want this screen to confirm a "
			"read as well as a connection.",
			tinromResultString(opened));
	}

	uiMessageBox(ctx, "Host filesystem", body);
}

static void status(TinromContext *ctx) {
	char body[256];

	snprintf(body, sizeof(body),
		"client\tworking, core/pcdrv.c\n"
		"trap\t%s\n"
		"served\t%u break(s) so far\n"
		"host\t%s\n"
		"link\tSIO1, shared with the serial console\n\n"
		"The seven calls are PCInit, PCCreat, PCOpen, "
		"PCClose, PCRead, PCWrite and PClSeek - break "
		"instructions 101h..107h.",
		tinromExceptionInstalled() ? "armed" : "not armed",
		(unsigned int) tinromExceptionBreakCount(),
		tinromPcdrvHostPresent() ? "answered at least once" : "not seen yet");

	uiMessageBox(ctx, "Host filesystem", body);
}

static void arm(TinromContext *ctx) {
	TinromResult result = tinromExceptionInstall();

	if (result != TINROM_OK) {
		uiShowResult(ctx, "Arming the trap", result);

		return;
	}

	uiMessageBox(ctx, "Trap armed",
		"tinrom owns 80000080h and will service break "
		"instructions 101h..107h from anything running.\n\n"
		"It does not chain to the kernel's handler, so "
		"any other exception now stops the console with "
		"a report rather than reaching the kernel. "
		"Disarming gives the vector back.");
}

/*
 * Proving the trap works needs an exception tinrom can service, or the
 * console stops - which is correct behaviour and a poor demonstration. A
 * PCDRV break is the one exception that returns.
 */
static void testTrap(TinromContext *ctx) {
	if (!tinromExceptionInstalled()) {
		uiMessageBox(ctx, "Not armed",
			"Arm the trap first, or this will reach the "
			"kernel's handler instead and stop the console.");

		return;
	}

	uint32_t before = tinromExceptionBreakCount();

	/*
	 * PCInit with no host listening: the handler traps it, PCDRV times out,
	 * and the break returns -1 through V0. What is being tested is the
	 * trapping and the return, not the file server.
	 */
	__asm__ volatile("break 0x101" ::: "memory");

	uint32_t after = tinromExceptionBreakCount();

	char body[192];

	snprintf(body, sizeof(body),
		"%s\n\n"
		"  serviced before\t%u\n"
		"  serviced after\t%u\n\n"
		"If you are reading this at all, execution resumed "
		"after a break instruction - which is the whole test.",
		(after > before) ? "The trap caught it." : "It was not caught.",
		(unsigned int) before, (unsigned int) after);

	uiMessageBox(ctx, "Trap test", body);
}

static void disarm(TinromContext *ctx) {
	if (!tinromExceptionInstalled()) {
		uiMessageBox(ctx, "Not armed", "The kernel has 80000080h already.");

		return;
	}

	TinromResult result = tinromExceptionRemove();

	if ((result != TINROM_OK) || tinromExceptionInstalled()) {
		uiShowResult(ctx, "Disarming the trap", result);

		return;
	}

	uiMessageBox(ctx, "Trap disarmed",
		"The kernel has 80000080h back. A running program's "
		"PCDRV calls now reach the kernel, which does not "
		"serve them, and a debugger that connects arms the "
		"trap again.");
}

static TinromResult pcdrvEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Check the host",   "Initialise, and try to open a file", true, 0 },
		{ "Status",           "What works and what does not", true, 1 },
		{ "Arm the trap",     "Serve PCDRV to a running program", true, 2 },
		{ "Test the trap",    "Make a PCDRV call and come back", true, 3 },
		{ "Disarm the trap",  "Give the vector back to the kernel", true, 4 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Host filesystem", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (items[choice].tag) {
			case 0:
				checkHost(ctx);
				break;

			case 1:
				status(ctx);
				break;

			case 2:
				arm(ctx);
				break;

			case 3:
				testTrap(ctx);
				break;

			default:
				disarm(ctx);
				break;
		}
	}
}

const TinromModule tinromModule_pcdrv = {
	.id      = "pcdrv",
	.name    = "Host filesystem",
	.summary = "Read and write files on the PC over serial",
	.minTier = TINROM_TIER_DEVELOPER,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = pcdrvEnter,
	.tick    = NULL,
	.shutdown = NULL
};
