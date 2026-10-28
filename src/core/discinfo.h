/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * The drive and the disc in it, described in lines of text - for the Boot
 * disc module's "Drive and disc" and for System information, which show the
 * same things.
 */
#pragma once

#include "hal/cdrom.h"

/* Adds one line to whatever the caller is building; printf-style. */
typedef void (*TinromLineFn)(const char *format, ...);

#ifdef __cplusplus
extern "C" {
#endif

const char *tinromRegionName(TinromRegion region);

/** The drive's firmware and region, and whether it is unlocked. */
void tinromDescribeDrive(TinromLineFn addLine);

/**
 * The disc: data or audio, its SCEx string and region, whether it carries
 * the licence, and - when it can be read - its volume label and SYSTEM.CNF's
 * BOOT line. Spins the disc up, so it can take a few seconds.
 */
void tinromDescribeDisc(TinromLineFn addLine);

#ifdef __cplusplus
}
#endif
