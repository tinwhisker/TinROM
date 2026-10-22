/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "core/log.h"
#include "hal/dbgcon.h"
#include "hal/uart_sio.h"
#include "libc/string.h"

static char         logBuffer[TINROM_LOG_LINES][TINROM_LOG_LENGTH];
static unsigned int logHead;
static unsigned int logCount;
static bool         logMirror;

/*
 * Only the mirror's setting: the buffer is .bss, empty from the start, and
 * by the time this runs tinromHalInit() has already logged into it - the
 * monitor's load among others. Clearing it here used to throw those lines
 * away, which is exactly when they are wanted.
 */
void tinromLogInit(bool mirrorToSerial) {
	logMirror = mirrorToSerial;
}

#define SINK_SERIAL 1
#define SINK_DEBUG  2

static void emitLine(const char *line, unsigned int sinks) {
	if (sinks & SINK_SERIAL) {
		tinromSioWriteString(line);
		tinromSioWriteString("\r\n");
	}

	if (sinks & SINK_DEBUG) {
		tinromDbgConsoleWriteString(line);
		tinromDbgConsoleWriteByte('\n');
	}
}

/*
 * SIO1 is opt-in because it is a cable the user may not have plugged in. The
 * emulator console is not: if something is listening on EXP2 it is a debugger,
 * and it always wants the output.
 */
static unsigned int liveSinks(void) {
	unsigned int sinks = 0;

	/* Not while a modem has the port: it would be sent down the line. */
	if (logMirror && !tinromSioBuffering())
		sinks |= SINK_SERIAL;
	if (tinromDbgConsolePresent())
		sinks |= SINK_DEBUG;

	return sinks;
}

/*
 * Turning the mirror on replays what has already been logged. Almost
 * everything interesting - the banner, HAL bring-up, module init, the model
 * loader - happens long before anyone can reach the settings screen, so
 * without the replay switching the toggle on appears to do nothing at all
 * until the next log line, which may never come. Build with
 * -DTINROM_LOG_TO_SERIAL=ON to have the mirror live from the first line.
 */
void tinromLogSetSerialMirror(bool enabled) {
	if (enabled && !logMirror) {
		logMirror = true;

		/*
		 * Serial only: anything already in the buffer went out of the
		 * emulator console as it was written.
		 */
		emitLine("--- tinrom log (replaying buffered lines) ---", SINK_SERIAL);

		for (unsigned int i = 0; i < logCount; i++)
			emitLine(tinromLogLine(i), SINK_SERIAL);

		emitLine("--- tinrom log (live) ---", SINK_SERIAL);

		return;
	}

	logMirror = enabled;
}

void tinromLogWriteV(const char *format, va_list args) {
	char *line = logBuffer[logHead];

	vsnprintf(line, TINROM_LOG_LENGTH, format, args);

	logHead = (logHead + 1) % TINROM_LOG_LINES;

	if (logCount < TINROM_LOG_LINES)
		logCount++;

	unsigned int sinks = liveSinks();

	if (sinks)
		emitLine(line, sinks);
}

void tinromLogWrite(const char *format, ...) {
	va_list args;

	va_start(args, format);
	tinromLogWriteV(format, args);
	va_end(args);
}

/*
 * Serial-only, unconditional, never buffered. Use this when the ring buffer
 * is the wrong place for the output: high-volume tracing, anything you need
 * to see even with the mirror off, and anything logged from a path that may
 * not survive to draw a screen. It writes straight to SIO1, so tinromSioInit()
 * (i.e. tinromHalInit()) must have run first.
 */
void tinromUartWriteV(const char *format, va_list args) {
	char line[TINROM_UART_LENGTH];

	vsnprintf(line, sizeof(line), format, args);
	emitLine(line, SINK_SERIAL | SINK_DEBUG);
}

void tinromUartWrite(const char *format, ...) {
	va_list args;

	va_start(args, format);
	tinromUartWriteV(format, args);
	va_end(args);
}

unsigned int tinromLogCount(void) {
	return logCount;
}

const char *tinromLogLine(unsigned int index) {
	if (index >= logCount)
		return "";

	unsigned int oldest = (logHead + TINROM_LOG_LINES - logCount)
		% TINROM_LOG_LINES;

	return logBuffer[(oldest + index) % TINROM_LOG_LINES];
}
