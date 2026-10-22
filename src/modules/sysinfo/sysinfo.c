/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * What am I running on? Console, BIOS, video timing, cart, disc, cards.
 * Mostly reporting on HAL stubs today, so most rows read "unknown".
 */

#include "modules/modules.h"
#include "tinrom/version.h"

#define INFO_LINES 16

static char infoLines[INFO_LINES][64];
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

static const char *regionName(TinromRegion region) {
	switch (region) {
		case TINROM_REGION_NTSC_J: return "NTSC-J";
		case TINROM_REGION_NTSC_U: return "NTSC-U";
		case TINROM_REGION_PAL:    return "PAL";
		default:                   return "unknown";
	}
}

static void collect(TinromContext *ctx) {
	infoCount = 0;

	addLine("tinrom\t%s", tinromVersionFull);
	addLine("built\t%s", tinromBuildTime);
	addLine("boot source %d", (int) ctx->bootSource);
	addLine("video\t%s", tinromVideoIsPAL() ? "PAL (50Hz)" : "NTSC (60Hz)");
	addLine("serial\t%u baud", (unsigned int) tinromSioBaud());

	if (tinromDbgConsolePresent())
		addLine("host\t%s (EXP2 console)", tinromDbgConsoleName());

	addLine("");

	/*
	 * STUB: the BIOS version string lives in the boot ROM at a documented
	 * offset and can simply be read out. Deliberately reading it rather than
	 * embedding any of it - see NOTICE.md.
	 */
	addLine("BIOS\t(not read yet)");
	addLine("");

	TinromFlashInfo flash;
	TinromResult    result = tinromFlashIdentify(&flash);

	if (result == TINROM_OK && flash.present) {
		addLine("cart\t%s (%02x/%02x)",
			flash.name ? flash.name : "unknown",
			flash.manufacturerId, flash.deviceId);
		addLine("cart size\t%u KB", (unsigned int) (flash.size / 1024));
	} else {
		addLine("cart\t%s", tinromResultString(result));
	}

	addLine("");

	TinromDiscInfo disc;

	result = tinromCdIdentify(&disc);

	if (result == TINROM_OK) {
		addLine("disc\t%s %s", regionName(disc.region), disc.serial);
		addLine("label\t%s", disc.label);
	} else {
		addLine("disc\t%s", tinromResultString(result));
	}

	addLine("");

	for (unsigned int port = 0; port < 2; port++) {
		TinromMcInfo card;

		result = tinromMcProbe(port, &card);

		if (result == TINROM_OK)
			addLine("card %u\tpresent, flag %02x", port,
				(unsigned int) card.flag);
		else if (result == TINROM_ERR_NOT_PRESENT)
			addLine("card %u\tempty", port);
		else
			addLine("card %u\t%s", port, tinromResultString(result));
	}
}

static const char *infoLineAt(unsigned int index) {
	return (index < infoCount) ? infoLines[index] : "";
}

static TinromResult sysinfoEnter(TinromContext *ctx) {
	collect(ctx);
	uiTextPane(ctx, "System information", infoLineAt, infoCount);

	return TINROM_OK;
}

const TinromModule tinromModule_sysinfo = {
	.id      = "sysinfo",
	.name    = "System information",
	.summary = "Console, BIOS, cart, disc and card details",
	.minTier = TINROM_TIER_ADVANCED,
	.flags   = TINROM_MODULE_MENU,
	.init    = NULL,
	.enter   = sysinfoEnter,
	.tick    = NULL,
	.shutdown = NULL
};
