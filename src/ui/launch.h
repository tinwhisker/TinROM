/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Handing the console to a PS-EXE, wherever it came from.
 *
 * Every refusal here is a message on screen rather than a return code the
 * caller has to render, because there is exactly one way to explain "this
 * executable will not fit" and it should not be written twice. The reader is
 * a callback so the disc, the file browser and anything later share one
 * load-and-enter path: the interesting part - planning around tinrom's own
 * footprint - has nothing to do with where the bytes come from.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

typedef struct TinromContext TinromContext;

/* PS-EXE payloads are sector-aligned, and every source tinrom reads from
 * hands out 2048-byte sectors. */
#define UI_LAUNCH_SECTOR_SIZE 2048

/** Reads `count` sectors starting at `sector` into `buffer`. */
typedef TinromResult (*UILaunchReader)(
	void *user, uint32_t sector, void *buffer, size_t count
);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Loads the PS-EXE whose 2048-byte header is at `headerSector` and enters it.
 *
 * `what` names the thing being launched, for the confirmation and for any
 * refusal. `quiesce` is called once the payload is in RAM and the console is
 * about to be handed over, for the source to stop its own hardware; pass NULL
 * if there is nothing to stop.
 *
 * On success this DOES NOT RETURN. Every return is a refusal, already
 * explained on screen: TINROM_ERR_ABORTED if the user said no, and otherwise
 * whatever went wrong.
 *
 * One caveat worth knowing before calling: once the second read starts,
 * tinrom has been partly overwritten by the payload. A failure there cannot
 * be reported on a screen that no longer exists, so it is reported on a best
 * effort basis and the console may need a reset.
 */
TinromResult uiLaunchExe(
	TinromContext *ctx,
	const char    *what,
	UILaunchReader read,
	void          *user,
	uint32_t       headerSector,
	void         (*quiesce)(void *user)
);

#ifdef __cplusplus
}
#endif
