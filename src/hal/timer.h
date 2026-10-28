/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     tinromTimerInit(void);
/** Frames elapsed since init. The UI's only clock. */
uint32_t tinromFrameCounter(void);
void     tinromWaitVSync(void);
/**
 * Like tinromWaitVSync(), but always for the start of the next vertical
 * blank: one that has already begun since the last wait is counted and let
 * go. For changing what is on screen, where "a blank happened since" is not
 * good enough - the picture may be half way down by now.
 */
void     tinromWaitNextVSync(void);
void     tinromDelayMicroseconds(int microseconds);
/**
 * The three root counters' current values, folded together. Not random, but
 * when it is read depends on everything that ran before, so it is the one
 * source of unpredictability a console without an RNG has. For seeding.
 */
uint32_t tinromTimerNoise(void);

#ifdef __cplusplus
}
#endif
