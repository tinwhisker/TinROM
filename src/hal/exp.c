/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot/rom_header.h"
#include "core/log.h"
#include "hal/exp.h"
#include "hal/timer.h"
#include "libc/string.h"

/*
 * EXP1 is an 8-bit bus in its default configuration; read it a byte at a
 * time. Built with EXP_HOST - tools/test_exp - both ways through it are
 * functions the test supplies: carts of every kind, made of arrays.
 */
#ifdef EXP_HOST
uint8_t expHostRead(uint32_t offset);
void    expHostWrite(uint32_t offset, uint8_t value);

#define EXP_BYTE(offset)         expHostRead((uint32_t) (offset))
#define EXP_WRITE(offset, value) expHostWrite((uint32_t) (offset), (value))

/* The host's own C runtime has a _start; the test supplies these instead. */
#define _textStart expHostTextStart
#define _start     expHostStart
#else
#define EXP_BYTE(offset) \
	(*((volatile const uint8_t *) (0xbf000000u + (offset))))
#define EXP_WRITE(offset, value) \
	(*((volatile uint8_t *) (0xbf000000u + (offset))) = (value))
#endif

static bool     runningFromCart;
static uint32_t windowSize;

/* ps1-bare-metal's linker script, and crt0's entry point. */
extern uint8_t _textStart[];
extern void    _start(void);

static uint32_t expWord(uint32_t offset) {
	return (uint32_t) EXP_BYTE(offset) | ((uint32_t) EXP_BYTE(offset + 1) << 8)
		| ((uint32_t) EXP_BYTE(offset + 2) << 16)
		| ((uint32_t) EXP_BYTE(offset + 3) << 24);
}

/*
 * Whether this tinrom was copied out of a cart whose EXP1 window is smaller
 * than the image, which pcsx-redux's is: it maps 256K, so a bigger image's
 * tail reads back as its own start, and the stub copies that into RAM
 * without knowing. Everything past the window - the end of .rodata, .data,
 * .sdata - is then wrong, and tinrom fails in ways that point anywhere but
 * here. A window that small shows the descriptor again one window along;
 * a real 512K flash holds payload bytes there instead.
 */
/*
 * The words below must survive the very damage they describe, so they live
 * in .text, which comes first and is inside the window whatever its size.
 * .rodata, where strings normally go, ends exactly where the damage starts.
 */
#define SURVIVES __attribute__((section(".text.exp_strings"), aligned(4)))

static const char windowLine1[] SURVIVES =
	"exp: EXP1 repeats every %uK but the cart's image is %uK:";
static const char windowLine2[] SURVIVES =
	"exp: RAM past %08x is not tinrom (pcsx-redux maps 256K)";
/* Packed, the damage is not in one place: it unpacked wrong from there on. */
static const char windowLine2Packed[] SURVIVES =
	"exp: tinrom unpacked wrong from there on (pcsx-redux maps 256K)";

static void checkWindow(const TinromRomDescriptor *desc) {
	uint32_t end = desc->payloadOffset + tinromRomStoredSize(desc);

	for (uint32_t window = 0x10000; window < end; window <<= 1) {
		if ((expWord(window + TINROM_ROM_DESC_OFFSET) != TINROM_ROM_DESC_MAGIC)
			|| (expWord(window + TINROM_ROM_DESC_OFFSET + 12)
				!= desc->payloadSize))
			continue;

		windowSize = window;
		tinromLogWrite(windowLine1, (unsigned int) (window >> 10),
			(unsigned int) ((end + 1023) >> 10));
		if (desc->packedSize)
			tinromLogWrite(windowLine2Packed);
		else
			tinromLogWrite(windowLine2, (unsigned int) (desc->loadAddress
				+ window - desc->payloadOffset));
		return;
	}
}

void tinromExpInit(void) {
	TinromRomDescriptor desc;
	uint8_t            *bytes = (uint8_t *) &desc;

	for (size_t i = 0; i < sizeof(desc); i++)
		bytes[i] = EXP_BYTE(TINROM_ROM_DESC_OFFSET + i);

	/*
	 * Copied to where this image runs, entered where it starts: very likely
	 * this tinrom came from that cart. Not certain - the same build may also
	 * have been uploaded with a cart in - but the window check below is only
	 * a warning, and only for images that would not fit.
	 */
	runningFromCart = (desc.magic == TINROM_ROM_DESC_MAGIC)
		&& (desc.loadAddress == (uint32_t) (uintptr_t) _textStart)
		&& (desc.entryPC == (uint32_t) (uintptr_t) _start);

	if (runningFromCart)
		checkWindow(&desc);
}

bool tinromExpSwitchOn(TinromSwitchSource source) {
	uint32_t offset;

	switch (source) {
		case TINROM_SWITCH_XPLORER: offset = 0x60000; break;
		case TINROM_SWITCH_DATEL:   offset = 0x20018; break;
		default:                    return false;
	}

	unsigned int on = 0;

	for (unsigned int i = 0; i < 3; i++) {
		if (i)
			tinromDelayMicroseconds(1000);

		on += EXP_BYTE(offset) & 1;
	}

	return on >= 2;
}

uint32_t tinromExpTruncatedWindow(void) {
	return windowSize;
}

bool tinromExpRunningFromCart(void) {
	return runningFromCart;
}

TinromResult tinromExpReadHeader(void *buffer, size_t length) {
	if (!buffer || length > 0x100)
		return TINROM_ERR_BAD_ARG;

	return tinromFlashRead(0, buffer, length);
}

TinromResult tinromFlashRead(uint32_t offset, void *data, size_t length) {
	uint8_t *bytes = (uint8_t *) data;

	if (!data)
		return TINROM_ERR_BAD_ARG;
	if ((offset + length) > TINROM_ROM_SIZE)
		return TINROM_ERR_BAD_ARG;

	for (size_t i = 0; i < length; i++)
		bytes[i] = EXP_BYTE(offset + i);

	return TINROM_OK;
}

/* What the console's boot ROM looks for, the start of it. */
static const char licencePrefix[] = "Licensed by Sony";

static bool licenceAt(uint32_t offset) {
	for (unsigned int i = 0; i < sizeof(licencePrefix) - 1; i++) {
		if (EXP_BYTE(offset + i) != (uint8_t) licencePrefix[i])
			return false;
	}

	return true;
}

TinromResult tinromExpCartInfo(TinromCartInfo *info) {
	if (!info)
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	info->header = licenceAt(0x004) || licenceAt(0x084);

	TinromRomDescriptor desc;
	uint8_t            *bytes = (uint8_t *) &desc;

	for (size_t i = 0; i < sizeof(desc); i++)
		bytes[i] = EXP_BYTE(TINROM_ROM_DESC_OFFSET + i);

	if (desc.magic != TINROM_ROM_DESC_MAGIC)
		return TINROM_OK;

	info->tinrom      = true;
	info->version     = desc.version;
	info->payloadSize = desc.payloadSize;
	info->packedSize  = desc.packedSize;
	info->imageEnd    = desc.payloadOffset + tinromRomStoredSize(&desc);

	return TINROM_OK;
}

/* --- the flash chip -------------------------------------------------------
 *
 * The JEDEC software-ID sequence: three writes - AAh to 5555h, 55h to 2AAAh,
 * 90h to 5555h - put a flash chip in its ID mode, where offset 0 reads its
 * maker and offset 1 the part; the same three with F0h last put it back.
 * Those are the addresses the 128K-512K parts carts use all decode (a
 * 29F040 ignores the bits above 7FFh), and both lie in a Datel cart's first
 * 128K, which is mapped straight through.
 *
 * Not every cart chip is a flash chip. An EEPROM such as a 28C010, or an
 * AT29C with its write protection off, takes a write as data to store - so
 * the sequence would overwrite what is at 2AAAh and 5555h. So the 256 bytes
 * around each are kept first (256: the largest page among the chips carts
 * use), compared afterwards, and anything that changed is written back and
 * read back to check. What changed says how: one byte on a chip that writes
 * bytes; a whole page on one that rewrites pages, which is written back as
 * a page, in one burst, as such chips take one.
 */
#define FLASH_UNLOCK_1 0x5555
#define FLASH_UNLOCK_2 0x2aaa
#define FLASH_ID_ENTER 0x90
#define FLASH_ID_EXIT  0xf0
/* Atmel's parts take up to 10 ms to switch modes. */
#define FLASH_MODE_US  10000

static void flashCommand(uint8_t command) {
	EXP_WRITE(FLASH_UNLOCK_1, 0xaa);
	EXP_WRITE(FLASH_UNLOCK_2, 0x55);
	EXP_WRITE(FLASH_UNLOCK_1, command);
}

typedef struct {
	uint8_t     maker, device;
	const char *name;
	uint32_t    size;
} FlashChip;

/* From the makers' datasheets. A pair not listed is shown by its numbers. */
static const FlashChip flashChips[] = {
	{ 0x01, 0x20, "AMD Am29F010",       0x20000 },
	{ 0x01, 0xa4, "AMD Am29F040",       0x80000 },
	{ 0x04, 0xa4, "Fujitsu MBM29F040",  0x80000 },
	{ 0x1f, 0xd5, "Atmel AT29C010A",    0x20000 },
	{ 0x1f, 0xda, "Atmel AT29C020",     0x40000 },
	{ 0x1f, 0xa4, "Atmel AT29C040A",    0x80000 },
	{ 0x20, 0xe2, "ST M29F040",         0x80000 },
	{ 0xad, 0xa4, "Hynix HY29F040",     0x80000 },
	{ 0xbf, 0x07, "SST 29EE010",        0x20000 },
	{ 0xbf, 0x10, "SST 29EE020",        0x40000 },
	{ 0xbf, 0xb5, "SST 39SF010A",       0x20000 },
	{ 0xbf, 0xb6, "SST 39SF020A",       0x40000 },
	{ 0xbf, 0xb7, "SST 39SF040",        0x80000 },
	{ 0xc2, 0xa4, "Macronix MX29F040",  0x80000 },
	{ 0xda, 0xc1, "Winbond W29EE011",   0x20000 }
};

static const struct {
	uint8_t     maker;
	const char *name;
} flashMakers[] = {
	{ 0x01, "AMD" },      { 0x04, "Fujitsu" }, { 0x1f, "Atmel" },
	{ 0x20, "ST" },       { 0x31, "Catalyst" }, { 0x89, "Intel" },
	{ 0xad, "Hynix" },    { 0xbf, "SST" },     { 0xc2, "Macronix" },
	{ 0xda, "Winbond" }
};

const char *tinromFlashMakerName(uint8_t maker) {
	for (size_t i = 0; i < sizeof(flashMakers) / sizeof(flashMakers[0]); i++) {
		if (flashMakers[i].maker == maker)
			return flashMakers[i].name;
	}

	return NULL;
}

/* --- putting back what an EEPROM stored ----------------------------------- */

#define GUARD_BLOCK   0x100
#define GUARD_BLOCKS  2
/* A write cycle is up to 10 ms on these parts; twice that, to be sure. */
#define WRITE_WAIT_US 20000

static const uint32_t guardBase[GUARD_BLOCKS] = {
	FLASH_UNLOCK_2 & ~(GUARD_BLOCK - 1), FLASH_UNLOCK_1 & ~(GUARD_BLOCK - 1)
};
static uint8_t guardCopy[GUARD_BLOCKS][GUARD_BLOCK];

static void guardSave(void) {
	for (unsigned int b = 0; b < GUARD_BLOCKS; b++) {
		for (unsigned int i = 0; i < GUARD_BLOCK; i++)
			guardCopy[b][i] = EXP_BYTE(guardBase[b] + i);
	}
}

static bool guardIntact(unsigned int block) {
	for (unsigned int i = 0; i < GUARD_BLOCK; i++) {
		if (EXP_BYTE(guardBase[block] + i) != guardCopy[block][i])
			return false;
	}

	return true;
}

/*
 * Waits for a write to land: until `offset` reads back as `value`. Not
 * before a page chip's load window - 150 us after the last byte - has
 * closed, though: until then nothing has been programmed, and a byte whose
 * old value happens to be the new one would read as landed already.
 */
#define LOAD_WINDOW_US 200

static bool writeLanded(uint32_t offset, uint8_t value) {
	tinromDelayMicroseconds(LOAD_WINDOW_US);

	for (unsigned int waited = 0; waited < WRITE_WAIT_US; waited += 100) {
		if (EXP_BYTE(offset) == value)
			return true;

		tinromDelayMicroseconds(100);
	}

	return EXP_BYTE(offset) == value;
}

/*
 * Writes one block's changes back. One changed byte goes back alone. More
 * than one means a chip that rewrote a whole page: the smallest aligned
 * page (64, 128 or 256 bytes) holding every change goes back as a burst -
 * all of it, unchanged bytes too, since such a chip programs every byte of
 * the page it is given, and back to back, since it waits only so long for
 * the next.
 */
static bool guardRestore(unsigned int block) {
	uint32_t base  = guardBase[block];
	int      first = -1, last = -1;

	for (int i = 0; i < GUARD_BLOCK; i++) {
		if (EXP_BYTE(base + (uint32_t) i) != guardCopy[block][i]) {
			if (first < 0)
				first = i;

			last = i;
		}
	}

	if (first < 0)
		return true;

	if (first == last) {
		EXP_WRITE(base + (uint32_t) first, guardCopy[block][first]);

		return writeLanded(base + (uint32_t) first, guardCopy[block][first]);
	}

	unsigned int page = 64;

	while ((page < GUARD_BLOCK) && ((first / (int) page) != (last / (int) page)))
		page *= 2;

	unsigned int start = ((unsigned int) first / page) * page;

	for (unsigned int i = start; i < start + page; i++)
		EXP_WRITE(base + i, guardCopy[block][i]);

	unsigned int end = start + page - 1;

	(void) writeLanded(base + end, guardCopy[block][end]);

	return guardIntact(block);
}

TinromResult tinromFlashIdentify(TinromFlashInfo *info) {
	if (!info)
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	uint8_t normal[2] = { EXP_BYTE(0), EXP_BYTE(1) };

	guardSave();

	flashCommand(FLASH_ID_ENTER);
	tinromDelayMicroseconds(FLASH_MODE_US);

	uint8_t maker  = EXP_BYTE(0);
	uint8_t device = EXP_BYTE(1);

	flashCommand(FLASH_ID_EXIT);
	tinromDelayMicroseconds(FLASH_MODE_US);

	/* Let any write an EEPROM started finish before judging. */
	tinromDelayMicroseconds(WRITE_WAIT_US);

	if (!guardIntact(0) || !guardIntact(1)) {
		/* Both, whatever becomes of the first. */
		bool first  = guardRestore(0);
		bool second = guardRestore(1);

		info->storedAsData = true;
		info->restored     = first && second;

		TINROM_LOG("exp: the cart stored the ID commands as data - %s",
			info->restored ? "written back" : "NOT written back");

		return TINROM_ERR_CORRUPT;
	}

	if ((EXP_BYTE(0) != normal[0]) || (EXP_BYTE(1) != normal[1])) {
		TINROM_LOG("exp: the cart's chip did not leave its ID mode");

		return TINROM_ERR_IO;
	}

	/* Nothing changed: no chip that answers the sequence, or no writes. */
	if ((maker == normal[0]) && (device == normal[1]))
		return TINROM_ERR_UNSUPPORTED;

	info->present        = true;
	info->writable       = true;
	info->manufacturerId = maker;
	info->deviceId       = device;

	for (size_t i = 0; i < sizeof(flashChips) / sizeof(flashChips[0]); i++) {
		if ((flashChips[i].maker == maker) && (flashChips[i].device == device)) {
			info->name = flashChips[i].name;
			info->size = flashChips[i].size;
		}
	}

	TINROM_LOG("exp: cart flash %02x/%02x %s", maker, device,
		info->name ? info->name : "(not in the table)");

	return TINROM_OK;
}

TinromResult tinromFlashEraseSector(uint32_t offset) {
	(void) offset;

	return TINROM_ERR_UNIMPLEMENTED;
}

TinromResult tinromFlashWrite(uint32_t offset, const void *data, size_t length) {
	(void) offset;
	(void) data;
	(void) length;

	return TINROM_ERR_UNIMPLEMENTED;
}
