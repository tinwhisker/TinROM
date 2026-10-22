/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Read-only ISO9660, enough to find and load a file off a disc.
 *
 * Deliberately knows nothing about the CD-ROM drive: it reads through a
 * caller-supplied function. That keeps the format code out of the HAL, and it
 * means the whole walker can be exercised on a host against a disc image
 * built by mkpsxiso, which is how it was checked before it ever ran on a
 * console. See docs/cd-boot.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

#define ISO_SECTOR_SIZE 2048
/* The primary volume descriptor always lives here. */
#define ISO_PVD_LBA     16
#define ISO_NAME_MAX    32

typedef TinromResult (*Iso9660Reader)(
	uint32_t lba, void *buffer, size_t count, void *user
);

typedef struct {
	Iso9660Reader read;
	void         *user;

	uint32_t      rootLba;
	uint32_t      rootSize;
	char          label[33];
	bool          mounted;
} Iso9660;

typedef struct {
	char     name[ISO_NAME_MAX + 1];
	uint32_t lba;
	uint32_t size;
	bool     isDirectory;
} Iso9660Entry;

/** Iteration state for walking one directory. */
typedef struct {
	Iso9660     *fs;
	uint32_t     lba;
	uint32_t     remaining;
	uint32_t     offset;
	uint8_t      sector[ISO_SECTOR_SIZE];
	bool         loaded;
} Iso9660Dir;

#ifdef __cplusplus
extern "C" {
#endif

/** Reads the primary volume descriptor and remembers where the root is. */
TinromResult iso9660Mount(Iso9660 *fs, Iso9660Reader read, void *user);

/** Opens a directory by path; "" or "/" is the root. */
TinromResult iso9660OpenDir(Iso9660 *fs, const char *path, Iso9660Dir *dir);
/** Next entry, or TINROM_ERR_NOT_PRESENT at the end. */
TinromResult iso9660NextEntry(Iso9660Dir *dir, Iso9660Entry *entry);

/**
 * Locates a file. Accepts the forms a PS1 disc actually uses:
 * "cdrom:\\SLUS_007.07;1", "\\SYSTEM.CNF", "/system.cnf" - separators either
 * way round, case insensitive, and the ";1" version suffix optional.
 */
TinromResult iso9660Find(
	Iso9660 *fs, const char *path, uint32_t *lba, uint32_t *size
);

/**
 * Finds and reads a whole file. Reads land whole sectors, so `capacity` must
 * cover the file size rounded up to ISO_SECTOR_SIZE; anything less fails with
 * TINROM_ERR_NO_SPACE rather than overrunning. `sizeOut` is filled in even
 * then, so a caller can size a buffer and try again.
 */
TinromResult iso9660ReadFile(
	Iso9660 *fs, const char *path, void *buffer, size_t capacity,
	uint32_t *sizeOut
);

#ifdef __cplusplus
}
#endif
