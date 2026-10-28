/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "core/result.h"
#include "core/settings.h"

/*
 * The display is 640 pixels wide and 240 (or 256) tall, so its pixels are
 * half as wide as they are tall. Everything above the GPU is laid out on a
 * 320-wide grid of square units, as it was when the screen was 320 wide, and
 * ui.c doubles x on the way out; what the extra columns buy is text drawn
 * with twice the detail across (ui/font.h).
 */
#define TINROM_SCREEN_WIDTH        320 /* layout units */
#define TINROM_SCREEN_SCALE_X      2   /* display pixels per unit across */
#define TINROM_SCREEN_DEVICE_WIDTH (TINROM_SCREEN_WIDTH * TINROM_SCREEN_SCALE_X)
#define TINROM_SCREEN_HEIGHT_NTSC 240
#define TINROM_SCREEN_HEIGHT_PAL  256

#ifdef __cplusplus
extern "C" {
#endif

void         tinromVideoInit(TinromVideoPreference preference);
/** True if the GPU is currently in PAL timing. */
bool         tinromVideoIsPAL(void);
unsigned int tinromVideoHeight(void);
/** Re-programme the GPU. Used by the region patch and settings modules. */
TinromResult tinromVideoSetMode(bool pal);
/**
 * Turns the display on, if it is off. Setting a mode leaves it off, so that
 * nothing is seen before a frame is drawn; uiEndFrame() calls this as it
 * shows each frame, so the first one finished is the first one seen.
 */
void         tinromVideoReveal(void);

#ifdef __cplusplus
}
#endif
