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

/** STUB. JEDEC software-ID probe. */
TinromResult tinromFlashIdentify(TinromFlashInfo *info);
/** STUB. */
TinromResult tinromFlashEraseSector(uint32_t offset);
/** STUB. */
TinromResult tinromFlashWrite(uint32_t offset, const void *data, size_t length);
/** Reading is just a bus read; this one is real. */
TinromResult tinromFlashRead(uint32_t offset, void *data, size_t length);

#ifdef __cplusplus
}
#endif
