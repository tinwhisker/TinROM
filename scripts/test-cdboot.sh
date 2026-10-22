#!/bin/sh
# tinrom - host test for the disc-reading half of CD boot.
#
# Builds a disc image with tools/makeiso.py and runs the real ISO9660 walker,
# SYSTEM.CNF parser and PS-EXE header code against it through a file-backed
# sector reader. No console and no MIPS toolchain needed.
#
#   sh scripts/test-cdboot.sh
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/core" "$work/libc"
cp "$root/src/core/iso9660.h" "$root/src/core/psexe.h" "$root/src/core/result.h" \
	"$work/core/"
printf '#pragma once\n#include <string.h>\n' > "$work/libc/string.h"

python3 "$root/tools/makeiso.py" "$work/test.iso"

${CC:-gcc} -std=c11 -Wall -Wextra -I "$work" \
	"$root/src/core/iso9660.c" "$root/src/core/psexe.c" "$root/src/core/result.c" \
	"$root/tools/test_cdboot.c" -o "$work/test_cdboot"

"$work/test_cdboot" "$work/test.iso"
