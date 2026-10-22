#!/bin/sh
# tinrom - end-to-end test for PCDRV.
#
# Runs the real console-side code (src/core/pcdrv.c) against the real host
# server (tools/tinrom_host/pcdrv.py) over a pair of pipes, so the two halves
# of the wire protocol are checked against each other rather than against one
# author's idea of it.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/inc/core" "$work/inc/libc" "$work/inc/hal" "$work/served/SUB"
cp "$root/src/core/pcdrv.h" "$root/src/core/result.h" "$work/inc/core/"
printf '#pragma once\n#include <string.h>\n' > "$work/inc/libc/string.h"
printf '#pragma once\n#include <stdarg.h>\n void tinromLogWrite(const char *, ...);\n#define TINROM_LOG(...) tinromLogWrite(__VA_ARGS__)\n' \
	> "$work/inc/core/log.h"
cat > "$work/inc/hal/uart_sio.h" <<'HDR'
#pragma once
#include <stddef.h>
#include "core/result.h"
void tinromSioWrite(const void *data, size_t length);
void tinromSioFlushInput(void);
TinromResult tinromSioRead(void *data, size_t length, unsigned int timeout);
HDR

printf 'hello, host!\n' > "$work/served/HELLO.TXT"
printf 'deeper\n' > "$work/served/SUB/DEEP.TXT"

${CC:-gcc} -std=c11 -Wall -Wextra -Wno-int-to-pointer-cast \
	-Wno-pointer-to-int-cast -I "$work/inc" \
	"$root/src/core/pcdrv.c" "$root/src/core/result.c" \
	"$root/tools/test_pcdrv.c" -o "$work/test_pcdrv"

PYTHONPATH="$root/tools" python3 - "$work" "$root" <<'PY'
import os, subprocess, sys
from tinrom_host.pcdrv import PCDrvServer

work, root = sys.argv[1], sys.argv[2]
to_host_r, to_host_w = os.pipe()
from_host_r, from_host_w = os.pipe()

child = subprocess.Popen(
    [os.path.join(work, "test_pcdrv"), str(to_host_w), str(from_host_r)],
    pass_fds=(to_host_w, from_host_r))
os.close(to_host_w); os.close(from_host_r)


class Pipes:
    def read(self, n):
        out = b""
        while len(out) < n:
            chunk = os.read(to_host_r, n - len(out))
            if not chunk:
                return out
            out += chunk
        return out

    def write(self, data):
        os.write(from_host_w, data)


server = PCDrvServer(os.path.join(work, "served"))
try:
    while child.poll() is None:
        if not server.serve_one(Pipes()):
            break
except OSError:
    pass

sys.exit(child.wait())
PY
