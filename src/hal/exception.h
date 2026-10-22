/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The resident exception handler.
 *
 * The console vectors every exception except the COP0 hardware breakpoint to
 * 80000080h (psx-spx, "Exception Vectors"; the PSX only ever uses the BEV=0
 * set). The kernel puts its own handler there. tinrom replaces it with a four
 * instruction trampoline into its own handler, keeping the original words so
 * the vector can be given back.
 *
 * The handler lives in the resident monitor (src/monitor), not in tinrom: a
 * debugger has to be able to load a program over tinrom and keep catching
 * its exceptions. These functions are tinrom's view of it.
 *
 * tinrom does NOT chain to the kernel's handler. Decoding someone else's
 * trampoline and re-entering it correctly is a good way to lose a console,
 * and while tinrom is installed it is the thing in control anyway. What that
 * costs is that a program's own handler will not run until the vector is
 * given back - which is why the handler is removed before any handoff.
 *
 * This file is included from src/monitor/entry.S, so everything outside the
 * __ASSEMBLER__ guard must be preprocessor only.
 */
#pragma once

/* --- frame layout, shared with the assembly ----------------------------- */

/*
 * Registers are stored at their own register number, so r4 is at word 4. A
 * GDB stub wants them in that order anyway, and it makes the assembly a
 * straight run of stores with no table to get out of step with.
 */
#define TINROM_EXC_R(n)   ((n) * 4)

#define TINROM_EXC_AT     TINROM_EXC_R(1)
#define TINROM_EXC_V0     TINROM_EXC_R(2)
#define TINROM_EXC_V1     TINROM_EXC_R(3)
#define TINROM_EXC_A0     TINROM_EXC_R(4)
#define TINROM_EXC_A1     TINROM_EXC_R(5)
#define TINROM_EXC_A2     TINROM_EXC_R(6)
#define TINROM_EXC_A3     TINROM_EXC_R(7)
#define TINROM_EXC_SP     TINROM_EXC_R(29)
#define TINROM_EXC_RA     TINROM_EXC_R(31)

#define TINROM_EXC_HI     (32 * 4)
#define TINROM_EXC_LO     (33 * 4)
#define TINROM_EXC_SR     (34 * 4)
#define TINROM_EXC_CAUSE  (35 * 4)
#define TINROM_EXC_EPC    (36 * 4)
#define TINROM_EXC_BADV   (37 * 4)
#define TINROM_EXC_TAR    (38 * 4)

#define TINROM_EXC_FRAME_WORDS 39

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>
#include "core/result.h"

/* Cause register, psx-spx "cop0r13 - CAUSE". */
#define TINROM_EXC_CODE(cause) (((cause) >> 2) & 0x1f)
#define TINROM_EXC_CAUSE_BT    (1u << 30)
#define TINROM_EXC_CAUSE_BD    (1u << 31)

/* ExcCode values worth naming. */
#define TINROM_EXC_INT  0x00
#define TINROM_EXC_ADEL 0x04
#define TINROM_EXC_ADES 0x05
#define TINROM_EXC_IBE  0x06
#define TINROM_EXC_DBE  0x07
#define TINROM_EXC_SYS  0x08
#define TINROM_EXC_BP   0x09
#define TINROM_EXC_RI   0x0a
#define TINROM_EXC_CPU  0x0b
#define TINROM_EXC_OVF  0x0c

typedef struct {
	uint32_t r[32];
	uint32_t hi, lo;
	uint32_t sr, cause, epc, badVaddr, tar;
} TinromExceptionFrame;

_Static_assert(
	sizeof(TinromExceptionFrame) == (TINROM_EXC_FRAME_WORDS * 4),
	"the frame layout and the assembly offsets have drifted apart"
);

#ifdef __cplusplus
extern "C" {
#endif

/** Takes over 80000080h. Safe to call twice. */
TinromResult tinromExceptionInstall(void);
/** Puts the kernel's four words back. Safe to call when not installed. */
TinromResult tinromExceptionRemove(void);
bool         tinromExceptionInstalled(void);

/**
 * How many break instructions the handler has serviced. Cheap way to see the
 * thing is alive.
 */
uint32_t tinromExceptionBreakCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */
