#!/bin/sh
# tinrom - draw the UI's screens on the host, as PNGs.
#
#     sh scripts/render-ui.sh [output directory]
#
# Builds the real UI code with a software GPU underneath it (see
# tools/render_ui/render_ui.c) and writes one PNG per screen, twice the
# console's resolution: the main menu, a message box, a confirmation, a text
# pane, a popup, the memory card grid, the audio CD player, a terminal session with its keyboard
# and menu, the line editor and the hex editor. For checking layout
# and text fit without a console; it says nothing about timing.
#
# Needs a C compiler, zlib and Python 3.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bare="$root/extern/ps1-bare-metal/src"
out=${1:-"$root/build/render"}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
mkdir -p "$out" "$work/shim/ps1" "$work/shim/tinrom"

# The real register header, but with the GPU's ports made variables.
cat > "$work/shim/ps1/registers.h" <<HDR
#pragma once
#include "$bare/ps1/registers.h"
#include <stdint.h>
extern volatile uint32_t fakeGp0, fakeGp1;
#undef GPU_GP0
#define GPU_GP0 fakeGp0
#undef GPU_GP1
#define GPU_GP1 fakeGp1
HDR
sed -e 's/@PROJECT_VERSION@/0.0.0-render/g' -e 's/@[A-Z_]*@/0/g' \
	"$root/src/core/version.h.in" > "$work/shim/tinrom/version.h"
printf '#pragma once\n#define TINROM_MODULE_LIST(X)\n' > "$work/shim/tinrom/module_list.h"

${PYTHON:-python3} "$root/tools/convertfont.py" \
	"$root/src/ui/fonts/font-20x20.png" --cell 20x20 --size 10 --width 20 \
	--space 8 --tracking -2 --alias 96=39 -o "$work/uifont.c" > /dev/null

${PYTHON:-python3} "$root/tools/convertbitfont.py" \
	"$root/src/ui/fonts/term4x8.txt" -o "$work/termfont.c" > /dev/null

${CC:-gcc} -std=gnu11 -O1 -g -w -DTINROM_WITH_MODEL=0 \
	-I "$work/shim" -I "$root/src" -I "$bare" \
	"$root/src/ui/ui.c" "$root/src/ui/font.c" "$work/uifont.c" \
	"$root/src/ui/canvas.c" "$root/src/ui/widgets.c" "$root/src/ui/menu.c" \
	"$root/src/core/profile.c" \
	"$root/src/ui/grid.c" "$root/src/ui/keyboard.c" \
	"$root/src/modules/hexedit/view.c" \
	"$root/src/modules/cdplay/cdplay.c" \
	"$root/src/modules/cdplay/visual.c" "$bare/common/trig.c" \
	"$root/src/comms/config.c" "$root/src/comms/modem.c" \
	"$root/src/comms/telnet.c" "$root/src/comms/term.c" \
	"$root/src/comms/termview.c" "$work/termfont.c" \
	"$root/src/modules/terminal/session.c" \
	"$root/src/comms/irc.c" "$root/src/comms/irctext.c" \
	"$root/src/comms/crypto.c" "$root/src/comms/vault.c" \
	"$root/src/modules/irc/client.c" \
	"$root/tools/render_ui/render_ui.c" -lz -o "$work/render_ui"

"$work/render_ui" "$out"
