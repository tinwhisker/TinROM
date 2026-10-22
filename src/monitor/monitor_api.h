/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The contract between tinrom and its resident debug monitor.
 *
 * The monitor is the exception handler, the debugger and a minimal serial
 * driver, linked on its own to run at TINROM_MONITOR_BASE and embedded in
 * tinrom as a blob. tinrom copies it there at boot. It lives apart from tinrom
 * because a debugger's job is to let you load a program over the top of
 * tinrom - which a stub inside tinrom could not survive. See docs/debugger.md.
 *
 * Everything here is plain data and function pointers at fixed addresses.
 * tinrom may call into the monitor; the monitor never calls into tinrom
 * except through the pcdrv hook, and only while tinrom is known to be intact.
 */
#pragma once

#include <stdint.h>
#include "hal/exception.h"

/*
 * 32 KB below the handoff stub at 801F0000h, leaving the 64 KB under the
 * default stack top (801FFFF0h) for stacks. A program loaded over this
 * window gets refused, not written.
 */
#define TINROM_MONITOR_BASE    0x801e8000u
#define TINROM_MONITOR_SIZE    0x00008000u
#define TINROM_MONITOR_MAGIC   0x4e4f4d54u /* "TMON" */
#define TINROM_MONITOR_VERSION 1u

#define TINROM_MONITOR_BREAKPOINTS 16

/*
 * Break codes the monitor owns. GDB's own software breakpoint is break 5
 * (0005000Dh), and the compiler uses 6 and 7 for overflow and division by
 * zero; these stay clear of those and of the PCDRV range, 101h-107h.
 */
#define TINROM_MONITOR_BREAK_ENTER 0x3feu /* tinrom asking to be halted */
#define TINROM_MONITOR_BREAK_PATCH 0x3fdu /* a breakpoint the monitor wrote */

typedef enum {
	TINROM_STOP_NONE       = 0,
	TINROM_STOP_REQUEST    = 1, /* halted on request: HALT, attach, GDB */
	TINROM_STOP_BREAKPOINT = 2, /* a breakpoint GDB set with Z0 */
	TINROM_STOP_STEP       = 3, /* a single step finished */
	TINROM_STOP_HOOK       = 4, /* a nops execute hook fired */
	TINROM_STOP_BREAK      = 5, /* a break instruction nobody registered */
	TINROM_STOP_FAULT      = 6  /* anything else: bus error, bad address... */
} TinromStopReason;

/* Why tinrom is halting itself; passed to breakIn(). */
typedef enum {
	TINROM_BREAKIN_ATTACH    = 0, /* a debugger made contact: halt quietly */
	TINROM_BREAKIN_HALT      = 1, /* nops sent HALT: answer HLTD */
	TINROM_BREAKIN_RSP       = 2, /* a GDB packet started; byte is its '$' */
	TINROM_BREAKIN_INTERRUPT = 3  /* GDB sent Ctrl-C */
} TinromBreakIn;

typedef enum {
	TINROM_HOST_NONE = 0,
	TINROM_HOST_NOPS = 1,
	TINROM_HOST_GDB  = 2
} TinromDebugHost;

typedef enum {
	TINROM_BP_FREE = 0,
	TINROM_BP_GDB  = 1, /* Z0 from GDB; stays until z0 */
	TINROM_BP_HOOK = 2, /* HKEX from nops; one-shot */
	TINROM_BP_STEP = 3  /* the monitor's own, for stepping; one-shot */
} TinromBreakpointKind;

typedef struct {
	uint32_t address;
	uint32_t kind;
} TinromBreakpoint;

/**
 * Called for a PCDRV break while tinrom is intact. Reads the arguments out of
 * the frame, writes V0/V1 back into it, returns non-zero if it serviced the
 * call.
 */
typedef int (*TinromPcdrvHook)(TinromExceptionFrame *frame, uint32_t function);

typedef struct {
	/* --- written by tinrom -------------------------------------------- */

	/** Non-PCDRV breaks stop in the debugger rather than being reported. */
	volatile uint32_t armed;
	/** Halt as soon as a debugger makes contact ("Wait for a debugger"). */
	volatile uint32_t attachOnContact;
	/** An emulator debug console is present; mirror crash reports to it. */
	uint32_t          dbgcon;
	/** tinrom's own image, as physical addresses [start, end). */
	uint32_t          tinromStart, tinromEnd;
	TinromPcdrvHook   pcdrv;
	/** tinrom's gp, set around the pcdrv call: tinrom's code needs it. */
	uint32_t          tinromGp;

	/* --- written by the monitor --------------------------------------- */

	/** Cleared when anything is written over tinrom's image. */
	volatile uint32_t tinromIntact;
	volatile uint32_t installed;
	volatile uint32_t halted;
	volatile uint32_t stopReason;
	volatile uint32_t stopSignal;
	volatile uint32_t stopPc;
	volatile uint32_t stopCount;

	volatile uint32_t breakCount;
	volatile uint32_t faultCount;
	volatile uint32_t interruptCount;

	volatile uint32_t    haveFault;
	TinromExceptionFrame lastFault;

	volatile uint32_t host;
	volatile uint32_t commands;
	volatile uint32_t lastCommand;
	volatile uint32_t bytesIn, bytesOut;
	/** Writes refused because they would have landed on the monitor. */
	volatile uint32_t refused;

	TinromBreakpoint  breakpoints[TINROM_MONITOR_BREAKPOINTS];
} TinromMonitorState;

typedef struct {
	uint32_t magic;
	uint32_t version;
	/**
	 * The blob is copied to TINROM_MONITOR_BASE up to imageEnd; imageEnd to
	 * bssEnd is zeroed. imageEnd - base must equal the blob's size.
	 */
	uint8_t *imageEnd;
	uint8_t *bssEnd;

	TinromMonitorState   *state;
	TinromExceptionFrame *frame;

	/** Takes over 80000080h. Returns 0. Safe to call twice. */
	int  (*install)(void);
	/** Puts the kernel's four words back. Safe when not installed. */
	int  (*remove)(void);
	/**
	 * Halt here, in the debugger, until a host resumes. Returns when it
	 * does. Only meaningful while installed.
	 */
	void (*breakIn)(uint32_t reason, uint32_t byte);
	/**
	 * Service one debug-class nops command whose token tinrom's serial
	 * server has already read. Returns non-zero if it was one.
	 */
	int  (*nopsCommand)(uint32_t token);
	/** Drops every breakpoint and hook. */
	void (*clearBreakpoints)(void);
} TinromMonitorHeader;

#define TINROM_MONITOR ((TinromMonitorHeader *) TINROM_MONITOR_BASE)
