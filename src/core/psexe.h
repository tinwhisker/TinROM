/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PS-EXE: the executable format the console's own loader uses, and the one
 * every disc, serial upload and memory card exploit ultimately hands over.
 *
 * The header is one 2048-byte sector; the payload is everything after it.
 *
 * Also here, because it is the other half of the same job: reading the BOOT
 * line out of a disc's SYSTEM.CNF, which is how a disc says which executable
 * is its game.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

#define TINROM_EXE_HEADER_SIZE 2048
#define TINROM_EXE_MAGIC       "PS-X EXE"

typedef struct {
	uint32_t pc;          /* entry point */
	uint32_t gp;          /* initial global pointer */
	uint32_t loadAddress; /* where the payload belongs in RAM */
	uint32_t loadSize;    /* payload bytes, excluding this header */
	uint32_t fillStart;   /* zero-fill region, often unused */
	uint32_t fillSize;
	uint32_t spBase;      /* initial stack pointer = base + offset */
	uint32_t spOffset;
} TinromExeHeader;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pulls the value of the BOOT line out of a SYSTEM.CNF. The file is a handful
 * of `KEY = value` lines with CRLF endings and inconsistent spacing; only
 * BOOT matters to us. Returns false if there is no BOOT line, or if its value
 * will not fit.
 */
bool tinromParseBootLine(
	const char *text, size_t length, char *out, size_t size
);

/**
 * Validates the 2048-byte header and fills `out`. Rejects anything whose load
 * region falls outside main RAM, because the alternative is a jump into
 * nowhere with the screen already handed over.
 */
TinromResult tinromExeParse(const void *headerSector, TinromExeHeader *out);

/**
 * Hands the console to a loaded executable and does not return.
 *
 * The payload must already be at `header->loadAddress`. Everything tinrom
 * owns has to be quiesced before this: see tinromHalShutdown().
 */
void tinromExeRun(const TinromExeHeader *header);

#ifdef __cplusplus
}
#endif
