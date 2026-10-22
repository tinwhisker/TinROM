/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Serial console. Owns SIO1 and runs the nops-compatible server in the
 * background so a host can talk to tinrom while the menu is up.
 */

#include "modules/modules.h"
#include "modules/siocons/nops_server.h"

static bool serverRunning;

static TinromResult sioconsInit(TinromContext *ctx) {
	(void) ctx;

	nopsServerInit();
	serverRunning = true;

	return TINROM_OK;
}

static void sioconsTick(TinromContext *ctx) {
	(void) ctx;

	/* Not while the terminal or IRC client has the port. */
	if (!serverRunning || tinromSioBuffering())
		return;

	/* Service at most a few commands per frame so the UI stays responsive. */
	for (int i = 0; i < 4; i++) {
		if (!nopsServerPoll())
			break;
	}
}

static TinromResult sioconsEnter(TinromContext *ctx) {
	UIContext *ui = &ctx->ui;

	for (;;) {
		const NopsServerState *status = nopsServerStatus();

		uiBeginFrame(ui);
		uiSetChrome(ui, "Serial console", "Cross: toggle server   Circle: back");

		int y = TINROM_HEADER_H + 10;

		uiTextf(ui, TINROM_MARGIN_X + 4, y, "server\t%s",
			serverRunning ? "listening" : "stopped");
		y += TINROM_ROW_HEIGHT;
		uiTextf(ui, TINROM_MARGIN_X + 4, y, "rate\t%u baud",
			(unsigned int) tinromSioBaud());
		y += TINROM_ROW_HEIGHT;
		uiTextf(ui, TINROM_MARGIN_X + 4, y, "commands\t%u",
			(unsigned int) status->commandsHandled);
		y += TINROM_ROW_HEIGHT;
		/*
		 * Transfers are the resident monitor's (see docs/debugger.md), so
		 * the byte counts are too.
		 */
		const TinromMonitorState *monitor = tinromDebugState();

		uiTextf(ui, TINROM_MARGIN_X + 4, y, "received\t%u bytes",
			(unsigned int) (monitor ? monitor->bytesIn : status->bytesReceived));
		y += TINROM_ROW_HEIGHT;
		uiTextf(ui, TINROM_MARGIN_X + 4, y, "sent\t%u bytes",
			(unsigned int) (monitor ? monitor->bytesOut : status->bytesSent));
		y += TINROM_ROW_HEIGHT * 2;
		uiText(ui, TINROM_MARGIN_X + 4, y,
			"Point the host tool at this console:");
		y += TINROM_ROW_HEIGHT;
		uiText(ui, TINROM_MARGIN_X + 4, y,
			"  python -m tinrom_host ping COM3");

		uiEndFrame(ui);

		sioconsTick(ctx);

		uint16_t pressed = tinromPadPressed(0) | tinromPadPressed(1);

		if (pressed & TINROM_PAD_CROSS)
			serverRunning = !serverRunning;
		if (pressed & TINROM_PAD_CIRCLE)
			return TINROM_OK;
	}
}

static void sioconsShutdown(TinromContext *ctx) {
	(void) ctx;

	serverRunning = false;
}

const TinromModule tinromModule_siocons = {
	.id      = "siocons",
	.name    = "Serial console",
	.summary = "nops server: upload, dump, poke, TTY",
	.minTier = TINROM_TIER_DEVELOPER,
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_BACKGROUND
		| TINROM_MODULE_OWNS_SIO,
	.init    = sioconsInit,
	.enter   = sioconsEnter,
	.tick    = sioconsTick,
	.shutdown = sioconsShutdown
};
