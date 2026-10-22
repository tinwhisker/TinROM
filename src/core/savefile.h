/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * tinrom's own saves on a memory card.
 *
 * A save is one block, laid out as any PS1 save is - a title frame naming it
 * and an icon frame, which is what lets the memory card screen and the
 * console's own BIOS show it as "tinrom settings" with an icon - and then a
 * container of tagged sections:
 *
 *   "TNRM", version, section count, bytes of sections, their FNV-1a hash,
 *   then each section: tag, length, data padded to four bytes.
 *
 * Sections are how one file holds several things - the settings, and the
 * terminal's bookmarks if they were saved with the settings - and how a
 * newer tinrom can add one an older one skips. There are two files, so
 * bookmarks can also live in a save of their own, on either card:
 *
 *   BETNRM-00001SETTINGS  "tinrom settings"
 *   BETNRM-00002COMMS     "tinrom bookmarks"
 *
 * One file is in memory at a time: load it, change sections, store it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

typedef struct {
	const char *filename; /* the directory name, at most 20 characters */
	const char *title;    /* ASCII; stored as Shift-JIS */
} TinromSaveFile;

extern const TinromSaveFile tinromSaveSettings;
extern const TinromSaveFile tinromSaveComms;

/* Section tags, four characters read little-endian. */
#define TINROM_SAVE_TAG(a, b, c, d) \
	((uint32_t) (a) | ((uint32_t) (b) << 8) | ((uint32_t) (c) << 16) \
	| ((uint32_t) (d) << 24))

#define TINROM_SECTION_SETTINGS TINROM_SAVE_TAG('S', 'E', 'T', 'S')
#define TINROM_SECTION_COMMS    TINROM_SAVE_TAG('C', 'O', 'M', 'M')
#define TINROM_SECTION_SWITCH   TINROM_SAVE_TAG('S', 'W', 'C', 'H')

/* Room for sections in one block, after the title and icon frames. */
#define TINROM_SAVE_CAPACITY (8192 - 256 - 16)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reads `file` from the card in `port` (0 or 1) into memory. A card with no
 * such file gives TINROM_ERR_NOT_PRESENT and leaves memory empty, ready to
 * build a new one; so does a damaged one, as TINROM_ERR_CORRUPT.
 */
TinromResult tinromSaveLoad(unsigned int port, const TinromSaveFile *file);

/** Forgets what is in memory. */
void tinromSaveClear(void);

/** A section of what was loaded, or NULL. `*length` gets its size. */
const void *tinromSaveSection(uint32_t tag, size_t *length);

/** Adds or replaces a section in memory. TINROM_ERR_NO_SPACE if too big. */
TinromResult tinromSaveSetSection(uint32_t tag, const void *data, size_t length);

/** Writes what is in memory to `port` as `file`, replacing any copy there. */
TinromResult tinromSaveStore(unsigned int port, const TinromSaveFile *file);

/** Whether a card is in `port`, for offering it as a place to save. */
bool tinromSaveCardPresent(unsigned int port);

#ifdef __cplusplus
}
#endif
