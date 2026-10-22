/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The address spaces the hex editor can show, and how each is read and
 * written. Memory regions are plain loads and stores; the memory cards go
 * through the frame copy in card.c.
 */

#include "common/cache.h"
#include "modules/hexedit/hexedit.h"
#include "monitor/monitor_api.h"

#ifndef TINROM_ROM_SIZE
#define TINROM_ROM_SIZE 0x80000
#endif

const HexRegion hexRegions[] = {
	{ "Main RAM",    "80000000h, 2 MB",
		HEX_REGION_MEMORY, 0x80000000u, 0x200000u, true,  0, 0x1000 },
	{ "Scratchpad",  "1F800000h, 1 KB",
		HEX_REGION_MEMORY, 0x1f800000u, 0x400u,    true,  0, 0x100 },
	{ "BIOS ROM",    "BFC00000h, 512 KB, read only",
		HEX_REGION_MEMORY, 0xbfc00000u, 0x80000u,  false, 0, 0x1000 },
	{ "Cart EXP1",   "1F000000h, read only; the flasher writes it",
		HEX_REGION_MEMORY, 0x1f000000u, TINROM_ROM_SIZE, false, 0, 0x1000 },
	{ "Memory card 1", "Frame by frame; written back on request",
		HEX_REGION_CARD,   0,           0x20000u,  true,  0, 0x2000 },
	{ "Memory card 2", "Frame by frame; written back on request",
		HEX_REGION_CARD,   0,           0x20000u,  true,  1, 0x2000 }
};

const unsigned hexRegionCount = sizeof(hexRegions) / sizeof(hexRegions[0]);

bool hexRead(const HexRegion *region, uint32_t offset, uint32_t size,
	uint32_t *value) {
	if ((region->kind != HEX_REGION_MEMORY) || ((offset + size) > region->size))
		return false;

	uint32_t address = region->base + offset;

	/* Aligned accesses of the unit's own size; everything else by byte. */
	if ((size == 4) && !(address & 3)) {
		*value = *(volatile const uint32_t *) address;
	} else if ((size == 2) && !(address & 1)) {
		*value = *(volatile const uint16_t *) address;
	} else {
		uint32_t result = 0;

		for (uint32_t i = 0; i < size; i++)
			result |= (uint32_t) *(volatile const uint8_t *) (address + i)
				<< (i * 8);

		*value = result;
	}

	return true;
}

bool hexWrite(const HexRegion *region, uint32_t offset, uint32_t size,
	uint32_t value) {
	if (
		(region->kind != HEX_REGION_MEMORY) || !region->writable
		|| ((offset + size) > region->size)
	)
		return false;

	uint32_t address = region->base + offset;

	if ((size == 4) && !(address & 3)) {
		*(volatile uint32_t *) address = value;
	} else if ((size == 2) && !(address & 1)) {
		*(volatile uint16_t *) address = (uint16_t) value;
	} else {
		for (uint32_t i = 0; i < size; i++)
			*(volatile uint8_t *) (address + i) = (uint8_t) (value >> (i * 8));
	}

	/* It may have been an instruction. */
	flushCache();

	return true;
}

/* ps1-bare-metal's linker script. */
extern uint8_t _textStart[];
extern uint8_t _bssEnd[];

const char *hexRamOwner(const HexRegion *region, uint32_t offset) {
	if ((region->kind != HEX_REGION_MEMORY) || (region->base != 0x80000000u))
		return NULL;

	uint32_t tinromStart = (uint32_t) (uintptr_t) _textStart & 0x1fffffu;
	uint32_t tinromEnd   = (uint32_t) (uintptr_t) _bssEnd    & 0x1fffffu;
	uint32_t monitor     = TINROM_MONITOR_BASE & 0x1fffffu;

	if (offset < 0x10000u)
		return "kernel";
	if ((offset >= tinromStart) && (offset < tinromEnd))
		return "tinrom";
	if ((offset >= monitor) && (offset < (monitor + TINROM_MONITOR_SIZE)))
		return "monitor";

	return NULL;
}

bool hexAddressToOffset(const HexRegion *region, uint32_t address,
	uint32_t *offset) {
	if (region->kind == HEX_REGION_CARD) {
		if (address >= region->size)
			return false;

		*offset = address;

		return true;
	}

	/* Main RAM answers at every mirror in KUSEG, KSEG0 and KSEG1. */
	if (region->base == 0x80000000u) {
		if (address >= 0xc0000000u)
			return false;

		uint32_t physical = address & 0x1fffffffu;

		if (physical >= 0x00800000u)
			return false;

		*offset = physical & (region->size - 1);

		return true;
	}

	/* The rest are compared by physical address. */
	uint32_t physical = address & 0x1fffffffu;
	uint32_t start    = region->base & 0x1fffffffu;

	if ((address >= 0xc0000000u) || (physical < start)
		|| (physical >= (start + region->size)))
		return false;

	*offset = physical - start;

	return true;
}
