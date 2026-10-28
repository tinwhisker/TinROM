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

It is also packed. `TINROM_MCR_COMPRESS`, on by default, has
`tools/packexe.py` turn the card's payload into a PS-EXE that unpacks
itself, `tinrom_card.packed.psexe`, and that is what goes on the card. It
packs with `tools/tinlz.py`, as the cart does (see "The packed payload" in
`cart-rom.md`), and is laid out as:

```
+000h  src/boot/exe_stub.S       a branch, the table packexe.py fills in, code
+...   src/boot/unpack.c         the decoder, the cart's own
+...   the original's body, packed
```

It loads just past the end of where the original goes, so the two never
overlap. The stub unpacks the body to the original's load address and jumps
to the original's entry point with the registers the loader gave it, and
from there tinrom runs as it would unpacked; it clears its own BSS, over
the packed copy. Packing about halves the card's payload: a debug build's
120K came to 62K, eight blocks.

The packed executable runs from anywhere an ordinary one does, so it can be
tried while the card itself does not boot: load
`build/<preset>/tinrom_card.packed.psexe` in pcsx-redux, or send it with
any PS-EXE uploader.

Like the cart's stub, it does not invalidate the instruction cache before
the jump. The kernel's Exec flushes it before running a PS-EXE, and nothing
runs from where tinrom unpacks to between then and the jump. A loader on
the card that does not flush will need to before it jumps to the stub.

`TINROM_MCR_COMPRESS` off puts the payload on the card as it is, which it
no longer fits.

| Option | Default | Leaves out | Saves, roughly |
| --- | --- | --- | --- |
| `TINROM_MCR_MODEL` | `OFF` | the model spinning behind the menu, its mesh and texture | 13 KB |
| `TINROM_MCR_DEBUGGER` | `OFF` | the resident monitor: exception handler, PCDRV breaks, GDB and nops debugging, and the `gdb` screen with it | 19 KB |
| `TINROM_MCR_EXCLUDE_MODULES` | `terminal;irc;siocons;flasher` | any modules by id, `;`-separated. The serial terminal and IRC client are on it by default because they need SIO1, and the PSone has no SIO1; the serial server and the flasher to save room | 35 KB for the terminal, more with IRC; then per module |

And one that keeps everything but makes it smaller:

| Option | Default | Does | Saves, roughly |
| --- | --- | --- | --- |
| `TINROM_MCR_OPTIMIZE` | `-Os` | compiles the card payload's own code for size, whatever the build type | a tenth of the code |
| `TINROM_MCR_COMPRESS` | `ON` | packs the card's payload into a self-extracting PS-EXE, above | about half |

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
