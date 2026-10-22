/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * See hal/crash.h, and crash/crash_api.h for the contract with the screen.
 */

#include <stdbool.h>
#include <stdint.h>
#include "boot/rom_header.h"
#include "core/log.h"
#include "crash/crash_api.h"
#include "hal/crash.h"
#include "hal/exception.h"
#include "hal/exp.h"

/* Where the cart's bytes are, seen uncached: what the screen is linked for. */
#define ROM_UNCACHED (0xa0000000u | (TINROM_ROM_BASE & 0x1fffffffu))

uint32_t tinromCrashScreenEntry(void) {
	TinromRomDescriptor desc;
	TinromCrashHeader   header;

	if ((tinromFlashRead(TINROM_ROM_DESC_OFFSET, &desc, sizeof(desc))
			!= TINROM_OK)
		|| (desc.magic != TINROM_ROM_DESC_MAGIC) || !desc.crashOffset
		|| (desc.crashOffset >= 0x10000))
		return 0;

	if ((tinromFlashRead(desc.crashOffset, &header, sizeof(header))
			!= TINROM_OK)
		|| (header.magic != TINROM_CRASH_MAGIC)
		|| (header.version != TINROM_CRASH_VERSION))
		return 0;

	/*
	 * Built for where it sits - a cart flashed with an image for another
	 * base would jump somewhere else entirely - and in one piece.
	 */
	uint32_t base = ROM_UNCACHED + desc.crashOffset;

	if ((header.base != base) || (header.end <= base)
		|| ((header.end - base) > TINROM_CRASH_MAX_SIZE)
		|| (header.entry < base) || (header.entry >= header.end))
		return 0;

	return header.entry;
}

/*
 * What A(40h) may point at before it is ours: somewhere in the kernel's
 * RAM, below where programs load, or in the BIOS ROM - through any segment
 * but KSEG2 - or a crash screen this cart had before it was flashed again.
 */
static bool replaceable(uint32_t address) {
	uint32_t physical = address & 0x1fffffffu;

	if ((address & 3) || (address >= 0xc0000000u)
		|| ((address >= 0x20000000u) && (address < 0x80000000u)))
		return false;

	return (physical < 0x00010000u)
		|| ((physical >= 0x1fc00000u) && (physical < 0x1fc80000u))
		|| ((address >= ROM_UNCACHED) && (address < (ROM_UNCACHED + 0x10000u)));
}

bool tinromCrashScreenArm(void) {
	uint32_t entry = tinromCrashScreenEntry();

	if (!entry)
		return false;

	/*
	 * TINROM_KERNEL_CALL, of a kind: not a call but a write into the
	 * kernel's A(nnh) jump table, so that the kernel's own call to A(40h)
	 * lands in the cart. Everything the kernel does before that - saving the
	 * registers, its handlers, DeliverEvent - is left as it was.
	 */
	volatile uint32_t *slot = (volatile uint32_t *) TINROM_KERNEL_A40_ENTRY;
	uint32_t           was  = *slot;

	if (was == entry)
		return true;

	if (!replaceable(was)) {
		TINROM_LOG("crash: A(40h) is %08x, which does not look like the "
			"kernel's; leaving it", (unsigned int) was);
		return false;
	}

	*slot = entry;
	TINROM_LOG("crash: A(40h) %08x -> the cart's crash screen at %08x",
		(unsigned int) was, (unsigned int) entry);

	return true;
}

TinromResult tinromCrashScreenTest(void) {
	if (!tinromCrashScreenArm())
		return TINROM_ERR_NOT_PRESENT;

	/* The kernel has to be the one that sees it, as it would for a game. */
	tinromExceptionRemove();
	TINROM_LOG("crash: crashing on purpose");

	/* An unaligned load, from an address that says what it is. */
	(void) *(volatile const uint32_t *) 0x0badc0deu;

	/* Not reached on a console: the load cannot succeed. */
	return TINROM_ERR_IO;
}
