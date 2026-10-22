/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The debugger's screens. The debugger itself - the GDB remote protocol, the
 * nops debug commands, breakpoints and the stop loop - is the resident
 * monitor (src/monitor), which answers on SIO1 whether or not this module is
 * open. See docs/debugger.md.
 *
 * Two ways in, both on the same serial port:
 *   GDB directly:  (gdb) set serial baud 115200
 *                  (gdb) target remote COM3
 *   through nops:  nops /gdb 127.0.0.1:3333 /dest COM3
 *                  (gdb) target remote localhost:3333
 */

#include "modules/modules.h"

#define INFO_LINES 22
#define INFO_WIDTH 112 /* a line may wrap on screen */

static char         infoLines[INFO_LINES][INFO_WIDTH];
static unsigned int infoCount;

static void addLine(const char *format, ...) {
	if (infoCount >= INFO_LINES)
		return;

	va_list args;

	va_start(args, format);
	vsnprintf(infoLines[infoCount], sizeof(infoLines[0]), format, args);
	va_end(args);

	infoCount++;
}

static const char *infoLineAt(unsigned int index) {
	return (index < infoCount) ? infoLines[index] : "";
}

static const char *hostName(uint32_t host) {
	switch (host) {
		case TINROM_HOST_NOPS: return "nops";
		case TINROM_HOST_GDB:  return "GDB";
		default:               return "none yet";
	}
}

static const char *stopName(uint32_t reason) {
	switch (reason) {
		case TINROM_STOP_REQUEST:    return "on request";
		case TINROM_STOP_BREAKPOINT: return "breakpoint";
		case TINROM_STOP_STEP:       return "step";
		case TINROM_STOP_HOOK:       return "exec hook";
		case TINROM_STOP_BREAK:      return "break insn";
		case TINROM_STOP_FAULT:      return "fault";
		default:                     return "never";
	}
}

static const char *kindName(uint32_t kind) {
	switch (kind) {
		case TINROM_BP_GDB:  return "GDB breakpoint";
		case TINROM_BP_HOOK: return "nops exec hook";
		case TINROM_BP_STEP: return "step";
		default:             return "?";
	}
}

static void addConnectHelp(void) {
	unsigned int baud = (unsigned int) tinromSioBaud();

	addLine("GDB straight at the serial port:");
	addLine("  set serial baud %u", baud);
	addLine("  target remote COM3");
	addLine("Or through nops:");
	addLine("  nops /gdb 127.0.0.1:3333 /dest COM3");
	addLine("  target remote localhost:3333");
}

/* Draws one, wrapped to the screen, and returns how far down it went. */
static int infoLine(UIContext *ui, int y, const char *line) {
	int          x     = TINROM_MARGIN_X + 4;
	unsigned int lines = uiTextWrapped(ui, x, y, (int) ui->width - (2 * x),
		TINROM_COLOR_TEXT, line);

	return TINROM_ROW_HEIGHT + (int) ((lines - 1) * UI_FONT_LINE_HEIGHT);
}

static void buildStatus(void) {
	const TinromMonitorState *state = tinromDebugState();

	infoCount = 0;

	if (!state) {
		addLine("No debugger: %s.", tinromDebugProblem());

		return;
	}

	addLine("monitor\t%08x", (unsigned int) TINROM_MONITOR_BASE);
	addLine("handler\t%s", state->installed ? "installed" : "not installed");
	addLine("debugger\t%s%s", state->armed ? "armed" : "not armed",
		state->attachOnContact ? ", waiting" : "");
	addLine("last host\t%s", hostName(state->host));
	addLine("commands\t%u", (unsigned int) state->commands);
	addLine("stops\t%u, last %s", (unsigned int) state->stopCount,
		stopName(state->stopReason));

	if (state->stopCount)
		addLine("  at\t%08x", (unsigned int) state->stopPc);

	addLine("breakpts\t%u", tinromDebugBreakpointCount());
	addLine("tinrom\t%s", state->tinromIntact
		? "intact" : "overwritten by a load");
	addLine("");
	addConnectHelp();
}

/*
 * Live, and servicing the serial port, so a debugger can connect while it is
 * up - which halts tinrom right here.
 */
static void showStatus(TinromContext *ctx) {
	UIContext *ui = &ctx->ui;

	for (;;) {
		buildStatus();

		uiBeginFrame(ui);
		uiSetChrome(ui, "Debugger status", "Circle: back");

		int y = TINROM_HEADER_H + 8;

		for (unsigned int i = 0; i < infoCount; i++)
			y += infoLine(ui, y, infoLines[i]) - 1;

		uiEndFrame(ui);
		tinromModulesTick(ctx);

		if ((tinromPadPressed(0) | tinromPadPressed(1)) & TINROM_PAD_CIRCLE)
			return;
	}
}

/*
 * Arm, then keep the menu's serial server running until a debugger makes
 * contact. The first DEBG from nops, or the first packet from GDB, halts
 * tinrom inside tinromModulesTick() below; the screen then stays as it was
 * drawn until the debugger resumes tinrom.
 */
static void waitForDebugger(TinromContext *ctx) {
	if (!tinromDebugAvailable()) {
		char body[160];

		snprintf(body, sizeof(body), "Nothing to connect to: %s.",
			tinromDebugProblem());
		uiMessageBox(ctx, "No debugger", body);

		return;
	}

	const TinromMonitorState *state = tinromDebugState();
	uint32_t                  stops = state->stopCount;
	UIContext                *ui    = &ctx->ui;

	tinromDebugSetAttachOnContact(true);
	TINROM_LOG("gdb: waiting for a debugger on SIO1");

	for (;;) {
		infoCount = 0;
		addLine("Connect a debugger now. tinrom halts the moment it hears "
			"from one, and this screen freezes until it is resumed.");
		addLine("");
		addConnectHelp();

		uiBeginFrame(ui);
		uiSetChrome(ui, "Waiting for a debugger", "Circle: stop waiting");

		int y = TINROM_HEADER_H + 10;

		for (unsigned int i = 0; i < infoCount; i++)
			y += infoLine(ui, y, infoLines[i]);

		uiEndFrame(ui);
		tinromModulesTick(ctx);

		if (state->stopCount != stops) {
			/* It attached, and has since let tinrom go again. */
			TINROM_LOG("gdb: resumed by %s", hostName(state->host));
			uiMessageBox(ctx, "Resumed",
				"A debugger attached and has resumed tinrom.\n\n"
				"It stays armed: breakpoints and crashes still\n"
				"stop in the debugger until you detach here.");

			return;
		}

		if ((tinromPadPressed(0) | tinromPadPressed(1)) & TINROM_PAD_CIRCLE) {
			tinromDebugSetAttachOnContact(false);

			return;
		}
	}
}

static void detach(TinromContext *ctx) {
	tinromDebugArm(false);
	TINROM_LOG("gdb: disarmed");

	uiMessageBox(ctx, "Detached",
		"Breakpoints cleared and the debugger disarmed.\n\n"
		"The exception handler stays installed; disarm the\n"
		"trap under Host filesystem if you want the\n"
		"kernel's back. A debugger that connects again\n"
		"re-arms it.");
}

static void showBreakpoints(TinromContext *ctx) {
	const TinromMonitorState *state = tinromDebugState();

	infoCount = 0;

	if (state) {
		for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
			const TinromBreakpoint *bp = &state->breakpoints[i];

			if (bp->kind != TINROM_BP_FREE)
				addLine("%08x\t%s", (unsigned int) bp->address,
					kindName(bp->kind));
		}
	}

	if (!infoCount) {
		uiMessageBox(ctx, "Breakpoints",
			"None set. GDB's own \"break\" command sets them; "
			"through nops, GDB writes break instructions "
			"itself, and those do not appear here.");

		return;
	}

	uiTextPane(ctx, "Breakpoints", infoLineAt, infoCount);

	if (uiConfirm(ctx, "Breakpoints", "Clear all of them?", false)) {
		tinromDebugClearBreakpoints();
		TINROM_LOG("gdb: breakpoints cleared");
	}
}

/*
 * The other side of debugging: what a game that crashes without a debugger
 * gets, which is the cart's crash screen (hal/crash.h). The only way to see
 * it from here is to crash for real, through the kernel, as a game would.
 */
static void crashOnPurpose(TinromContext *ctx) {
	if (!tinromCrashScreenEntry()) {
		uiMessageBox(ctx, "No crash screen",
			"Games get a crash screen from a tinrom cart built "
			"with TINROM_ROM_CRASH_SCREEN, in the slot. There "
			"is no such cart in this one.");

		return;
	}

	if (!uiConfirm(ctx, "Crash on purpose",
		"This crashes the console the way a game would, to "
		"show the screen a game gets. Nothing comes back from "
		"it: reset the console afterwards.\n\n"
		"If it freezes with no screen at all, this kernel "
		"does not reach the crash screen. Please report it.",
		false))
		return;

	/* Returns only if the cart went away between asking and crashing. */
	uiShowResult(ctx, "Crashing", tinromCrashScreenTest());
}

static TinromResult gdbEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Status",              "What the debugger is doing, live", true, 0 },
		{ "Wait for a debugger", "Halt when GDB or nops connects", true, 1 },
		{ "Detach",              "Clear breakpoints and disarm", true, 2 },
		{ "Breakpoints",         "List and clear what is set", true, 3 },
		{ "Crash on purpose",    "See the crash screen games get", true, 4 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Debugger", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		if (choice == UI_MENU_CANCELLED)
			return TINROM_OK;

		switch (items[choice].tag) {
			case 0:  showStatus(ctx);      break;
			case 1:  waitForDebugger(ctx); break;
			case 2:  detach(ctx);          break;
			case 3:  showBreakpoints(ctx); break;
			default: crashOnPurpose(ctx);  break;
		}
	}
}

const TinromModule tinromModule_gdb = {
	.id       = "gdb",
	.name     = "GDB",
	.summary  = "Debug over SIO1: GDB directly, or through nops",
	.minTier  = TINROM_TIER_DEVELOPER,
	/*
	 * Not OWNS_SIO: the port belongs to the serial console, which hands GDB
	 * packets and nops debug commands to the resident monitor.
	 */
	.flags    = TINROM_MODULE_MENU | TINROM_MODULE_AUTOSTART,
	.init     = NULL,
	.enter    = gdbEnter,
	.tick     = NULL,
	.shutdown = NULL
};
