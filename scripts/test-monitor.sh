#!/bin/sh
# tinrom - host-side tests for the resident debug monitor (src/monitor).
#
# The monitor's C compiles for the host with its serial port and memory
# faked (tools/test_monitor). Two builds:
#
#   tests  scripted byte streams for GDB and nops sessions, breakpoints,
#          stepping, faults, PCDRV and the refused ranges;
#   live   a pretend console on a socket, driven by the real tinrom_host code
#          and by a GDB remote-protocol client, and by a replay of what
#          `nops /gdb` sends - so both ends of each protocol are exercised.
#
# It cannot test entry.S (MIPS assembly) or the hardware; see
# docs/hardware-tests.md for that.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
here="$root/tools/test_monitor"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

cc="${CC:-gcc} -std=gnu11 -g -O0 -Wall -Wextra -Wno-unused-parameter \
	-Wno-unused-function -Wno-sign-compare -fno-builtin -DMON_HOST_TEST \
	-I $here/shim -I $root/src"
core="$root/src/monitor/core.c $root/src/monitor/rsp.c \
	$root/src/monitor/nops.c $root/src/monitor/util.c $here/fake_mem.c"

$cc $core "$here/fake_sio_queue.c" "$here/tests.c" -o "$work/tests"
$cc $core "$here/fake_sio_fd.c" "$here/live.c" -o "$work/live"

cd "$work"
./tests
MONITOR_CONSOLE="$work/live" python3 "$here/live_test.py"
MONITOR_CONSOLE="$work/live" python3 "$here/live_nops.py"
echo "monitor tests: clean"
