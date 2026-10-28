/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

/*
 * The EXP1 expansion bus: cheat carts and their flash EEPROMs.
 *
 * tinrom needs three things here: knowing whether it is running from a cart,
 * identifying the EEPROM so the flasher can pick a command sequence, and
 * reserving a sector for the settings block.
 *
 * The JEDEC identify/erase/program sequences are chip-vendor documented and
 * live in a table (see flasher module). Nothing here is device-specific.
 */

#ifndef TINROM_ROM_BASE
#define TINROM_ROM_BASE 0x1f000000
#endif
#ifndef TINROM_ROM_SIZE
#define TINROM_ROM_SIZE 0x80000
#endif

typedef struct {
	bool        present;
	uint8_t     manufacturerId;
	uint8_t     deviceId;
	const char *name;        /* NULL if the id pair is not in the table */
	uint32_t    size;        /* bytes, 0 if unknown */
	uint32_t    sectorSize;  /* smallest erasable unit */
	bool        writable;
	/*
	 * Set with TINROM_ERR_CORRUPT: the chip took the ID command as data to
	 * store - an EEPROM, not flash - and whether what it overwrote has been
	 * written back and read back the same.
	 */
	bool        storedAsData;
	bool        restored;
} TinromFlashInfo;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where a cart's switch can be read, from psx-spx "Cheat Devices". Both are
 * bit 0 of a byte, 1 for on. Nothing on a cart says which kind it is, and
 * on one without that register the address reads as ROM - so which to use
 * is the user's to say, and none is the default.
 */
typedef enum {
	TINROM_SWITCH_NONE    = 0,
	TINROM_SWITCH_XPLORER = 1, /* 1F060000h: Xplorer, and pcsx-redux */
	TINROM_SWITCH_DATEL   = 2, /* 1F020018h: Action Replay, GameShark */
	TINROM_SWITCH_COUNT   = 3
} TinromSwitchSource;

void         tinromExpInit(void);
/**
 * Reads the switch: true if `source` says how and it is on. Takes the
 * majority of three reads a millisecond apart, so a switch caught moving
 * reads one way or the other rather than flickering.
 */
bool         tinromExpSwitchOn(TinromSwitchSource source);
/** True if a tinrom image is the thing currently mapped at EXP1. */
bool         tinromExpRunningFromCart(void);
/**
 * Non-zero if this tinrom was copied from a cart whose EXP1 window (this
 * many bytes) is smaller than its image - pcsx-redux's 256K - so what was
 * copied past it is not tinrom. 0 otherwise.
 */
uint32_t     tinromExpTruncatedWindow(void);
/** Read the EXP1 ROM header and report whether it looks like a tinrom build. */
TinromResult tinromExpReadHeader(void *buffer, size_t length);

typedef struct {
	/* The console's boot header: the licence ID at +004h or +084h. */
	bool     header;
	/* And a tinrom image's descriptor at +100h; the rest is from it. */
	bool     tinrom;
	uint32_t version;     /* major << 16 | minor << 8 | patch */
	uint32_t payloadSize; /* unpacked */
	uint32_t packedSize;  /* 0 for a payload stored as it is */
	uint32_t imageEnd;    /* ROM bytes the image takes */
} TinromCartInfo;

/** What is at EXP1 now, from its header. Reads only. */
TinromResult tinromExpCartInfo(TinromCartInfo *info);

/**
 * The JEDEC software-ID probe: the cart flash chip's maker and part, and a
 * name and size from a table of the parts carts use.
 *
 * WRITES to the cart: three command bytes in, three out. Harmless to a flash
 * chip; an EEPROM, or an AT29C with its protection off, stores them as data
 * instead. The bytes around them are kept beforehand, and if any changed
 * they are written back and checked: TINROM_ERR_CORRUPT, with `restored`
 * saying whether that worked - if not, the cart wants flashing again.
 * TINROM_ERR_UNSUPPORTED: nothing answered, either no such chip or a cart
 * that does not pass writes. TINROM_ERR_IO: the chip answered but did not
 * go back to reading as ROM.
 */
TinromResult tinromFlashIdentify(TinromFlashInfo *info);
/** The JEDEC maker's name, or NULL if it is not one carts use. */
const char  *tinromFlashMakerName(uint8_t maker);
/** STUB. */
TinromResult tinromFlashEraseSector(uint32_t offset);
/** STUB. */
TinromResult tinromFlashWrite(uint32_t offset, const void *data, size_t length);
/** Reading is just a bus read; this one is real. */
TinromResult tinromFlashRead(uint32_t offset, void *data, size_t length);

#ifdef __cplusplus
}
#endif
