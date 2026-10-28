/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdint.h>

/*
 * The EXP1 cart image layout. tools/makerom.py writes these bytes; this
 * header is the runtime's view of them, so the sysinfo and flasher modules
 * can read a cart's descriptor without duplicating the offsets.
 *
 * The first 100h bytes are dictated by the console's boot ROM, not by tinrom:
 *
 *   +000h  4   post-boot entry point (an address, e.g. 1F000140h)
 *   +004h  2Ch post-boot ID string
 *   +030h  50h post-boot TTY message, NUL-terminated
 *   +080h  4   pre-boot entry point
 *   +084h  2Ch pre-boot ID string
 *   +0B0h  50h the same message, after the pre-boot ID
 *
 * The ID strings are magic values the boot ROM compares against before it
 * will call the corresponding entry point. They are exactly 2Ch bytes and
 * fill their field completely; a retail BIOS compares those bytes and no
 * more, so the message runs on from each - " would be great, but it isn't."
 * by default (TINROM_ROM_TTY_MESSAGE). See docs/cart-rom.md for the
 * evidence, and the note in NOTICE.md about what they are.
 *
 * Everything from +100h on is tinrom's own.
 */

#define TINROM_ROM_DESC_OFFSET 0x0100
#define TINROM_ROM_STUB_OFFSET 0x0140
#define TINROM_ROM_DESC_MAGIC  0x4d4f5254 /* 'TROM' little-endian */

/* Offsets inside the stub, relative to TINROM_ROM_STUB_OFFSET. */
#define TINROM_ROM_POSTBOOT_ENTRY 0x00
#define TINROM_ROM_PREBOOT_ENTRY  0x08

typedef struct {
	uint32_t magic;
	uint32_t version;       /* packed major<<16 | minor<<8 | patch */
	uint32_t payloadOffset; /* ROM offset of the payload image */
	uint32_t payloadSize;   /* bytes, unpacked: what lands in RAM */
	uint32_t loadAddress;   /* RAM address to copy it to */
	uint32_t entryPC;
	uint32_t initialSP;     /* 0 = leave the stack where the caller had it */
	uint32_t flags;
	uint32_t buildId;       /* FNV-1a of the payload, for "am I the cart?" */
	uint32_t crashOffset;   /* ROM offset of the crash screen, 0 = none */
	/*
	 * The payload packed with tools/tinlz.py: its size in the ROM, and the
	 * ROM offset of the decoder (src/boot/unpack.c) the stub calls to unpack
	 * it. Both 0 for a payload stored as it is, payloadSize bytes.
	 */
	uint32_t packedSize;
	uint32_t unpackOffset;
} TinromRomDescriptor;

/* The bytes the payload takes in the ROM. */
static inline uint32_t tinromRomStoredSize(const TinromRomDescriptor *desc) {
	return desc->packedSize ? desc->packedSize : desc->payloadSize;
}

#define TINROM_ROM_FLAG_HAS_PREBOOT  (1u << 0)
#define TINROM_ROM_FLAG_HAS_POSTBOOT (1u << 1)
#define TINROM_ROM_FLAG_NO_LICENCE   (1u << 2)
/* Run the payload decoder uncached; src/boot/rom_stub.S says why it may be. */
#define TINROM_ROM_FLAG_UNPACK_UNCACHED (1u << 3)
