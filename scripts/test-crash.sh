#!/bin/sh
# tinrom - the cart's crash screen, drawn on the host.
#
#     sh scripts/test-crash.sh [output directory]
#
# Builds src/crash/crash.c over a software GPU and a made-up RAM (see
# tools/test_crash/test_crash.c), crashes four imaginary programs, and reads
# each screen back to check what it says. With a directory, it also writes
# the screens there as PNGs. Needs a C compiler, zlib and Python 3.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

${PYTHON:-python3} "$root/tools/convertbitfont.py" \
	"$root/src/ui/fonts/term4x8.txt" -o "$work/crashfont.c" \
	--name crashFont --first 0x20 --last 0x7e > /dev/null

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -DCRASH_HOST \
	-I "$root/src" -I "$bare" \
	"$root/src/crash/crash.c" "$work/crashfont.c" \
	"$root/tools/test_crash/test_crash.c" -lz -o "$work/test_crash"

if [ -n "$1" ]; then
	mkdir -p "$1"
	"$work/test_crash" "$1"
else
	"$work/test_crash"
fi
