/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include "common/gpu.h"
#include "hal/console.h"
#include "hal/video.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

static bool videoIsPAL;
static bool videoBlanked;

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
	 * setupGPU() resets the GPU, enables its DMA channels and unblanks the
	 * display - which would show whatever is in VRAM until the first frame
	 * is drawn: noise from a cold start, the last picture from a reset, the
	 * old mode's frame stretched into the new one. So it is blanked again
	 * here, and tinromVideoReveal() shows it once there is a frame to show.
	 */
	GPU_GP1      = gp1_dispBlank(true);
	videoBlanked = true;
}

void tinromVideoReveal(void) {
	if (!videoBlanked)
		return;

	GPU_GP1      = gp1_dispBlank(false);
	videoBlanked = false;
}

/*
 * The console's region, from the boot ROM's version string (hal/console.c):
 * E is PAL, J and A are NTSC. This is what "auto" goes by first. The GPU's
 * current mode only says what the BIOS last set, and a cart that takes over
 * mid-boot (docs/cart-rom.md) gets there before the BIOS has set anything:
 * a reset GPU is NTSC, so a PAL console came up NTSC. pcsx-redux does not
 * show it.
 */
static bool biosRegionIsPAL(bool *pal) {
	switch (tinromBiosRegion()) {
		case 'E':           *pal = true;  return true;
		case 'J': case 'A': *pal = false; return true;
		default:            return false;
	}
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
