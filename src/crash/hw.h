/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Everything the crash screen touches, in one place: the GPU's two ports,
 * the reads of what the program left in memory, and the few writes that
 * quieten the rest of the console.
 *
 * On the console these are the registers themselves. Built with CRASH_HOST,
 * for tools/test_crash, they are functions the test supplies - a software
 * GPU and a made-up RAM - so the screen can be drawn and checked as a PNG
 * without a console.
 */
#pragma once

#include <stdint.h>

#ifdef CRASH_HOST

uint32_t hwGpuStat(void);
void     hwGp1(uint32_t word);
/** A word of a command after its first. */
void     hwGp0(uint32_t word);
/** The first word of a command: waits, a while, for the GPU to be ready. */
void     hwGp0Command(uint32_t word);
/** A word of memory, at an address crash.c has already checked is RAM or ROM. */
uint32_t hwPeek(uint32_t address);
/** Stops the GPU's DMA and silences the SPU and CD audio. */
void     hwQuiet(void);
/** The end: on the console a loop forever, in the test a return. */
void     hwHalt(void);

#else

#include "ps1/registers.h"

static inline uint32_t hwGpuStat(void) {
	return GPU_STAT;
}

static inline void hwGp1(uint32_t word) {
	GPU_GP1 = word;
}

/*
 * The words of a command go straight in, as ps1-bare-metal's own drawing
 * does: the GPU takes a word far faster than code running uncached from an
 * 8-bit ROM can make one, and the FIFO holds sixteen.
 */
static inline void hwGp0(uint32_t word) {
	GPU_GP0 = word;
}

/*
 * Before each command, wait for GPUSTAT bit 28, "ready to receive DMA
 * block", as ps1-bare-metal's waitForGP0Ready() does - but not forever: a
 * status bit that never comes should cost a slow screen, not a blank one.
 * Not inlined: it is called from everywhere, and in ROM size is what is
 * short.
 */
#define GP0_READY_POLLS 4096

static void __attribute__((noinline)) hwGp0Command(uint32_t word) {
	for (unsigned int i = 0; i < GP0_READY_POLLS; i++) {
		if (GPU_STAT & GPU_STAT_WFEP)
			break;
	}

	GPU_GP0 = word;
}

static inline uint32_t hwPeek(uint32_t address) {
	return *((volatile const uint32_t *) address);
}

static inline void hwQuiet(void) {
	/* Whatever the program was sending the GPU stops mid-list. */
	DMA_CHCR(DMA_GPU) = 0;

	/*
	 * A crash leaves voices playing whatever they were, often a held note
	 * or a looping sample: off with all 24, and the master and CD volumes
	 * down so nothing still sounding is heard.
	 */
	SPU_MVOLL = 0;
	SPU_MVOLR = 0;
	SPU_AVOLL = 0;
	SPU_AVOLR = 0;
	SPU_KOF0  = 0xffff;
	SPU_KOF1  = 0x00ff;
}

static inline void hwHalt(void) {
	for (;;)
		__asm__ volatile("");
}

#endif
