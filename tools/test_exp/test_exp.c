/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * Host-side test of the cart code that writes to the cart: identifying its
 * flash chip (hal/exp.c), against carts made of arrays - a flash chip that
 * answers the JEDEC ID sequence, EEPROMs that store it as data instead (one
 * byte at a time, or a page at a time, unloaded bytes erased), a cart that
 * ignores writes, and a chip that will not leave its ID mode. Also what a
 * cart's header says. Run by scripts/test-exp.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot/rom_header.h"
#include "hal/exp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* --- the cart ------------------------------------------------------------ */

typedef enum { FLASH, EEPROM_BYTE, EEPROM_PAGE, READ_ONLY, STUCK } Kind;

#define ROM_SIZE 0x20000

static uint8_t  rom[ROM_SIZE], original[ROM_SIZE];
static Kind     kind;
static uint8_t  maker, device;
static unsigned page;          /* EEPROM_PAGE's page size */
static unsigned cycle;         /* where in the unlock sequence a flash chip is */
static int      idMode;
static unsigned writes;

/* A page chip's page being loaded: programmed when time passes. */
static int      loading = -1;
static uint8_t  loaded[256];
static int      isLoaded[256];

static void commitPage(void) {
	if (loading < 0)
		return;

	/* Every byte of the page is programmed; one not loaded is erased. */
	for (unsigned i = 0; i < page; i++)
		rom[(unsigned) loading + i] = isLoaded[i] ? loaded[i] : 0xff;

	loading = -1;
}

uint8_t expHostRead(uint32_t offset) {
	if (idMode && (offset == 0)) return maker;
	if (idMode && (offset == 1)) return device;
	return rom[offset % ROM_SIZE];
}

void expHostWrite(uint32_t offset, uint8_t value) {
	writes++;
	offset %= ROM_SIZE;

	switch (kind) {
		case FLASH:
		case STUCK:
			if ((cycle == 0) && (offset == 0x5555) && (value == 0xaa)) { cycle = 1; return; }
			if ((cycle == 1) && (offset == 0x2aaa) && (value == 0x55)) { cycle = 2; return; }
			if ((cycle == 2) && (offset == 0x5555)) {
				if (value == 0x90) idMode = 1;
				if ((value == 0xf0) && (kind == FLASH)) idMode = 0;
			}
			cycle = 0;
			return;

		case EEPROM_BYTE:
			rom[offset] = value;
			return;

		case EEPROM_PAGE: {
			int base = (int) (offset & ~(page - 1));

			if (loading != base) {
				commitPage();
				loading = base;
				memset(isLoaded, 0, sizeof(isLoaded));
			}
			loaded[offset - (unsigned) base]   = value;
			isLoaded[offset - (unsigned) base] = 1;
			return;
		}

		case READ_ONLY:
			return;
	}
}

/* Time passing: a page chip's load window closes. */
void tinromDelayMicroseconds(int us) { (void) us; commitPage(); }

void tinromLogWrite(const char *format, ...) { (void) format; }
uint8_t expHostTextStart[1];
void    expHostStart(void) {}

static void fit(Kind k, uint8_t m, uint8_t d, unsigned p) {
	kind = k; maker = m; device = d; page = p;
	cycle = 0; idMode = 0; loading = -1; writes = 0;

	/* Something like a cart image, with some FFh in it as real ones have. */
	unsigned x = 12345;
	for (unsigned i = 0; i < ROM_SIZE; i++) {
		x = x * 1103515245u + 12345u;
		rom[i] = ((x >> 16) % 7 == 0) ? 0xff : (uint8_t) (x >> 16);
	}
	memcpy(original, rom, sizeof(rom));
}

int main(void) {
	TinromFlashInfo info;

	puts("flash");
	fit(FLASH, 0x01, 0x20, 0);
	CHECK(tinromFlashIdentify(&info) == TINROM_OK);
	CHECK(info.present && info.manufacturerId == 0x01 && info.deviceId == 0x20);
	CHECK(info.name && !strcmp(info.name, "AMD Am29F010") && info.size == 0x20000);
	CHECK(!idMode && !memcmp(rom, original, ROM_SIZE));

	fit(FLASH, 0xbf, 0x99, 0);
	CHECK(tinromFlashIdentify(&info) == TINROM_OK);
	CHECK(!info.name && !info.size && !strcmp(tinromFlashMakerName(0xbf), "SST"));

	puts("an EEPROM that writes bytes");
	fit(EEPROM_BYTE, 0, 0, 0);
	CHECK(tinromFlashIdentify(&info) == TINROM_ERR_CORRUPT);
	CHECK(info.storedAsData && info.restored);
	CHECK(!memcmp(rom, original, ROM_SIZE));

	puts("EEPROMs that write pages");
	for (unsigned p = 64; p <= 256; p *= 2) {
		fit(EEPROM_PAGE, 0, 0, p);
		CHECK(tinromFlashIdentify(&info) == TINROM_ERR_CORRUPT);
		CHECK(info.storedAsData && info.restored);
		CHECK(!memcmp(rom, original, ROM_SIZE));
		if (memcmp(rom, original, ROM_SIZE)) printf("    with %u-byte pages\n", p);
	}

	puts("no answer");
	fit(READ_ONLY, 0, 0, 0);
	CHECK(tinromFlashIdentify(&info) == TINROM_ERR_UNSUPPORTED);
	CHECK(!info.present && !memcmp(rom, original, ROM_SIZE));

	puts("stuck in its ID mode");
	fit(STUCK, 0x01, 0xa4, 0);
	CHECK(tinromFlashIdentify(&info) == TINROM_ERR_IO);

	puts("the cart's header");
	{
		TinromCartInfo cart;

		fit(READ_ONLY, 0, 0, 0);
		memset(rom, 0xff, 0x200);
		CHECK(tinromExpCartInfo(&cart) == TINROM_OK && !cart.header && !cart.tinrom);

		memcpy(&rom[0x84], "Licensed by Sony Computer Entertainment Inc.", 44);
		CHECK(tinromExpCartInfo(&cart) == TINROM_OK && cart.header && !cart.tinrom);

		TinromRomDescriptor desc = { 0 };
		desc.magic = TINROM_ROM_DESC_MAGIC;
		desc.version = (0 << 16) | (1 << 8) | 0;
		desc.payloadOffset = 0x1800;
		desc.payloadSize = 233472;
		desc.packedSize = 117303;
		memcpy(&rom[TINROM_ROM_DESC_OFFSET], &desc, sizeof(desc));
		CHECK(tinromExpCartInfo(&cart) == TINROM_OK && cart.tinrom);
		CHECK(cart.version == 0x100 && cart.payloadSize == 233472
			&& cart.packedSize == 117303 && cart.imageEnd == 0x1800 + 117303);
	}

	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);

	return fails ? 1 : 0;
}
