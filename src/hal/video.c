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

void tinromVideoInit(TinromVideoPreference preference) {
	bool pal;

	switch (preference) {
		case TINROM_VIDEO_NTSC: pal = false;                         break;
		case TINROM_VIDEO_PAL:  pal = true;                          break;
		default:                pal = getCurrentVideoMode() == GP1_MODE_PAL; break;
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
