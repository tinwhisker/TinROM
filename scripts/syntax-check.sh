#!/bin/sh
# tinrom - host syntax check.
#
# Parses every source file with the host compiler against the real headers.
# It cannot link, run or produce MIPS code - the toolchain does that - but it
# catches typos, bad prototypes, unused variables and signature drift in a
# second, without leaving the machine you are editing on.
#
#   sh scripts/syntax-check.sh
#
# Two files need generated headers that only CMake produces; stubs for them
# are written to a temporary directory. src/core/module.c is skipped because
# the stub module list makes its bounds checks look degenerate.
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
stub=$(mktemp -d)
trap 'rm -rf "$stub"' EXIT

mkdir -p "$stub/tinrom"
# Generated from the real template rather than written out here, so the two
# cannot drift apart - a hand-written stub is how a new declaration in
# version.h.in goes unnoticed until the MIPS build fails.
sed -e 's/@PROJECT_VERSION@/0.0.0-syntaxcheck/g' \
    -e 's/@PROJECT_VERSION_MAJOR@/0/g' \
    -e 's/@PROJECT_VERSION_MINOR@/0/g' \
    -e 's/@PROJECT_VERSION_PATCH@/0/g' \
    "$root/src/core/version.h.in" > "$stub/tinrom/version.h"

# The build stamp is a generated .c file the real build compiles in; the
# syntax check only needs the symbols to exist.
cat > "$stub/tinrom_build_stub.c" <<'SRC'
#include <stdint.h>
const char tinromBuildNumber[] = "00000000";
const char tinromBuildTime[]   = "00-00-00 00:00";
const char tinromVersionFull[] = "0.0.0-00000000";
const uint32_t tinromBuildStamp = 0u;
SRC
cat > "$stub/tinrom/module_list.h" <<'HDR'
#pragma once
#define TINROM_MODULE_LIST(X)
HDR

status=0

for file in $(find "$root/src" -name '*.c' | sort); do
	case "$file" in *core/module.c) continue ;; esac

	output=$(${CC:-gcc} -std=c11 -fsyntax-only -Wall -Wextra \
		-Wno-unused-parameter -Wno-pointer-to-int-cast \
		-Wno-int-to-pointer-cast \
		-I "$root/src" -I "$bare" -I "$bare/libc" -I "$stub" \
		-DTINROM_DEFAULT_TIER=TINROM_TIER_ADVANCED \
		-DTINROM_DEFAULT_BAUD=115200 \
		-DTINROM_ROM_BASE=0x1F000000 -DTINROM_ROM_SIZE=0x80000 \
		-DTINROM_LOG_SERIAL_DEFAULT=0 \
		"$file" 2>&1 \
		| grep -v 'In file included from\|^ \|ps1-bare-metal\|gpucmd.h' || true)

	if [ -n "$output" ]; then
		echo "=== ${file#$root/}"
		echo "$output"
		status=1
	fi
done

# Hand-written assembly gets a second pass the compiler cannot do: the R3000
# has no interlocks, and inside .set noreorder nothing warns about an unfilled
# load delay slot.
if ! python3 "$root/scripts/check-hazards.py"; then
	status=1
fi

[ $status -eq 0 ] && echo "syntax check: clean"
exit $status
