/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The CD player visualiser's 3D half: what visual.c measured, as boxes
 * drawn with the GTE behind the UI. See visual.h.
 */
#pragma once

#include "modules/cdplay/visual.h"
#include "ui/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draws `visual` in `mode` into the frame, in the background's buckets and
 * within `wordBudget` (a UIBackground, ui/ui.h, does the calling). `frame`
 * drives the slow turn.
 */
void sceneDraw(
	UIContext *ui, unsigned int wordBudget, const Visual *visual,
	VisualMode mode, unsigned int frame
);

#ifdef __cplusplus
}
#endif
