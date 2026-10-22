/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PCSX-Redux debug console driver.
 *
 * Register map (psx-spx, "I/O Map", Expansion Region 2; and the PCSX-Redux
 * MIPS API documentation):
 *
 *   1F802080h  4  Redux-Expansion ID 'PCSX'      (R)
 *   1F802080h  1  Redux-Expansion Console putchar (W)
 *   1F802081h  1  Redux-Expansion Debug break     (W)
 *   1F802082h  1  Redux-Expansion Exit code       (W)
 *
 * The BIOS leaves EXP2 128 bytes wide (BIU_DEV8_DELAY size field = 7), which
 * stops one address short of all of this - the POST register the ROM stub
 * traces to lives at +41h and is inside the window, these are not. Redux's
 * own crt0.s widens the field to 8 (256 bytes) before touching them, so we do
 * the same, and put it back if nothing answers. Nothing on a retail console
 * decodes EXP2 at all, so the widened window is harmless there either way.
 */

#include <stdbool.h>
#include <stdint.h>
#include "hal/dbgcon.h"
#include "ps1/registers.h"

#define DBGCON_ID    _MMIO32(DEV8_BASE | 0x080)
#define DBGCON_PUTC  _MMIO8 (DEV8_BASE | 0x080)
#define DBGCON_BREAK _MMIO8 (DEV8_BASE | 0x081)

/* 'P', 'C', 'S', 'X' as they sit in memory, read back little-endian. */
#define DBGCON_ID_PCSX 0x58534350

#define DBGCON_SIZE_256 (8 << 16)

static bool        consolePresent;
static const char *consoleName;

bool tinromDbgConsoleInit(void) {
	uint32_t saved = BIU_DEV8_DELAY;

	consolePresent = false;
	consoleName    = (const char *) 0;

	if ((saved & BIU_DEV_DELAY_ADDR_BITS_BITMASK) < DBGCON_SIZE_256)
		BIU_DEV8_DELAY = (saved & ~BIU_DEV_DELAY_ADDR_BITS_BITMASK) | DBGCON_SIZE_256;

	if (DBGCON_ID == DBGCON_ID_PCSX) {
		consolePresent = true;
		consoleName    = "PCSX-Redux";
	} else {
		BIU_DEV8_DELAY = saved;
	}

	return consolePresent;
}

bool tinromDbgConsolePresent(void) {
	return consolePresent;
}

const char *tinromDbgConsoleName(void) {
	return consoleName;
}

void tinromDbgConsoleWriteByte(uint8_t value) {
	if (consolePresent)
		DBGCON_PUTC = value;
}

void tinromDbgConsoleWriteString(const char *string) {
	if (!consolePresent)
		return;

	while (*string)
		DBGCON_PUTC = (uint8_t) *(string++);
}

void tinromDbgConsoleBreak(void) {
	if (consolePresent)
		DBGCON_BREAK = 1;
}
