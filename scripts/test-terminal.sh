#!/bin/sh
# tinrom - host-side test for the terminal's ANSI screen, telnet and modem.
#
# Compiles src/comms/term.c, telnet.c and modem.c for the host, with SIO1 and
# the clock faked, and checks the screen against ANSI sequences, telnet
# against a negotiating server, and the modem against a scripted adapter:
# dialling, BUSY, numeric results, NO CARRIER split across reads, both ways
# of hanging up, and typing at it directly. See
# tools/test_terminal/test_terminal.c.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# modem.c's headers, with their hardware calls declared and nothing more.
mkdir -p "$work/shim/hal" "$work/shim/core" "$work/shim/libc"
cat > "$work/shim/hal/uart_sio.h" <<'HDR'
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
uint32_t tinromSioBaud(void);
int      tinromSioSetBaud(uint32_t baud);
void     tinromSioBufferStart(void);
void     tinromSioBufferStop(void);
void     tinromSioSetCtsFlow(bool enabled);
void     tinromSioSetDtr(bool asserted);
bool     tinromSioDsr(void);
void     tinromSioFlushInput(void);
size_t   tinromSioReceive(uint8_t *out, size_t max);
void     tinromSioWrite(const void *data, size_t length);
void     tinromSioWriteString(const char *text);
HDR
printf '#pragma once\n#include <stdint.h>\nuint32_t tinromFrameCounter(void);\nvoid tinromDelayMicroseconds(int us);\n' > "$work/shim/hal/timer.h"
printf '#pragma once\n#include <stdbool.h>\nbool tinromVideoIsPAL(void);\n' > "$work/shim/hal/video.h"
printf '#pragma once\nvoid tinromLogWrite(const char *f, ...);\n#define TINROM_LOG(...) tinromLogWrite(__VA_ARGS__)\n' > "$work/shim/core/log.h"
printf '#pragma once\n#include <string.h>\n' > "$work/shim/libc/string.h"

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function \
	-I "$work/shim" -I "$root/src" \
	"$root/src/comms/term.c" "$root/src/comms/telnet.c" \
	"$root/src/comms/modem.c" \
	"$root/tools/test_terminal/test_terminal.c" -o "$work/test_terminal"

"$work/test_terminal"
