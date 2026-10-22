#!/bin/sh
# tinrom - the CD player visualiser's analysis, on the host.
#
#     sh scripts/test-visual.sh
#
# Feeds src/modules/cdplay/visual.c made-up tones and silence and checks the
# bands, levels and waveform it makes of them. Needs a C compiler.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -I "$root/src" -I "$bare" \
	"$root/src/modules/cdplay/visual.c" "$bare/common/trig.c" \
	"$root/tools/test_visual/test_visual.c" -lm -o "$work/test_visual"

"$work/test_visual"
