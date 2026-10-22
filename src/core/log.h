/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdarg.h>
#include <stdbool.h>

/*
 * One logging path for the whole of tinrom. Output goes to an in-memory ring
 * buffer that the about/sysinfo screens can display; optionally out of SIO1 so
 * a host running `tinrom-host monitor` (or `nops /m`) sees it; and, when
 * tinrom is running under an emulator that offers one, out of that emulator's
 * debug console. See docs/architecture.md, "Logging".
 */

#define TINROM_LOG_LINES   64
#define TINROM_LOG_LENGTH  80

/* TINROM_UART() is not limited by what fits on a 320px screen. */
#define TINROM_UART_LENGTH 160

#ifdef __cplusplus
extern "C" {
#endif

void tinromLogInit(bool mirrorToSerial);
void tinromLogSetSerialMirror(bool enabled);

void tinromLogWrite(const char *format, ...);
void tinromLogWriteV(const char *format, va_list args);

/* Straight to the host, bypassing the ring buffer and the mirror setting. */
void tinromUartWrite(const char *format, ...);
void tinromUartWriteV(const char *format, va_list args);

/** Newest-last access to the ring buffer. */
unsigned int tinromLogCount(void);
const char  *tinromLogLine(unsigned int index);

#ifdef __cplusplus
}
#endif

/*
 * TINROM_LOG  - ring buffer (visible in the log screen), mirrored to SIO1 when
 *               "TTY to serial" is on or TINROM_LOG_TO_SERIAL was built in,
 *               and always to an emulator's debug console if one is detected.
 * TINROM_UART - straight to the host (SIO1 and the emulator console), always,
 *               no buffer. For tracing and for anything that has to be
 *               visible when the UI is not.
 */
#define TINROM_LOG(...)  tinromLogWrite(__VA_ARGS__)
#define TINROM_UART(...) tinromUartWrite(__VA_ARGS__)
