/* tinrom - (C) 2026 The tinrom contributors - MIT licensed */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/gpu.h"
#include "hal/console.h"
#include "libc/string.h"
#include "ps1/cop0.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

#define BIOS_BYTE(address) (*((const volatile char *) (address)))

#define BIOS_KERNEL_DATE  0xbfc00100u
#define BIOS_KERNEL_MAKER 0xbfc00108u

/*
 * Retail BIOSes from v2.0 on end with a "System ROM Version 4.1 12/16/97 E"
 * string. Its exact place varies between versions, so the last 100h bytes
 * are searched for it.
 */
#define BIOS_TAIL_START 0xbfc7ff00u
#define BIOS_TAIL_END   0xbfc80000u

static const char versionTag[] = "System ROM Version";

/* Copies the printable string at `address`; "" if it is not one. */
static void readText(uint32_t address, uint32_t end, char *out, size_t size) {
	size_t length = 0;

	while (((address + length) < end) && ((length + 1) < size)) {
		char c = BIOS_BYTE(address + length);

		if (!c)
			break;
		if ((c < 0x20) || (c > 0x7e)) {
			length = 0;
			break;
		}

		out[length++] = c;
	}

	out[length] = '\0';
}

/* The address of the version string, or 0. */
static uint32_t findVersion(void) {
	const unsigned int tagLength = sizeof(versionTag) - 1;

	for (uint32_t at = BIOS_TAIL_START; (at + tagLength) < BIOS_TAIL_END; at++) {
		unsigned int i;

		for (i = 0; i < tagLength; i++) {
			if (BIOS_BYTE(at + i) != versionTag[i])
				break;
		}

		if (i == tagLength)
			return at;
	}

	return 0;
}

/* The letter after the last space of the version string, if it is a region. */
static char regionOf(const char *version) {
	size_t length = strlen(version);

	if ((length < 2) || (version[length - 2] != ' '))
		return 0;

	switch (version[length - 1]) {
		case 'J': case 'A': case 'E': return version[length - 1];
		default:                      return 0;
	}
}

char tinromBiosRegion(void) {
	char     version[40];
	uint32_t at = findVersion();

	if (!at)
		return 0;

	readText(at, BIOS_TAIL_END, version, sizeof(version));

	return regionOf(version);
}

/* Eight BCD digits making a date from the console's years. */
static bool plausibleDate(uint32_t bcd) {
	for (unsigned int shift = 0; shift < 32; shift += 4) {
		if (((bcd >> shift) & 15) > 9)
			return false;
	}

	uint32_t year = bcd >> 16;

	return (year >= 0x1993) && (year <= 0x2010);
}

/*
 * The GPU's own answer to "what are you": GP1(10h) with index 07h. The
 * original 160-pin GPU does not know that index and leaves GPUREAD as it
 * was, so something known is latched there first - the drawing offset,
 * index 05h - and an unchanged read means no answer.
 */
static void identifyGpu(TinromConsoleInfo *info) {
	waitForGPUDMADone();

	GPU_GP1 = GP1_CMD_GET_INFO | 0x05;
	uint32_t before = GPU_GP0;

	GPU_GP1 = GP1_CMD_GET_INFO | 0x07;
	info->gpuType = GPU_GP0;

	info->gpuAnswered = (info->gpuType != before) || (info->gpuType == 2);
}

/*
 * How much RAM is fitted. A retail console has 2 MB, which the BIOS maps
 * four times over an 8 MB window, so a word 2 MB above another is the same
 * word; a development unit's 8 MB is 8 MB. Only tested while the window is
 * set up that way, as the BIOS leaves it: in any other layout the address
 * 2 MB up may be one that faults.
 */
static volatile uint32_t ramProbe;

static void measureRam(TinromConsoleInfo *info) {
	if ((DRAM_CTRL & DRAM_CTRL_SIZE_BITMASK) != DRAM_CTRL_SIZE_8MB)
		return;

	uint32_t physical = ((uint32_t) (uintptr_t) &ramProbe) & 0x1fffffffu;
	volatile uint32_t *here  = (volatile uint32_t *) (uintptr_t) (0xa0000000u | physical);
	volatile uint32_t *above = (volatile uint32_t *) (uintptr_t)
		(0xa0000000u | (physical + 0x200000u));

	uint32_t saved = *here;
	bool     mirrored;

	*here    = 0x5aa5c33cu;
	mirrored = (*above == 0x5aa5c33cu);
	*here    = 0xa55a3cc3u;
	mirrored = mirrored && (*above == 0xa55a3cc3u);
	*here    = saved;

	info->ramBytes = mirrored ? 0x200000u : 0x800000u;
}

void tinromConsoleIdentify(TinromConsoleInfo *info) {
	memset(info, 0, sizeof(*info));

	uint32_t date = (uint32_t) (uint8_t) BIOS_BYTE(BIOS_KERNEL_DATE)
		| ((uint32_t) (uint8_t) BIOS_BYTE(BIOS_KERNEL_DATE + 1) << 8)
		| ((uint32_t) (uint8_t) BIOS_BYTE(BIOS_KERNEL_DATE + 2) << 16)
		| ((uint32_t) (uint8_t) BIOS_BYTE(BIOS_KERNEL_DATE + 3) << 24);

	if (plausibleDate(date))
		info->kernelDate = date;

	readText(BIOS_KERNEL_MAKER, BIOS_KERNEL_MAKER + sizeof(info->kernelMaker),
		info->kernelMaker, sizeof(info->kernelMaker));

	uint32_t at = findVersion();

	if (at) {
		readText(at, BIOS_TAIL_END, info->version, sizeof(info->version));
		info->region = regionOf(info->version);
	}

	info->cpuId = cop0_getReg(COP0_PRID);

	identifyGpu(info);
	measureRam(info);
}
