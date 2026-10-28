#!/bin/sh
# tinrom - (C) 2026 The tinrom contributors - MIT licensed
#
# Host test for the cart code that writes to the cart: identifying its flash
# chip, and putting back what an EEPROM stores when it takes the ID command
# as data - against fake carts of every kind (tools/test_exp/test_exp.c).
# Needs a C compiler.
#
#   scripts/test-exp.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/shim/libc"
printf '#pragma once\n#include <string.h>\n' > "$work/shim/libc/string.h"

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -DEXP_HOST \
	-I "$work/shim" -I "$root/src" \
	"$root/src/hal/exp.c" "$root/tools/test_exp/test_exp.c" -o "$work/test_exp"

"$work/test_exp"
