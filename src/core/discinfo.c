/* tinrom - (C) 2026 The tinrom contributors - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/discinfo.h"
#include "core/iso9660.h"
#include "core/psexe.h"
#include "core/result.h"
#include "hal/cdrom.h"

/* SYSTEM.CNF is a few short lines, but reads land whole sectors. */
static uint8_t configBuffer[ISO_SECTOR_SIZE];
static Iso9660 disc;

static TinromResult readSectors(
	uint32_t lba, void *buffer, size_t count, void *user
) {
	(void) user;

	return tinromCdReadSectors(lba, buffer, count);
}

const char *tinromRegionName(TinromRegion region) {
	switch (region) {
		case TINROM_REGION_NTSC_J: return "NTSC-J";
		case TINROM_REGION_NTSC_U: return "NTSC-U";
		case TINROM_REGION_PAL:    return "PAL";
		case TINROM_REGION_FREE:   return "region free";
		default:                   return "unknown";
	}
}

void tinromDescribeDrive(TinromLineFn addLine) {
	TinromDriveInfo drive;

	if (tinromCdDriveInfo(&drive) != TINROM_OK) {
		addLine("drive\tdid not answer");

		return;
	}

	addLine("drive\t%s", drive.version[0] ? drive.version : "unknown");
	addLine("console\t%s (%s)", tinromRegionName(drive.region),
		drive.regionString[0] ? drive.regionString : "no answer");
	addLine("unlocked\t%s", tinromCdUnlocked() ? "yes" : "no");
}

void tinromDescribeDisc(TinromLineFn addLine) {
	TinromDiscInfo info;
	TinromResult   result = tinromCdIdentify(&info);

	if (result != TINROM_OK) {
		addLine("disc\t%s", tinromResultString(result));

		return;
	}

	if (info.type == TINROM_DISC_AUDIO) {
		addLine("disc\taudio");

		return;
	}

	addLine("disc\tdata");
	addLine("SCEx\t%s", info.scex[0] ? info.scex : "(none)");
	addLine("region\t%s", tinromRegionName(info.region));
	addLine("licensed\t%s", info.licenceStringPresent ? "yes"
		: "no - a CD-R or an import");

	/* A locked drive will not read a disc without the licence. */
	if (!info.licenceStringPresent && !tinromCdUnlocked())
		return;

	if (iso9660Mount(&disc, readSectors, NULL) != TINROM_OK)
		return;

	addLine("label\t%s", disc.label[0] ? disc.label : "(none)");

	uint32_t size = 0;

	if (iso9660ReadFile(&disc, "\\SYSTEM.CNF", configBuffer,
		sizeof(configBuffer), &size) == TINROM_OK) {
		char boot[48];

		if (tinromParseBootLine((const char *) configBuffer, size, boot,
			sizeof(boot)))
			addLine("boot\t%s", boot);
	} else {
		addLine("boot\t(no SYSTEM.CNF)");
	}
}
