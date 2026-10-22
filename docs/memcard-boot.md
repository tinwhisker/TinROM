# Booting from a memory card

`tools/makemcard.py` builds `tinrom.mcr`. **It is a stub.**

What it does today: writes a structurally valid 128K card image with a
formatted directory and the payload stored in the data blocks. A console sees
a normal save.

What is missing: the exploit. The
[FreePSXBoot](https://github.com/brad-lin/FreePSXBoot) technique works by
handing the BIOS a deliberately malformed directory so that data from the card
ends up being executed. The exact layout depends on the BIOS revision, so a
real implementation needs a per-revision table and a way to pick the right
entry - either by asking the user or by reading the BIOS version out first
(which the sysinfo module wants to do anyway).

## Fitting on a card

A card has 15 usable blocks, 122880 bytes. That is a hard ceiling, and the
full tinrom payload no longer fits under it. So the memory card image gets a
payload of its own, `tinrom_card.psexe`, built from the same sources with
less in it. The boot disc keeps everything; the cart ROM leaves out only the
flasher by default (`TINROM_ROM_EXCLUDE_MODULES`).

| Option | Default | Leaves out | Saves, roughly |
| --- | --- | --- | --- |
| `TINROM_MCR_MODEL` | `OFF` | the model spinning behind the menu, its mesh and texture | 13 KB |
| `TINROM_MCR_DEBUGGER` | `OFF` | the resident monitor: exception handler, PCDRV breaks, GDB and nops debugging, and the `gdb` screen with it | 19 KB |
| `TINROM_MCR_EXCLUDE_MODULES` | `terminal;irc;siocons;flasher` | any modules by id, `;`-separated. The serial terminal and IRC client are on it by default because they need SIO1, and the PSone has no SIO1; the serial server and the flasher to save room | 35 KB for the terminal, more with IRC; then per module |

And one that keeps everything but makes it smaller:

| Option | Default | Does | Saves, roughly |
| --- | --- | --- | --- |
| `TINROM_MCR_OPTIMIZE` | `-Os` | compiles the card payload's own code for size, whatever the build type | a tenth of the code |

The main payload has the same switch, `TINROM_OPTIMIZE`, also `-Os` by
default. An empty one compiles with the build type's optimisation (`-Og` for
Debug), which GDB follows more closely.

Setting `TINROM_MCR_EXCLUDE_MODULES` replaces the whole list, so a list
given with `-D` has to name everything to leave out, the defaults included.
Editing the default in `cmake/options.cmake` is picked up by existing build
directories too: a setting nobody changed by hand follows the file (see
"Settings that follow this file" there). A build directory configured
before the list held them had a separate `TINROM_MCR_COMMS` switch; the
first configure after the change folds it into the list, so the card comes
out as it did.

The sizes are from a debug build. Excluding `siocons` also excludes `gdb` and
`pcdrv`, which cannot work without it. The `TINROM_MODULE_*` options apply to
every image, so anything switched off there is gone from the card too.

The same model and debugger switches exist for the main payload as
`TINROM_MODEL` and `TINROM_DEBUGGER`, both `ON` by default. When the card's
settings come out the same as the main payload's - which takes
`TINROM_MCR_OPTIMIZE` equal to `TINROM_OPTIMIZE` too - the card reuses
`tinrom.psexe` and nothing is built twice. The configure summary shows which applies:

```
cmake --preset debug -DTINROM_MCR_EXCLUDE_MODULES="terminal;irc;flasher;hexedit"
...
--   memory card    : tinrom_card.psexe - model OFF, debugger OFF, -Os
--     left out     : terminal;irc;flasher;hexedit
--     modules      : about;settingsui;sysinfo;...
```

Without the model the menu has a plain background. Without the monitor, the
PCDRV screen reports that there is nothing to arm.

## Before implementing this

Read the FreePSXBoot entry in `NOTICE.md`. If any of its data or code is
reused rather than reimplemented, its licence has to be recorded there and
checked against MIT redistribution first. Reimplementing from a description of
the technique is the cleaner path and is what the rest of tinrom does.

## Why it matters

This is the only route onto a PSone or any later board without a parallel
port. Without it, tinrom is cart-and-modchip only, which is a much smaller
audience than UniROM reaches.
