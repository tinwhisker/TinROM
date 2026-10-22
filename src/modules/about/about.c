/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Credits and attribution. This module is ESSENTIAL and should stay that
 * way: it is where tinrom discharges its obligation to say whose work it is
 * built on. Keep it in sync with NOTICE.md.
 */

#include "modules/modules.h"
#include "tinrom/version.h"

/*
 * The first two lines carry the build identity, which is only known at link
 * time, so they are filled in rather than baked into the table.
 */
static char versionLine[48];
static char buildLine[48];

static const char *const creditLines[] = {
	NULL, /* versionLine */
	NULL, /* buildLine   */
	"A clean-room, modular UniROM replacement.",
	"MIT licensed. No Sony code. See NOTICE.md.",
	"",
	"Built on:",
	"  ps1-bare-metal, by spicyjpeg (MIT)",
	"  printf, by Marco Paland (MIT)",
	"",
	"Font:",
	"  from the Superpowers asset packs (CC0)",
	"  superpowers-html5.com",
	"",
	"Documented by:",
	"  psx-spx, by Martin Korth (nocash)",
	"  and the psx-spx community",
	"",
	"Wire-compatible with, but no code from:",
	"  NOTPSXSerial, by JonathanDotCel (MPL-2.0)",
	"",
	"Feature list modelled on:",
	"  UniROM, by JonathanDotCel et al",
	"",
	"Memory card boot concept:",
	"  FreePSXBoot, by brad-lin et al",
	"",
	"tinrom is not affiliated with, or endorsed by, any of the above. "
	"Thanks to all of them.",
	""
};

#define CREDIT_COUNT (sizeof(creditLines) / sizeof(creditLines[0]))

static const char *creditLine(unsigned int index) {
	if (index >= CREDIT_COUNT)
		return "";

	switch (index) {
		case 0:
			snprintf(versionLine, sizeof(versionLine), "tinrom %s",
				tinromVersionFull);

			return versionLine;

		case 1:
			snprintf(buildLine, sizeof(buildLine), "built %s",
				tinromBuildTime);

			return buildLine;

		default:
			return creditLines[index];
	}
}

static const char *logLineAt(unsigned int index) {
	return tinromLogLine(index);
}

static TinromResult aboutEnter(TinromContext *ctx) {
	static const UIMenuItem items[] = {
		{ "Credits and licences", "Who wrote what tinrom is built on", true, 0 },
		{ "Boot log",             "Everything logged since power-on",  true, 1 },
		{ "Module list",          "What is compiled into this build",  true, 2 }
	};

	size_t cursor = 0;

	for (;;) {
		size_t choice = uiMenuRunAt(ctx, "About tinrom", items, 3, &cursor);

		switch (choice) {
			case 0:
				uiTextPane(ctx, "Credits", creditLine, CREDIT_COUNT);
				break;

			case 1:
				uiTextPane(ctx, "Boot log", logLineAt, tinromLogCount());
				break;

			case 2:
				uiNotImplemented(ctx, "Module list");
				break;

			default:
				return TINROM_OK;
		}
	}
}

const TinromModule tinromModule_about = {
	.id      = "about",
	.name    = "About",
	.summary = "Version, credits, licences and the boot log",
	.minTier = TINROM_TIER_BASIC,
	.flags   = TINROM_MODULE_MENU | TINROM_MODULE_ESSENTIAL,
	.init    = NULL,
	.enter   = aboutEnter,
	.tick    = NULL,
	.shutdown = NULL
};
