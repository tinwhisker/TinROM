/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Internal interfaces of the resident monitor. Nothing outside src/monitor
 * includes this; tinrom talks to the monitor through monitor_api.h only.
 *
 * Rules for code in this directory, because it is linked on its own and must
 * keep working after tinrom has been overwritten:
 *   - no calls into tinrom, ps1-bare-metal's libc or printf;
 *   - no gp-relative data (built with -G0);
 *   - nothing that can block without a way out, except the stop loop itself.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "monitor/monitor_api.h"

/*
 * Raw memory the monitor touches directly: the vector, the kernel's table,
 * the emulator console. A host-side test build maps these onto fake memory.
 */
#ifndef MON_HOST_TEST
#define MON_RAM32(address) ((volatile uint32_t *) (uintptr_t) (address))
#define MON_RAM8(address)  ((volatile uint8_t  *) (uintptr_t) (address))
#else
volatile uint32_t *monTestRam32(uint32_t address);
volatile uint8_t  *monTestRam8(uint32_t address);
#define MON_RAM32(address) monTestRam32((uint32_t) (address))
#define MON_RAM8(address)  monTestRam8((uint32_t) (address))
#endif

/* Signals GDB understands in a stop reply. */
#define MON_SIGINT  2
#define MON_SIGILL  4
#define MON_SIGTRAP 5
#define MON_SIGFPE  8
#define MON_SIGBUS  10
#define MON_SIGSEGV 11
#define MON_SIGSYS  12

/* Who resumed the target last, and so who is owed a stop notification. */
typedef enum {
	MON_RESUMED_BY_NONE = 0,
	MON_RESUMED_BY_NOPS = 1,
	MON_RESUMED_BY_GDB  = 2
} MonResumer;

typedef enum {
	MON_ACTION_STAY     = 0,
	MON_ACTION_CONTINUE = 1,
	MON_ACTION_STEP     = 2
} MonAction;

/* --- entry.S -------------------------------------------------------------- */

extern TinromExceptionFrame tinromExceptionFrame;
extern void tinromExceptionHandler(void);
extern void monitorBreakInstruction(void);
int monitorPeek(uint32_t address, uint32_t size, uint32_t *out);
int monitorPoke(uint32_t address, uint32_t size, uint32_t value);

/* core.c, called from entry.S. */
void monitorDispatch(void);
void monitorNestedFault(uint32_t cause, uint32_t epc);

/* ps1-bare-metal's common/cache.s, linked into the monitor as well. */
void flushCache(void);

/* --- core.c ---------------------------------------------------------------- */

extern TinromMonitorState monitorState;

/** Set by the protocol handlers while stopped; read by the stop loop. */
void monRequestResume(MonAction action, MonResumer by);
int  monInstallVector(void);
/** Halts here until a host resumes; see TinromMonitorHeader.breakIn. */
void monBreakInNow(uint32_t reason, uint32_t byte);
bool monStopped(void);

/** Register access in GDB's numbering (0-31, sr, lo, hi, bad, cause, pc). */
uint32_t monRegGet(unsigned int regnum);
void     monRegSet(unsigned int regnum, uint32_t value);
#define  MON_GDB_REGS 72

/**
 * Memory as a debugger sees it. False if the range is refused (the monitor
 * itself, the exception vector, the serial port it is talking through) or
 * the access faulted.
 */
bool monRead(uint32_t address, uint8_t *out, uint32_t length);
bool monWrite(uint32_t address, const uint8_t *data, uint32_t length);
/** Whether a write of the whole range would be allowed, without writing. */
bool monWritable(uint32_t address, uint32_t length);
/** One aligned access of 1, 2 or 4 bytes, for I/O registers. */
bool monReadSized(uint32_t address, uint32_t size, uint32_t *out);
bool monWriteSized(uint32_t address, uint32_t size, uint32_t value);
/** After writing code. */
void monCodeChanged(void);

bool monBreakpointAdd(uint32_t address, TinromBreakpointKind kind);
bool monBreakpointRemove(uint32_t address, TinromBreakpointKind kind);
void monBreakpointClear(TinromBreakpointKind kind);

/** The stop reply GDB is owed, as a signal number. */
uint32_t monStopSignal(void);

/* --- sio.c and util.c --------------------------------------------------------- */

void monSioPut(uint8_t value);
void monSioPutWord(uint32_t value);
void monSioPutString(const char *text);
/** -1 if nothing is waiting. */
int  monSioPoll(void);
/** Waits about `ticks` polls (see MON_TIMEOUT_*); -1 on timeout. */
int  monSioGet(uint32_t ticks);
int  monSioGetBlocking(void);
bool monSioGetWord(uint32_t *out, uint32_t ticks);
void monSioFlushInput(void);
void monSioSetBaud(uint32_t baud);

/* Rough poll counts; one poll is a few dozen cycles at 33.8 MHz. */
#define MON_TIMEOUT_BYTE   600000u /* ~0.5s: a host mid-sentence */
#define MON_TIMEOUT_SHORT  150000u /* ~0.1s: a host that may say nothing */

char     monHexDigit(uint32_t value);
int      monHexValue(char c);
/** Parses hex up to a non-hex character; advances *text past it. */
uint32_t monParseHex(const char **text);

void monReport(const char *text);
void monReportHex(uint32_t value);

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
int   monStrStarts(const char *text, const char *prefix);

/* --- rsp.c ----------------------------------------------------------------- */

/** A '$' has been read; read the rest of the packet and act on it. */
void monRspPacket(void);
void monRspSendStop(void);
void monRspReset(void);

/* --- nops.c ---------------------------------------------------------------- */

/** One nops command whose token has been read. Non-zero if handled. */
int  monNopsCommand(uint32_t token);
/** The target just stopped; tell nops if it resumed us. */
void monNopsAnnounceStop(void);
