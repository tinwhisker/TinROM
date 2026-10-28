/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * What console this is: the boot ROM's version text, the CPU and GPU
 * revisions, and how much RAM there is. All of it read, none of it set.
 *
 * The BIOS's strings are read as text, the same way a hex dump would show
 * them, and nothing of the BIOS is kept or copied - see NOTICE.md. Where
 * they live is psx-spx's "BIOS Versions".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	/* The kernel's build date at BFC00100h, as BCD yyyymmdd; 0 if absent. */
	uint32_t kernelDate;
	/* Its maker string at BFC00108h, e.g. "CEX-3000 KT-3  by K.S."; "" if absent. */
	char     kernelMaker[40];
	/* "System ROM Version 4.1 12/16/97 E" from near the ROM's end; "" if absent. */
	char     version[40];
	/* The version string's last letter: 'J', 'A' or 'E'; 0 if unknown. */
	char     region;

	/* COP0 PRId, the processor revision. */
	uint32_t cpuId;
	/* GP1(10h:07h)'s answer: 2 on the later 208-pin GPU. */
	uint32_t gpuType;
	/* False if the GPU did not answer 07h, as the original 160-pin one does not. */
	bool     gpuAnswered;

	/* Bytes of RAM fitted: 2 MB, or 8 MB on a development unit; 0 if not tested. */
	uint32_t ramBytes;
} TinromConsoleInfo;

#ifdef __cplusplus
extern "C" {
#endif

/** Fills `info` in. Reads the GPU, so call it between frames, not inside one. */
void tinromConsoleIdentify(TinromConsoleInfo *info);

/**
 * Just the region letter from the BIOS's version string: 'J', 'A' or 'E',
 * or 0 if it has none or another. Cheap enough for start-up.
 */
char tinromBiosRegion(void);

#ifdef __cplusplus
}
#endif
