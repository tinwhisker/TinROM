#!/bin/sh
# tinrom - host-side test for tinrom's saves on memory cards.
#
# Compiles the card driver (src/hal/mcard.c) against two emulated cards that
# answer the SIO0 read and write commands byte by byte, with the save
# container, the settings and the terminal's bookmarks on top, and checks:
# saving and loading settings with card 1 preferred, files replaced rather
# than doubled, a different module set resetting only module choices,
# bookmarks with the settings or in a save of their own and the newest
# winning, each kind of save keeping the other's section, full cards, no
# cards, and a damaged save. See tools/test_saves/test_saves.c.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

mkdir -p "$work/shim/ps1" "$work/shim/tinrom"
cat > "$work/shim/ps1/registers.h" <<HDR
#pragma once
#include "$bare/ps1/registers.h"
#include <stdint.h>
extern volatile uint32_t fakeGp0;
#undef GPU_GP0
#define GPU_GP0 fakeGp0
HDR
sed -e 's/@PROJECT_VERSION@/0.0.0-test/g' -e 's/@[A-Z_]*@/0/g' \
	"$root/src/core/version.h.in" > "$work/shim/tinrom/version.h"
printf '#pragma once\n#define TINROM_MODULE_LIST(X)\n' > "$work/shim/tinrom/module_list.h"

${CC:-gcc} -std=gnu11 -O1 -g -w -I "$work/shim" -I "$root/src" -I "$bare" \
	"$root/src/hal/mcard.c" "$root/src/core/savefile.c" \
	"$root/src/core/settings.c" "$root/src/comms/config.c" \
	"$root/src/core/profile.c" \
	"$root/tools/test_saves/test_saves.c" -o "$work/test_saves"

"$work/test_saves"
