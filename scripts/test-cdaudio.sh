#!/bin/sh
# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Host tests for CD audio and data reads: src/hal/cdrom.c and src/hal/spu.c,
# against a fake drive behind the registers (tools/test_cdaudio). Needs a C
# compiler that understands #include_next (gcc or clang).
#
#   scripts/test-cdaudio.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/shim/ps1" "$work/shim/core" "$work/shim/libc"
cp "$root/tools/test_cdaudio/registers.h" "$work/shim/ps1/registers.h"
printf '#pragma once\nvoid tinromLogWrite(const char *f, ...);\n#define TINROM_LOG(...) tinromLogWrite(__VA_ARGS__)\n' > "$work/shim/core/log.h"
printf '#pragma once\n#include <string.h>\n' > "$work/shim/libc/string.h"

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function \
	-I "$work/shim" -I "$root/src" -I "$bare" \
	"$root/src/hal/cdrom.c" "$root/src/hal/spu.c" \
	"$root/tools/test_cdaudio/test_cdaudio.c" -o "$work/test_cdaudio"

"$work/test_cdaudio"
