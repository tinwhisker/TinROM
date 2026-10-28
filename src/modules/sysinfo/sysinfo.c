/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * What am I running on? A menu of sections - the console, the cart, the
 * drive and disc, the memory cards. The cart's has, apart from what it
 * holds, asking its flash chip what it is - apart because that writes to
 * the cart.
 */

#include "core/discinfo.h"
#include "core/profile.h"
#include "hal/handoff.h"
#include "libc/string.h"
#include "modules/modules.h"
#include "tinrom/version.h"

#define INFO_LINES 18

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

static const char *infoLineAt(unsigned int index) {
	return (index < infoCount) ? infoLines[index] : "";
}

/* A screen to show while something slow - the disc - is read. */
static void working(TinromContext *ctx, const char *what) {
	uiBeginFrame(&ctx->ui);
	uiSetChrome(&ctx->ui, "System information", "");
	uiText(&ctx->ui, TINROM_MARGIN_X + 4, TINROM_HEADER_H + 20, what);
	uiEndFrame(&ctx->ui);
}

/* --- the console ------------------------------------------------------- */

static const char *bootSourceName(TinromBootSource source) {
	switch (source) {
		case TINROM_BOOT_PSEXE:   return "uploaded, or an emulator";
		case TINROM_BOOT_CART:    return "cart, post-boot (with a disc)";
		case TINROM_BOOT_PREBOOT: return "cart, pre-boot";
		case TINROM_BOOT_CD:      return "boot disc";
		case TINROM_BOOT_MEMCARD: return "memory card";
		case TINROM_BOOT_MIDBOOT: return "cart, mid-boot";
		default:                  return "unknown";
	}
}

static const char *regionLetterName(char region) {
	switch (region) {
		case 'J': return "NTSC-J";
		case 'A': return "NTSC-U";
		case 'E': return "PAL";
		default:  return "unknown";
	}
}

static void consoleSection(TinromContext *ctx) {
	TinromConsoleInfo console;

	tinromConsoleIdentify(&console);
	infoCount = 0;

	addLine("tinrom\t%s", tinromVersionFull);
	addLine("built\t%s", tinromBuildTime);
	addLine("started\t%s", bootSourceName(ctx->bootSource));
	addLine("video\t%s", tinromVideoIsPAL() ? "PAL (50Hz)" : "NTSC (60Hz)");
	addLine("serial\t%u baud", (unsigned int) tinromSioBaud());

	if (tinromDbgConsolePresent())
		addLine("host\t%s (EXP2 console)", tinromDbgConsoleName());

	/* What a program loaded from here may not overwrite. */
	addLine("ends at\t%08x", (unsigned int) tinromHandoffProtectedEnd());

	addLine("");

	/* "System ROM Version 4.1 12/16/97 E" - the version, without the words. */
	const char *version = console.version;

	if (!strncmp(version, "System ROM Version ", 19))
		version += 19;

	addLine("BIOS\t%s", version[0] ? version : "(no version string)");
	addLine("region\t%s", regionLetterName(console.region));

	if (console.kernelDate)
		addLine("kernel\t%04x-%02x-%02x",
			(unsigned int) (console.kernelDate >> 16),
			(unsigned int) ((console.kernelDate >> 8) & 0xff),
			(unsigned int) (console.kernelDate & 0xff));
	if (console.kernelMaker[0])
		addLine("\t%s", console.kernelMaker);

	addLine("");
	addLine("CPU\tPRId %08x", (unsigned int) console.cpuId);

	if (!console.gpuAnswered)
		addLine("GPU\tthe original 160-pin one (no type)");
	else if (console.gpuType == 2)
		addLine("GPU\ttype 2, the later 208-pin one");
	else
		addLine("GPU\ttype %u", (unsigned int) console.gpuType);

	if (console.ramBytes)
		addLine("RAM\t%u MB%s", (unsigned int) (console.ramBytes >> 20),
			(console.ramBytes > 0x200000) ? " - a development unit" : "");
	else
		addLine("RAM\tnot tested (memory window set up otherwise)");

	uiTextPane(ctx, "Console", infoLineAt, infoCount);
}

/* --- the cart ---------------------------------------------------------- */

/* What the chip said when asked, this session. */
static TinromFlashInfo flash;
static TinromResult    flashResult = TINROM_ERR_NOT_PRESENT;

static void addChipLine(void) {
	switch (flashResult) {
		case TINROM_OK: {
			const char *maker = tinromFlashMakerName(flash.manufacturerId);

			if (flash.name)
				addLine("chip\t%s, %u K", flash.name,
					(unsigned int) (flash.size >> 10));
			else
				addLine("chip\t%s %02x, part %02x", maker ? maker : "maker",
					flash.manufacturerId, flash.deviceId);
			break;
		}

		case TINROM_ERR_NOT_PRESENT:
			addLine("chip\tnot asked yet - \"Identify its chip\"");
			break;

		case TINROM_ERR_CORRUPT:
			addLine("chip\tan EEPROM - %s", flash.restored
				? "put back as it was" : "NOT put back: reflash it");
			break;

		default:
			addLine("chip\t%s", tinromResultString(flashResult));
			break;
	}
}

static void cartDetails(TinromContext *ctx) {
	TinromCartInfo cart;

	(void) tinromExpCartInfo(&cart);
	infoCount = 0;

	if (cart.tinrom) {
		addLine("cart\ttinrom %u.%u.%u", (unsigned int) (cart.version >> 16),
			(unsigned int) ((cart.version >> 8) & 0xff),
			(unsigned int) (cart.version & 0xff));

		if (cart.packedSize)
			addLine("payload\t%u K, packed to %u K",
				(unsigned int) (cart.payloadSize >> 10),
				(unsigned int) (cart.packedSize >> 10));
		else
			addLine("payload\t%u K, as it is",
				(unsigned int) (cart.payloadSize >> 10));

		addLine("image\t%u K of the ROM", (unsigned int) ((cart.imageEnd + 1023) >> 10));
		addLine("this\t%s", tinromExpRunningFromCart()
			? "is the tinrom it holds" : "came from elsewhere");
	} else if (cart.header) {
		addLine("cart\tone with a boot header - not tinrom's");
	} else {
		addLine("cart\tnone, or none the console would boot");
	}

	if (tinromExpTruncatedWindow())
		addLine("window\tEXP1 repeats every %u K",
			(unsigned int) (tinromExpTruncatedWindow() >> 10));

	addLine("");

	TinromSwitchSource source = tinromSwitchSource();

	if (source == TINROM_SWITCH_NONE)
		addLine("switch\tnot set up (Settings)");
	else
		addLine("switch\t%s (%s)", tinromExpSwitchOn(source) ? "on" : "off",
			tinromSwitchSourceName(source));

	addChipLine();

	uiTextPane(ctx, "What the cart holds", infoLineAt, infoCount);
}

static void identifyChip(TinromContext *ctx) {
	if (!uiConfirm(ctx, "Identify the cart's chip",
		"This sends the cart's chip the standard command to say what it "
		"is. A flash chip answers and carries on as before.\n\n"
		"A few carts use an EEPROM instead, which stores the command as "
		"data over what is on the cart. tinrom keeps a copy of those bytes "
		"first, puts them back if that happens and checks them - but have a "
		"way to reflash the cart before saying yes.", true))
		return;

	flashResult = tinromFlashIdentify(&flash);

	char body[200];

	switch (flashResult) {
		case TINROM_OK: {
			const char *maker = tinromFlashMakerName(flash.manufacturerId);
			char        size[16] = "unknown";

			if (flash.size)
				snprintf(size, sizeof(size), "%u K",
					(unsigned int) (flash.size >> 10));

			snprintf(body, sizeof(body),
				"Maker:\t%s (%02x)\nPart:\t%s (%02x)\nSize:\t%s",
				maker ? maker : "not one tinrom knows", flash.manufacturerId,
				flash.name ? flash.name : "not in tinrom's table",
				flash.deviceId, size);

			uiMessageBox(ctx, "The cart's chip", body);
			break;
		}

		case TINROM_ERR_UNSUPPORTED:
			uiMessageBox(ctx, "No answer",
				"Nothing answered. The cart's chip does not take the "
				"command, or the cart does not pass writes through to it.\n\n"
				"Nothing on the cart changed.");
			break;

		case TINROM_ERR_CORRUPT:
			if (flash.restored)
				uiMessageBox(ctx, "An EEPROM",
					"The chip stored the command as data rather than "
					"answering it, so it is an EEPROM, not a flash chip.\n\n"
					"What it wrote over has been put back and read back the "
					"same: the cart is as it was.");
			else
				uiMessageBox(ctx, "The cart was changed",
					"The chip stored the command as data, and what it wrote "
					"over - around +2AAAh and +5555h - could not be put back."
					"\n\ntinrom runs from RAM, so nothing breaks yet, but "
					"flash the cart again before you next boot from it.");
			break;

		case TINROM_ERR_IO:
			uiMessageBox(ctx, "The chip is stuck",
				"The chip answered but did not go back to reading as normal. "
				"Turn the console off and on before booting from the cart.");
			break;

		default:
			uiShowResult(ctx, "Identify", flashResult);
			break;
	}
}

static void cartSection(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "What it holds",    "tinrom, its sizes, the switch, the chip", true, 0 },
		{ "Identify its chip", "Asks the flash chip what it is - writes to the cart",
			true, 1 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "Cart", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		switch (choice) {
			case 0:  cartDetails(ctx);  break;
			case 1:  identifyChip(ctx); break;
			default: return;
		}
	}
}

/* --- the drive and disc ------------------------------------------------ */

static void discSection(TinromContext *ctx) {
	working(ctx, "Reading the drive and disc...");

	infoCount = 0;
	tinromDescribeDrive(addLine);
	addLine("");
	tinromDescribeDisc(addLine);

	uiTextPane(ctx, "Drive and disc", infoLineAt, infoCount);
}

/* --- the memory cards -------------------------------------------------- */

static void cardsSection(TinromContext *ctx) {
	infoCount = 0;

	for (unsigned int port = 0; port < 2; port++) {
		TinromMcInfo card;
		TinromResult result = tinromMcProbe(port, &card);

		if (port)
			addLine("");

		if (result == TINROM_ERR_NOT_PRESENT) {
			addLine("card %u\tempty", port + 1);
			continue;
		}
		if (result != TINROM_OK) {
			addLine("card %u\t%s", port + 1, tinromResultString(result));
			continue;
		}

		addLine("card %u\tpresent, status %02x", port + 1,
			(unsigned int) card.flag);

		if (card.flag & 0x08)
			addLine("\tnot written since power-up");
		if (card.flag & 0x04)
			addLine("\tits last write failed");

		bool aliases = false;

		result = tinromMcCheckAliasing(port, &aliases);

		if (result == TINROM_OK)
			addLine("\t%s", aliases ? "possibly counterfeit: wraps past its end"
				: "behaves like a genuine card");
		else if (result == TINROM_ERR_UNSUPPORTED)
			addLine("\tanswers past its end - a bigger card?");
	}

	uiTextPane(ctx, "Memory cards", infoLineAt, infoCount);
}

/* ---------------------------------------------------------------------- */

static TinromResult sysinfoEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Console",        "tinrom, BIOS, CPU, GPU and RAM",          true, 0 },
		{ "Cart",           "What is in the parallel port",            true, 1 },
		{ "Drive and disc", "The CD drive, and the disc in it",        true, 2 },
		{ "Memory cards",   "Both slots, and the counterfeit check",   true, 3 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "System information", items,
			sizeof(items) / sizeof(items[0]), &cursor);

		switch (choice) {
			case 0:  consoleSection(ctx); break;
			case 1:  cartSection(ctx);    break;
			case 2:  discSection(ctx);    break;
			case 3:  cardsSection(ctx);   break;
			default: return TINROM_OK;
		}
	}
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
