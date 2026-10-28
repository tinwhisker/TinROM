#!/bin/sh
# tinrom - (C) 2026 The tinrom contributors - MIT licensed
#
# Host tests for the cart's packed payload: tools/tinlz.py's encoder and
# reference decoder, tools/makerom.py's layout of a packed image, and
# src/boot/unpack.c - the decoder the cart runs - unpacking every case.
# Needs a C compiler and Python 3.
#
#   scripts/test-unpack.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# With the address sanitizer where there is one, so a read past a stream or
# a write past the output is caught.
flags="-std=gnu11 -O1 -g -Wall -Wextra -I $root/src"
sources="$root/src/boot/unpack.c $root/tools/test_unpack/test_unpack.c"

if ! ${CC:-gcc} $flags -fsanitize=address $sources -o "$work/test_unpack" 2> /dev/null; then
	${CC:-gcc} $flags $sources -o "$work/test_unpack"
fi

echo "packing:"
${PYTHON:-python3} "$root/tools/test_unpack/cases.py" "$root" "$work" > "$work/cases.log" \
	|| { cat "$work/cases.log"; exit 1; }
sed '$d' "$work/cases.log"

echo "unpacking:"
"$work/test_unpack" "$work"/*.lz
