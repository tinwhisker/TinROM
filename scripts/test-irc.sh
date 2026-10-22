#!/bin/sh
# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# Host tests for the IRC client's pure parts: the protocol, its screen text,
# the cryptography (against the published test vectors) and the password
# lock. Needs a C compiler and nothing else.
#
#   scripts/test-irc.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/shim/hal" "$work/shim/core" "$work/shim/libc"
printf '#pragma once\n#include <stdint.h>\nuint32_t tinromFrameCounter(void);\nuint32_t tinromTimerNoise(void);\n' > "$work/shim/hal/timer.h"
printf '#pragma once\n#include <string.h>\n' > "$work/shim/libc/string.h"

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wno-unused-function -D_GNU_SOURCE \
	-I "$work/shim" -I "$root/src" \
	"$root/src/comms/term.c" "$root/src/comms/irc.c" \
	"$root/src/comms/irctext.c" "$root/src/comms/crypto.c" \
	"$root/src/comms/vault.c" \
	"$root/tools/test_irc/test_irc.c" -o "$work/test_irc"

"$work/test_irc"
