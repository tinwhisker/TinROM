/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The contract between tinrom and the cart's crash screen.
 *
 * The crash screen is what a game gets when it crashes on a console with a
 * tinrom cart in it. It is linked on its own (src/crash/crash.ld) to run
 * straight out of the cart's ROM, which is the one thing a loaded game does
 * not overwrite, and tools/makerom.py puts it at TINROM_ROM_CRASH_OFFSET.
 * Before handing the console over, tinrom points the kernel's
 * A(40h) SystemErrorUnresolvedException at its entry point: the kernel then
 * runs the game exactly as it would without tinrom - its interrupts, events
 * and exception handlers included - and only an exception nothing handled,
 * the kind that would have hung the console, reaches the crash screen.
 * See hal/crash.h and docs/exceptions.md.
 *
 * Included from src/crash/entry.S, so everything outside the __ASSEMBLER__
 * guard must be preprocessor only.
 */
#pragma once

/*
 * Where in the image it goes: after the stub, which ends well before this,
 * and before the payload, so the payload's size never moves it. It is
 * linked for exactly this address, which cmake/crash.cmake reads from here;
 * tools/makerom.py places it by the address in its header.
 */
#define TINROM_ROM_CRASH_OFFSET 0x0400

/* Room it may take, to the start of the payload at the latest. */
#define TINROM_CRASH_MAX_SIZE   0x1c00

/* No "u" suffixes on what the assembly uses: gas does not take them. */
#define TINROM_CRASH_MAGIC   0x53524354 /* "TCRS" little-endian */
#define TINROM_CRASH_VERSION 1

/*
 * The kernel's A(nnh) jump table (psx-spx, "BIOS Memory Map": 200h, 300h
 * bytes), and the entry for A(40h) SystemErrorUnresolvedException, which
 * the kernel calls once an exception has found no handler - BRK opcodes,
 * bus and address errors and the like - and which on its own just hangs.
 */
#define TINROM_KERNEL_A0_TABLE  0x80000200u
#define TINROM_KERNEL_A40_ENTRY (TINROM_KERNEL_A0_TABLE + 0x40 * 4)

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * At the start of the blob, so tinrom can find it from the cart's
 * descriptor (TinromRomDescriptor.crashOffset) and check it before trusting
 * it.
 */
typedef struct {
	uint32_t magic;
	uint32_t version;
	/* The address it was linked to run at: where this header must be. */
	uint32_t base;
	/* One past its last byte. */
	uint32_t end;
	/* What A(40h) is pointed at. */
	uint32_t entry;
	uint32_t reserved[3];
} TinromCrashHeader;

/*
 * Draws the screen and never returns. entry.S calls it with what COP0 said
 * about the fault, on a stack of its own.
 */
void crashMain(uint32_t cause, uint32_t epc, uint32_t badVaddr, uint32_t sr);

/* src/crash/entry.S lays it down as eight words. */
_Static_assert(sizeof(TinromCrashHeader) == 32,
	"the header and entry.S have drifted apart");

#endif /* __ASSEMBLER__ */
