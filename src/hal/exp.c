/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "boot/rom_header.h"
#include "core/log.h"
#include "hal/exp.h"
#include "hal/timer.h"
#include "libc/string.h"

/* EXP1 is an 8-bit bus in its default configuration; read it a byte at a time. */
#define EXP_BYTE(offset) \
	(*((volatile const uint8_t *) (0xbf000000u + (offset))))

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

TinromResult tinromFlashIdentify(TinromFlashInfo *info) {
	/*
	 * STUB. The JEDEC software-ID sequence is three unlock writes followed by
	 * reads of offsets 0 and 1; the flasher module holds the per-family
	 * command table. Writing to EXP1 needs the bus to be writable, which on
	 * most carts means a jumper or a register on the cart itself.
	 */
	if (!info)
		return TINROM_ERR_BAD_ARG;

	memset(info, 0, sizeof(*info));

	return TINROM_ERR_UNIMPLEMENTED;
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
