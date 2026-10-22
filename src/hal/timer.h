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
