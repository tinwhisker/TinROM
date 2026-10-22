/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * ISO9660 layout used here (ECMA-119; the same fields psx-spx lists under
 * "CDROM File Formats"):
 *
 *   Primary volume descriptor, at LBA 16:
 *     +000h  1   type, 1 for the primary descriptor
 *     +001h  5   "CD001"
 *     +028h  32  volume identifier
 *     +09Ch  34  the root directory record, in the layout below
 *
 *   Directory record:
 *     +000h  1   record length; 0 means "nothing more in this sector"
 *     +002h  4   extent LBA, little endian (big endian follows at +006h)
 *     +00Ah  4   data length, little endian
 *     +019h  1   flags; bit 1 set marks a directory
 *     +020h  1   name length
 *     +021h  n   name, with ";1" appended on files
 *
 * Records never straddle a sector boundary, which is what the zero-length
 * terminator is for.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/iso9660.h"
#include "libc/string.h"

#define PVD_TYPE_PRIMARY 1
#define PVD_LABEL        0x28
#define PVD_ROOT_RECORD  0x9c

#define REC_LENGTH  0x00
#define REC_LBA     0x02
#define REC_SIZE    0x0a
#define REC_FLAGS   0x19
#define REC_NAMELEN 0x20
#define REC_NAME    0x21

#define REC_FLAG_DIRECTORY 0x02

static uint32_t read32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static char upper(char c) {
	return ((c >= 'a') && (c <= 'z')) ? (char) (c - 'a' + 'A') : c;
}

static bool isSeparator(char c) {
	return (c == '/') || (c == '\\');
}

/*
 * Compares one path component with a name out of a directory record. The disc
 * side may carry a ";1" version suffix that the path side usually omits, and
 * PS1 discs are inconsistent about case even though ISO9660 says upper.
 */
static bool nameMatches(const char *recordName, const char *wanted,
	size_t wantedLength
) {
	size_t i = 0;

	for (; i < wantedLength; i++) {
		char onDisc = recordName[i];

		if (!onDisc)
			return false;
		if (upper(onDisc) != upper(wanted[i]))
			return false;
	}

	/* Exhausted the wanted name: the rest may only be a version suffix. */
	if (!recordName[i])
		return true;

	return (recordName[i] == ';');
}

TinromResult iso9660Mount(Iso9660 *fs, Iso9660Reader read, void *user) {
	if (!fs || !read)
		return TINROM_ERR_BAD_ARG;

	memset(fs, 0, sizeof(*fs));

	fs->read = read;
	fs->user = user;

	uint8_t      sector[ISO_SECTOR_SIZE];
	TinromResult result = read(ISO_PVD_LBA, sector, 1, user);

	if (result != TINROM_OK)
		return result;

	if ((sector[0] != PVD_TYPE_PRIMARY) || memcmp(&sector[1], "CD001", 5))
		return TINROM_ERR_CORRUPT;

	const uint8_t *root = &sector[PVD_ROOT_RECORD];

	fs->rootLba  = read32(&root[REC_LBA]);
	fs->rootSize = read32(&root[REC_SIZE]);

	if (!fs->rootSize)
		return TINROM_ERR_CORRUPT;

	memcpy(fs->label, &sector[PVD_LABEL], 32);
	fs->label[32] = '\0';

	/* The identifier is space padded to its full width. */
	for (int i = 31; (i >= 0) && (fs->label[i] == ' '); i--)
		fs->label[i] = '\0';

	fs->mounted = true;

	return TINROM_OK;
}

static TinromResult openAt(
	Iso9660 *fs, uint32_t lba, uint32_t size, Iso9660Dir *dir
) {
	memset(dir, 0, sizeof(*dir));

	dir->fs        = fs;
	dir->lba       = lba;
	dir->remaining = size;
	dir->offset    = 0;
	dir->loaded    = false;

	return TINROM_OK;
}

TinromResult iso9660NextEntry(Iso9660Dir *dir, Iso9660Entry *entry) {
	if (!dir || !entry || !dir->fs)
		return TINROM_ERR_BAD_ARG;

	for (;;) {
		if (!dir->remaining)
			return TINROM_ERR_NOT_PRESENT;

		if (!dir->loaded) {
			TinromResult result = dir->fs->read(
				dir->lba, dir->sector, 1, dir->fs->user
			);

			if (result != TINROM_OK)
				return result;

			dir->loaded = true;
			dir->offset = 0;
		}

		if (dir->offset >= ISO_SECTOR_SIZE) {
			dir->lba++;
			dir->loaded     = false;
			dir->remaining -= (dir->remaining < ISO_SECTOR_SIZE)
				? dir->remaining
				: ISO_SECTOR_SIZE;

			continue;
		}

		const uint8_t *record = &dir->sector[dir->offset];
		uint8_t        length = record[REC_LENGTH];

		/* Zero length: nothing more in this sector, try the next one. */
		if (!length) {
			dir->offset = ISO_SECTOR_SIZE;

			continue;
		}

		if ((dir->offset + length) > ISO_SECTOR_SIZE)
			return TINROM_ERR_CORRUPT;

		dir->offset += length;

		uint8_t nameLength = record[REC_NAMELEN];

		/* "." and ".." are stored as single bytes 00h and 01h. */
		if ((nameLength == 1)
			&& ((record[REC_NAME] == 0) || (record[REC_NAME] == 1)))
			continue;

		if (nameLength > ISO_NAME_MAX)
			nameLength = ISO_NAME_MAX;

		memcpy(entry->name, &record[REC_NAME], nameLength);
		entry->name[nameLength] = '\0';

		entry->lba         = read32(&record[REC_LBA]);
		entry->size        = read32(&record[REC_SIZE]);
		entry->isDirectory = (record[REC_FLAGS] & REC_FLAG_DIRECTORY) != 0;

		return TINROM_OK;
	}
}

/* Skips "cdrom:" and any leading separators. */
static const char *normalisePath(const char *path) {
	if (!strncmp(path, "cdrom:", 6) || !strncmp(path, "CDROM:", 6))
		path += 6;

	while (*path && isSeparator(*path))
		path++;

	return path;
}

static TinromResult findIn(
	Iso9660 *fs, uint32_t lba, uint32_t size,
	const char *name, size_t nameLength, Iso9660Entry *out
) {
	Iso9660Dir dir;

	openAt(fs, lba, size, &dir);

	for (;;) {
		Iso9660Entry entry;
		TinromResult result = iso9660NextEntry(&dir, &entry);

		if (result != TINROM_OK)
			return result;

		if (nameMatches(entry.name, name, nameLength)) {
			*out = entry;

			return TINROM_OK;
		}
	}
}

TinromResult iso9660OpenDir(Iso9660 *fs, const char *path, Iso9660Dir *dir) {
	if (!fs || !fs->mounted || !dir)
		return TINROM_ERR_BAD_ARG;

	uint32_t lba  = fs->rootLba;
	uint32_t size = fs->rootSize;

	const char *cursor = normalisePath(path ? path : "");

	while (*cursor) {
		const char *start = cursor;

		while (*cursor && !isSeparator(*cursor))
			cursor++;

		Iso9660Entry entry;
		TinromResult result = findIn(
			fs, lba, size, start, (size_t) (cursor - start), &entry
		);

		if (result != TINROM_OK)
			return result;
		if (!entry.isDirectory)
			return TINROM_ERR_BAD_ARG;

		lba  = entry.lba;
		size = entry.size;

		while (*cursor && isSeparator(*cursor))
			cursor++;
	}

	return openAt(fs, lba, size, dir);
}

TinromResult iso9660Find(
	Iso9660 *fs, const char *path, uint32_t *lba, uint32_t *size
) {
	if (!fs || !fs->mounted || !path || !lba || !size)
		return TINROM_ERR_BAD_ARG;

	const char *cursor    = normalisePath(path);
	uint32_t    dirLba    = fs->rootLba;
	uint32_t    dirSize   = fs->rootSize;

	if (!*cursor)
		return TINROM_ERR_BAD_ARG;

	for (;;) {
		const char *start = cursor;

		while (*cursor && !isSeparator(*cursor))
			cursor++;

		size_t length = (size_t) (cursor - start);

		Iso9660Entry entry;
		TinromResult result = findIn(fs, dirLba, dirSize, start, length, &entry);

		if (result != TINROM_OK)
			return result;

		while (*cursor && isSeparator(*cursor))
			cursor++;

		if (!*cursor) {
			if (entry.isDirectory)
				return TINROM_ERR_BAD_ARG;

			*lba  = entry.lba;
			*size = entry.size;

			return TINROM_OK;
		}

		if (!entry.isDirectory)
			return TINROM_ERR_NOT_PRESENT;

		dirLba  = entry.lba;
		dirSize = entry.size;
	}
}

TinromResult iso9660ReadFile(
	Iso9660 *fs, const char *path, void *buffer, size_t capacity,
	uint32_t *sizeOut
) {
	uint32_t     lba, size;
	TinromResult result = iso9660Find(fs, path, &lba, &size);

	if (result != TINROM_OK)
		return result;

	if (sizeOut)
		*sizeOut = size;

	size_t sectors = (size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

	/*
	 * Reads land whole sectors, so the buffer has to hold the rounded-up
	 * size, not the file size. Checking against the file size instead is how
	 * a 100-byte SYSTEM.CNF overruns a 100-byte buffer by 1948 bytes.
	 */
	if ((sectors * ISO_SECTOR_SIZE) > capacity)
		return TINROM_ERR_NO_SPACE;

	if (!sectors)
		return TINROM_OK;

	return fs->read(lba, buffer, sectors, fs->user);
}
