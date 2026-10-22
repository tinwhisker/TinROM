#!/bin/sh
# tinrom - host-side test for the hex editor module.
#
# Compiles the real module (src/modules/hexedit), the text canvas and the font
# for the host, with the GPU, the pads, memory and the memory card faked, and
# drives it with scripted button presses: editing across units, undo,
# read-only regions, value and text searches (including one across two card
# frames), writing a card frame back with its checksum fixed, and the warning
# before editing the kernel's RAM. See tools/test_hexedit/test_hexedit.c.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# The real register header, but with the GPU's command port made a variable.
mkdir -p "$work/shim/ps1" "$work/shim/tinrom"
cat > "$work/shim/ps1/registers.h" <<HDR
#pragma once
#include "$bare/ps1/registers.h"
#include <stdint.h>
extern volatile uint32_t fakeGp0;
#undef GPU_GP0
#define GPU_GP0 fakeGp0
HDR
sed -e 's/@PROJECT_VERSION@/0.0.0-test/g' -e 's/@PROJECT_VERSION_[A-Z]*@/0/g' \
	"$root/src/core/version.h.in" > "$work/shim/tinrom/version.h"
printf '#pragma once\n#define TINROM_MODULE_LIST(X)\n' > "$work/shim/tinrom/module_list.h"

# The font tables are generated, as in the real build.
${PYTHON:-python3} "$root/tools/convertfont.py" \
	"$root/src/ui/fonts/font-20x20.png" --cell 20x20 --size 10 --width 20 \
	--space 8 --tracking -2 --alias 96=39 -o "$work/uifont.c" > /dev/null

${CC:-gcc} -std=gnu11 -O0 -g -w -I "$work/shim" -I "$root/src" -I "$bare" \
	"$root/src/ui/canvas.c" "$root/src/ui/font.c" "$work/uifont.c" \
	"$root/src/modules/hexedit/view.c" "$root/src/modules/hexedit/hexedit.c" \
	"$root/src/modules/hexedit/find.c" "$root/src/modules/hexedit/card.c" \
	"$root/src/modules/hexedit/dial.c" \
	"$root/tools/test_hexedit/test_hexedit.c" -o "$work/test_hexedit"

"$work/test_hexedit"
