/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include "common/gpu.h"
#include "hal/video.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

static bool videoIsPAL;

static void applyMode(bool pal) {
	videoIsPAL = pal;

	setupGPU(
		pal ? GP1_MODE_PAL : GP1_MODE_NTSC,
		GP1_HRES_640,
		GP1_VRES_256,
		TINROM_SCREEN_DEVICE_WIDTH,
		pal ? TINROM_SCREEN_HEIGHT_PAL : TINROM_SCREEN_HEIGHT_NTSC
	);

	/*
	 * setupGPU() already resets the GPU, unblanks the display and enables the
	 * GPU and OTC DMA channels, so there is nothing else to do here.
	 */
}

/*
 * The console's region, from the boot ROM. Retail BIOSes from v2.0 on end
 * with a "System ROM Version 4.1 12/16/97 E" string, whose last letter is
 * the region: J or A for NTSC, E for PAL. Its exact place varies, so the
 * last 100h bytes are searched for it; it is only read, as text.
 *
 * This is what "auto" goes by first. The GPU's current mode only says what
 * the BIOS last set, and a cart that takes over mid-boot (docs/cart-rom.md)
 * gets there before the BIOS has set anything: a reset GPU is NTSC, so a PAL
 * console came up NTSC. pcsx-redux does not show it.
 */
#define BIOS_TAIL_START 0xbfc7ff00u
#define BIOS_TAIL_END   0xbfc80000u

static const char biosVersionTag[] = "System ROM Version";

static bool biosRegionIsPAL(bool *pal) {
	const unsigned int tagLength = sizeof(biosVersionTag) - 1;

	for (uint32_t at = BIOS_TAIL_START; at + tagLength < BIOS_TAIL_END; at++) {
		const volatile char *text = (const volatile char *) at;
		unsigned int         i;

		for (i = 0; i < tagLength; i++) {
			if (text[i] != biosVersionTag[i])
				break;
		}
		if (i < tagLength)
			continue;

		/* The letter is the last character before the terminator. */
		for (uint32_t end = at + tagLength; end < BIOS_TAIL_END; end++) {
			if (*((const volatile char *) end))
				continue;

			char region = *((const volatile char *) (end - 1));
			char before = *((const volatile char *) (end - 2));

			if (before != ' ')
				return false;

			switch (region) {
				case 'E':           *pal = true;  return true;
				case 'J': case 'A': *pal = false; return true;
				default:            return false;
			}
		}

		return false;
	}

	return false;
}

void tinromVideoInit(TinromVideoPreference preference) {
	bool pal;

	switch (preference) {
		case TINROM_VIDEO_NTSC: pal = false; break;
		case TINROM_VIDEO_PAL:  pal = true;  break;
		default:
			if (!biosRegionIsPAL(&pal))
				pal = getCurrentVideoMode() == GP1_MODE_PAL;
			break;
	}

	applyMode(pal);
}

bool tinromVideoIsPAL(void) {
	return videoIsPAL;
}

unsigned int tinromVideoHeight(void) {
	return videoIsPAL ? TINROM_SCREEN_HEIGHT_PAL : TINROM_SCREEN_HEIGHT_NTSC;
}

TinromResult tinromVideoSetMode(bool pal) {
	applyMode(pal);

	return TINROM_OK;
}
