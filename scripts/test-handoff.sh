#!/bin/sh
# tinrom - host test for the handoff planner. See tools/test_handoff.c.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/hal" "$work/core" "$work/libc"
cp "$root/src/hal/handoff.h" "$work/hal/"
cp "$root/src/core/result.h" "$work/core/"
printf '#pragma once\n#include <string.h>\n' > "$work/libc/string.h"
printf '#pragma once\n#include <stdio.h>\n#define TINROM_LOG(...) ((void) 0)\n' \
	> "$work/core/log.h"

# The linker symbol becomes an ordinary variable for the host build.
sed -e 's/^extern uint8_t _bssEnd\[\];/extern uint8_t *fake_bssEnd;\n#define _bssEnd fake_bssEnd/' \
	"$root/src/hal/handoff.c" > "$work/handoff.c"

# The stub lives in assembly the host cannot build; only its address is used,
# and tinromHandoffRun() is not exercised here.
cat > "$work/stub.c" <<'STUB'
const unsigned char tinromHandoffStub[1];
const unsigned char tinromHandoffStubEnd[1];
STUB

${CC:-gcc} -std=c11 -Wall -Wextra -Wno-int-to-pointer-cast \
	-Wno-pointer-to-int-cast -I "$work" -I "$root/src" \
	"$work/handoff.c" "$work/stub.c" "$root/src/core/result.c" \
	"$root/tools/test_handoff.c" \
	-o "$work/test_handoff"

"$work/test_handoff"
