/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * tinrom's saves. See savefile.h. The title and icon frames follow psx-spx's
 * "Memory Card Data Format"; see NOTICE.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "core/savefile.h"
#include "hal/mcard.h"
#include "libc/string.h"

const TinromSaveFile tinromSaveSettings = {
	"BETNRM-00001SETTINGS", "tinrom settings"
};
const TinromSaveFile tinromSaveComms = {
	"BETNRM-00002COMMS", "tinrom bookmarks"
};

#define BLOCK       TINROM_MC_BLOCK_SIZE
#define DATA_OFFSET (2 * TINROM_MC_FRAME_SIZE) /* after title and icon */
#define MAGIC       TINROM_SAVE_TAG('T', 'N', 'R', 'M')
#define VERSION     1

typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t count;
	uint32_t length; /* bytes of sections after this header */
	uint32_t hash;
} Header;

typedef struct {
	uint32_t tag;
	uint32_t length;
} SectionHeader;

static uint8_t block[BLOCK] __attribute__((aligned(4)));

/* The sections, back to back, as they will be stored. */
static uint8_t  sections[TINROM_SAVE_CAPACITY] __attribute__((aligned(4)));
static size_t   sectionBytes;
static unsigned sectionCount;

/* A cartridge chip with a "t" on it; palette index 0 is transparent. */
static const uint8_t iconPixels[TINROM_MC_FRAME_SIZE] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00,
	0x10, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x01, 0x10, 0x32, 0x33, 0x33, 0x33, 0x33, 0x23, 0x01,
	0x10, 0x32, 0x22, 0x22, 0x22, 0x22, 0x23, 0x01, 0x10, 0x32, 0x22, 0x42, 0x22, 0x22, 0x23, 0x01,
	0x10, 0x32, 0x22, 0x44, 0x44, 0x22, 0x23, 0x01, 0x10, 0x32, 0x22, 0x42, 0x22, 0x22, 0x23, 0x01,
	0x10, 0x32, 0x22, 0x42, 0x22, 0x22, 0x23, 0x01, 0x10, 0x32, 0x22, 0x42, 0x22, 0x24, 0x23, 0x01,
	0x10, 0x32, 0x22, 0x22, 0x44, 0x22, 0x23, 0x01, 0x10, 0x32, 0x22, 0x22, 0x22, 0x22, 0x23, 0x01,
	0x10, 0x32, 0x33, 0x33, 0x33, 0x33, 0x23, 0x01, 0x10, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x01,
	0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x00
};
static const uint16_t iconPalette[16] = {
	0x0000, 0x1462, 0x3904, 0x7e69, 0x7fff, 0x22df
};

static uint32_t fnv(const uint8_t *data, size_t length) {
	uint32_t hash = 0x811c9dc5;

	for (size_t i = 0; i < length; i++) {
		hash ^= data[i];
		hash *= 0x01000193;
	}

	return hash;
}

static size_t padded(size_t length) {
	return (length + 3) & ~(size_t) 3;
}

void tinromSaveClear(void) {
	sectionBytes = 0;
	sectionCount = 0;
}

bool tinromSaveCardPresent(unsigned int port) {
	TinromMcInfo info;

	return (tinromMcProbe(port, &info) == TINROM_OK) && info.present;
}

/*
 * The hash only catches accidents; a file from a save editor or another build
 * can carry a good hash over nonsense. So before anything uses the sections,
 * check that exactly `count` of them, each padded, fill `length` - written
 * without additions that could wrap.
 */
static bool sectionsWellFormed(
	const uint8_t *data, size_t length, unsigned count
) {
	size_t at = 0;

	for (unsigned i = 0; i < count; i++) {
		SectionHeader header;

		if (length - at < sizeof(header))
			return false;

		memcpy(&header, data + at, sizeof(header));
		at += sizeof(header);

		if ((header.length > length - at)
			|| (padded(header.length) > length - at))
			return false;

		at += padded(header.length);
	}

	return at == length;
}

TinromResult tinromSaveLoad(unsigned int port, const TinromSaveFile *file) {
	tinromSaveClear();

	/* The header first, to know how much more there is. */
	size_t       want   = DATA_OFFSET + sizeof(Header);
	TinromResult result = tinromMcReadFile(port, file->filename, block,
		(want + TINROM_MC_FRAME_SIZE - 1) & ~(size_t) (TINROM_MC_FRAME_SIZE - 1));

	if (result != TINROM_OK)
		return result;

	Header header;

	memcpy(&header, block + DATA_OFFSET, sizeof(header));

	if ((header.magic != MAGIC) || (header.version != VERSION)
		|| (header.length > sizeof(sections))) {
		TINROM_LOG("save: %s on card %u is not one tinrom can read",
			file->filename, port + 1);

		return TINROM_ERR_CORRUPT;
	}

	want   = DATA_OFFSET + sizeof(Header) + header.length;
	result = tinromMcReadFile(port, file->filename, block,
		(want + TINROM_MC_FRAME_SIZE - 1) & ~(size_t) (TINROM_MC_FRAME_SIZE - 1));

	if (result != TINROM_OK)
		return result;

	const uint8_t *data = block + DATA_OFFSET + sizeof(Header);

	if (fnv(data, header.length) != header.hash) {
		TINROM_LOG("save: %s on card %u fails its checksum",
			file->filename, port + 1);

		return TINROM_ERR_CORRUPT;
	}

	if (!sectionsWellFormed(data, header.length, header.count)) {
		TINROM_LOG("save: %s on card %u has damaged sections",
			file->filename, port + 1);

		return TINROM_ERR_CORRUPT;
	}

	memcpy(sections, data, header.length);
	sectionBytes = header.length;
	sectionCount = header.count;

	return TINROM_OK;
}

/* Walks the sections; returns the offset of `tag`'s header, or -1. */
static long findSection(uint32_t tag) {
	size_t at = 0;

	for (unsigned i = 0; (i < sectionCount)
		&& (at + sizeof(SectionHeader) <= sectionBytes); i++) {
		SectionHeader header;

		memcpy(&header, sections + at, sizeof(header));

		if (header.length > sectionBytes - at - sizeof(header))
			return -1;
		if (header.tag == tag)
			return (long) at;

		at += sizeof(header) + padded(header.length);
	}

	return -1;
}

const void *tinromSaveSection(uint32_t tag, size_t *length) {
	long at = findSection(tag);

	if (at < 0)
		return NULL;

	SectionHeader header;

	memcpy(&header, sections + at, sizeof(header));

	if (length)
		*length = header.length;

	return sections + at + sizeof(header);
}

TinromResult tinromSaveSetSection(
	uint32_t tag, const void *data, size_t length
) {
	long at = findSection(tag);

	/* Out with the old one, closing the gap. */
	if (at >= 0) {
		SectionHeader header;

		memcpy(&header, sections + at, sizeof(header));

		size_t size = sizeof(header) + padded(header.length);

		memmove(sections + at, sections + at + size,
			sectionBytes - (size_t) at - size);
		sectionBytes -= size;
		sectionCount--;
	}

	size_t size = sizeof(SectionHeader) + padded(length);

	if (sectionBytes + size > sizeof(sections))
		return TINROM_ERR_NO_SPACE;

	SectionHeader header = { tag, (uint32_t) length };

	memcpy(sections + sectionBytes, &header, sizeof(header));
	memcpy(sections + sectionBytes + sizeof(header), data, length);
	memset(sections + sectionBytes + sizeof(header) + length, 0,
		padded(length) - length);
	sectionBytes += size;
	sectionCount++;

	return TINROM_OK;
}

/* ASCII to the full-width Shift-JIS a save title is written in. */
static size_t encodeTitle(uint8_t *out, size_t room, const char *text) {
	size_t used = 0;

	for (; *text && (used + 2 <= room); text++) {
		char     c    = *text;
		uint16_t wide = 0x8140; /* space */

		if ((c >= 'A') && (c <= 'Z'))
			wide = (uint16_t) (0x8260 + (c - 'A'));
		else if ((c >= 'a') && (c <= 'z'))
			wide = (uint16_t) (0x8281 + (c - 'a'));
		else if ((c >= '0') && (c <= '9'))
			wide = (uint16_t) (0x824f + (c - '0'));
		else if (c == '-')
			wide = 0x817c;
		else if (c == '.')
			wide = 0x8144;

		out[used++] = (uint8_t) (wide >> 8);
		out[used++] = (uint8_t) wide;
	}

	return used;
}

TinromResult tinromSaveStore(unsigned int port, const TinromSaveFile *file) {
	memset(block, 0, sizeof(block));

	/* Title frame: "SC", one icon frame, one block, the title, the palette. */
	block[0] = 'S';
	block[1] = 'C';
	block[2] = 0x11;
	block[3] = 1;
	encodeTitle(block + 4, 64, file->title);
	memcpy(block + 0x60, iconPalette, sizeof(iconPalette));

	/* Icon frame. */
	memcpy(block + TINROM_MC_FRAME_SIZE, iconPixels, sizeof(iconPixels));

	Header header = {
		MAGIC, VERSION, (uint16_t) sectionCount, (uint32_t) sectionBytes,
		fnv(sections, sectionBytes)
	};

	memcpy(block + DATA_OFFSET, &header, sizeof(header));
	memcpy(block + DATA_OFFSET + sizeof(header), sections, sectionBytes);

	return tinromMcWriteFile(port, file->filename, block, sizeof(block));
}
