# The cart ROM image

`tinrom.rom` is a flashable EXP1 image for an Xplorer, Action Replay or other
Caetla-compatible cart. `tools/makerom.py` builds it.

## Layout

```
  +000h  4     post-boot entry point (an address, e.g. 1F000140h)
  +004h  2Ch   post-boot ID string          } required by the boot ROM
  +030h  50h   post-boot TTY message, NUL-terminated
  +080h  4     pre-boot entry point
  +084h  2Ch   pre-boot ID string           } required by the boot ROM
  +0B0h  50h   reserved
  +100h  30h   tinrom descriptor
  +140h  ...   loader stub (post-boot at +00, pre-boot at +08)
  +400h  ...   the crash screen games get, about 5K (TINROM_ROM_CRASH_SCREEN)
  +...   ...   PS-EXE payload body, 4-byte aligned
```

The first 100h bytes are dictated by the console, not by tinrom. The layout
is documented in psx-spx; see `NOTICE.md`.

The descriptor's `crashOffset` (word 9) says where the crash screen is, or 0
for an image without one; the payload then follows the stub directly. The
crash screen runs from the cart itself, where a loaded game cannot overwrite
it, and is linked for its place there: `+400h` is fixed in
`src/crash/crash_api.h` (`TINROM_ROM_CRASH_OFFSET`). `makerom.py` puts it
where its header says it was linked, and refuses one linked for another
cart base or one the stub would run into. See [exceptions.md](exceptions.md#the-crash-screen-games-get).

## The ID string

The bytes at +004h and +084h must read
`Licensed by Sony Computer Entertainment Inc.`, or the console will not call
the corresponding entry point. Two independent sources say so:

**psx-spx**, on the header layout:

> The entrypoints are called if their corresponding ID strings are present,
> return address to BIOS is passed in R31 [...] Aside from verifying the IDs,
> the BIOS will also display the Post-Boot ID string (and the following
> message string) via TTY.

**OpenBIOS**, pcsx-redux's clean-room reimplementation of the boot ROM
(`src/mips/openbios/pio/pio.c`), which gates both hooks on it outright:

```c
static const char *const licenseText = "Licensed by Sony Computer Entertainment Inc.";

void runExp1PreHook() {
    if (is_running_from_rom() && (strcmp(preHookInfo->signature, licenseText) == 0))
        preHookInfo->vector();
}
```

So it is a magic value an interface requires, in the same way a file format
requires its magic number. tinrom emits it because a cart image that does not
is a cart image the console ignores. It is gated behind the CMake option
`TINROM_ROM_LICENCE_STRING`; building with it `OFF` produces an image that a
flash cart will hold and a dumper will read back correctly, and that the
console will never call into. That option exists so the decision is visible
and yours, not buried.

### Confirmed on hardware-equivalent silicon

A retail **SCPH-5552 (PAL, Ver 2.5)** BIOS calls the pre-boot entry point with
the header this repository generates. The stub's `0xE1` trace byte appears
between the BIOS's own `0x0f` and `0x0e`, which is precisely where
`runExp1PreHook()` sits, and boot then continues normally because the stub
returns via `$ra`.

So, empirically settled:

- The ID string really is required, and the string in `tools/makerom.py` is
  the right one.
- The header layout - entry word at +000h/+080h, ID at +004h/+084h, entry
  points at stub offsets +00h and +08h - is correct for a retail BIOS.
- Leaving +030h and +0B0h zero does not stop a retail BIOS calling the hook.

Still open: whether the retail BIOS compares a fixed 2Ch bytes or uses a
`strcmp`. The default header satisfies both, so this test cannot tell them
apart. To settle it, rebuild with `-DTINROM_ROM_TTY_MESSAGE=tinrom` - that
puts a non-zero byte at +030h and removes the terminator. If `0xE1` still
appears, the comparison is length-bounded and a TTY banner is safe; if it
disappears, it is a `strcmp` and the banner must stay off.

### Why there is no TTY message by default

The string is exactly 44 bytes, which is the whole of the 2Ch-byte ID field -
there is no room for a terminator inside it. That matters, because the two
sources imply different comparisons:

- OpenBIOS uses `strcmp`, which only succeeds if the byte at **+030h** - the
  first byte of the TTY message field - is `00h`.
- psx-spx describes the BIOS printing "the Post-Boot ID string (and the
  following message string)" as one run, which only makes sense if the ID is
  *not* terminated at +030h and the print continues into the message. That
  reads like a fixed-length comparison of 2Ch bytes, which would not care
  what is at +030h.

For OpenBIOS this is now settled, and the terminator is required. Its own
`psx-bios-as-cart.ld` - the script that builds OpenBIOS *as* a cart image -
emits the licence string byte by byte and then explicitly adds `BYTE(0x00)`
before aligning. It NUL-terminates its own ID field, because its own check is
a `strcmp`. The retail BIOS may still differ.

So `makerom.py` satisfies both readings: the ID fills +004h..+02Fh and
**+030h is left zero**, which passes a fixed-length compare and a `strcmp`
alike. The cost is that there is no TTY banner.

`makerom.py --tty-message TEXT` (CMake: `TINROM_ROM_TTY_MESSAGE`) puts one
back, and warns that doing so breaks the strcmp case. Do not set it unless you
have tested the resulting image on the hardware you care about.

If someone does pin this down against a real BIOS, update this section and the
constant in `tools/makerom.py` together.

## The two entry points

**Post-boot** is called while the "PS" logo is on screen, after the kernel's
A0/B0/C0 vectors are installed but before the disc's executable is loaded.
This is where tinrom actually starts. Note that it is only called when the
logo is shown - not with an empty drive or an audio CD.

**Pre-boot** is called almost immediately after reset, before the vectors
exist and before RAM has been sized - far too early to load anything. With
`TINROM_ROM_MIDBOOT` (the default) it arms the mid-boot hook below and
returns, and tinrom then loads with or without a disc. With it off the stub
only leaves a trace byte and returns, and the post-boot path, which needs a
disc, is the only one that loads the payload.

This matters more than it sounds. A cart that only works with a disc in the
drive is not much of a cart; booting with an empty drive is most of the point.
The way out is the mid-boot hook, which is what Action Replay v2 and later do:

1. Pre-boot sets a COP0 opcode-fetch hardware breakpoint on `80030000h`
   (`BPC` = the address, `BPCM` = the mask, `PCE` set in `DCIC`), then returns
   to the BIOS.
2. The kernel initialises normally - A0/B0/C0 tables, exception vectors, the
   lot - so by the time we come back everything works.
3. When the kernel reaches the shell at `80030000h`, the breakpoint fires and
   the CPU jumps to **`80000040h`** - a dedicated breakpoint vector, *not* the
   general `80000080h` one. `Cause.Excode` reads `09h`, as for `BREAK`.
4. tinrom takes over there, before the CD-ROM is touched.

This is implemented, behind `TINROM_ROM_MIDBOOT` (on by default). Turning it
off restores the old behaviour, where the cart only runs with a disc present.

What the code does:

- **Pre-boot** copies a 28-byte trampoline into `A0000040h` - the KSEG1 view
  of the breakpoint vector, written uncached so it lands in RAM rather than
  sitting in the data cache - then sets `BPC` = `80030000h`,
  `BPCM` = `FFFFFFFFh` (exact match) and `DCIC` = `E1800000h`, and returns to
  the BIOS. `DCIC` there is `TR|UD|KD|PCE|DE`; note `DE` (bit 23) gates bits
  24-31, so without it the rest is inert.
- **The trampoline** clears `DCIC` as its very first instruction - psx-spx
  calls this out, and without it the handler is one breakpoint away from
  re-entering itself - then jumps back into ROM. It uses `$k0`/`$k1`, the
  registers reserved for exception handlers, because nothing expects them
  preserved and we have no idea what the kernel was in the middle of.
- **Back in ROM** it is an ordinary load: descriptor, copy, jump, with the
  boot source reported as `TINROM_BOOT_MIDBOOT`.

### What is not yet proven about it

Two things reasoning cannot settle, only testing:

- ~~**Whether the trampoline survives.**~~ **Confirmed on a retail SCPH-5552.**
  The full sequence `0x0f 0xE1 0xE5 0x0e … 0x07 0xE4 0xE2 0xE3` was observed:
  the kernel initialises over the top of it and leaves `80000040h` intact, the
  breakpoint fires, and control reaches tinrom with no disc in the drive. This
  is the technique working end to end.
- **The state tinrom inherits.** We arrive via an exception and never `rfe`,
  so the CPU stays in whatever mode the exception left: kernel mode,
  interrupts disabled. That happens to suit tinrom, which masks interrupts
  and polls anyway - but it is inherited rather than chosen, and the stack is
  still the kernel's.

In both cases the return address to the boot ROM is in `$ra`, so returning
cleanly is always available.

## The loader stub

`src/boot/rom_stub.S` is assembled straight to a raw binary and never linked,
so it must contain no relocations at all: every branch is PC-relative and the
descriptor is read from a fixed uncached EXP1 address (`0xBF000100`) rather
than through a symbol. That is why it can sit at whatever offset
`makerom.py` chooses without anything having to patch it.

It reads the descriptor, copies the payload from ROM into RAM a byte at a
time - EXP1 is an 8-bit bus in its default configuration, so byte reads are
the only safe width - sets the stack, and jumps to the payload's entry point
with the boot source in `$a0`.

### Seeing whether it ran at all

At the moment either entry point is called, tinrom owns no screen and no
serial port, so a stub that misbehaves is completely silent. The build option
`TINROM_ROM_TRACE` (on by default) has the stub write progress bytes to the
EXP2 POST register at `BF802041h` - the same register the boot ROM uses for
its own progress, so it is known good by the time we get there.

PCSX-Redux logs every byte written there as `BIOS Trace1: 0xNN`.

| Byte | Meaning |
| --- | --- |
| `0xE0` | post-boot entry reached |
| `0xE1` | pre-boot entry reached |
| `0xE5` | trampoline installed, breakpoint armed, returning to the BIOS |
| `0xE4` | breakpoint fired - we have the machine |
| `0xE2` | reading the descriptor |
| `0xE3` | payload copied, jumping to it |
| `0xEF` | descriptor empty, returning to the BIOS |

With a cart and no disc, the expected sequence is
`0x0f`, `0xE1`, `0xE5`, `0x0e`, … the kernel's own progress …, then `0xE4`,
`0xE2`, `0xE3` and tinrom on screen. Where it stops tells you which stage
failed.

Seeing `0xE1` between the BIOS's own `0x0f` and `0x0e` proves the header was
accepted - which is also the empirical answer to the ID-string question above,
since the console only calls the entry point when the string matches.

### Testing in an emulator

**Tick "Connected" in the PIO Cartridge window, or nothing will happen.**
Loading the image and mapping it are two separate things in PCSX-Redux.
`Configuration -> PIO Cartridge` has a ROM picker, an On/Off switch and a
`Connected` checkbox. Loading a file through the picker fills the emulator's
EXP1 buffer, but only `Connected` installs the memory-map entries that let the
CPU read it - `Memory::setLuts()` maps main RAM and nothing else; EXP1 is
mapped solely by `PIOCart::setLuts()`, which returns early unless that box is
ticked.

Without it, `psxmem.cc` has an explicit special case returning `0xff` for
addresses `1F000004h` and `1F000084h` - the two ID string locations - with the
comment *"EXP1 not mapped, likely the bios looking for pre/post boot entry
point"*. The string comparison then fails and neither entry point is ever
called. The symptom is a completely normal-looking boot log with no sign of
the cart at all, which reads like the ROM being rejected when it was never
looked at.

The On/Off switch only drives a status bit in the emulated PAL at page
`1F06h`; it does not affect ROM reads.

Note redux caps EXP1 at `0x40000` (256K) - it maps four 64K pages - and
repeats them, so an image bigger than that reads back its own start past
256K. The stub copies that into RAM without knowing, and the end of tinrom -
the last of `.rodata`, `.data` and `.sdata` - is then the start of tinrom
again. Nothing fails where the damage is: the monitor's size reads wrong and
it "does not load", a save file's name pointer is an instruction and the
memory card code reads from it, and so on.

A debug build with everything in, compiled with the build type's `-Og`, is
past 256K; with `TINROM_OPTIMIZE=-Os`, the default, it fits. So:

- `makerom.py` warns when the image is bigger than 256K;
- tinrom checks at boot: a window that small shows the cart's descriptor
  again 256K on (a real flash has payload there), and if this tinrom came
  from such a cart it logs `exp: EXP1 repeats every 256K ...` and says so on
  screen before anything else runs;
- for the emulator, boot `tinrom.psexe`, or build the cart with
  `-DTINROM_ROM_SIZE=0x40000` so that makerom refuses an image that would
  not fit.

**The post-boot entry point needs a disc in the drive.** psx-spx is explicit
that it is called only when the "PS" logo is shown - not with an empty drive,
not with an audio CD. OpenBIOS matches: the hook sits after `initCDRom()`, and
with no disc the BIOS reaches `SystemError` and halts before ever getting
there. An empty-drive test looks like tinrom failing when tinrom was never
called. Mount any disc image, or watch for `0xE1` from the pre-boot path
instead, which runs unconditionally and much earlier.

### OpenBIOS will not call either entry point

If you are testing against OpenBIOS rather than a retail BIOS, the hooks are
gated off by a bug and your cart will never run, however correct its header.

`src/mips/openbios/pio/pio.c`:

```c
extern const char *_reset;
static int is_running_from_rom() { return _reset == (const char *)0xbfc00000; }
...
if (is_running_from_rom() && (strcmp(preHookInfo->signature, licenseText) == 0))
    preHookInfo->vector();
```

`_reset` is the reset vector - a **function label** at `BFC00000h`, declared in
`boot/psx.s` as `.type _reset, @function`. But pio.c declares it as
`extern const char *_reset`, a pointer *object*. Reading it therefore loads the
word stored **at** `BFC00000h` - the first instruction of the BIOS,
`lui $t0, 0x0013` = `3C080013h` - and compares that against `BFC00000h`. It is
never equal, so `is_running_from_rom()` always returns false and both hooks are
skipped.

The intent was plainly `&_reset == (const char *)0xbfc00000`, which would be
true for a ROM build and false for the build-as-a-cart variant the guard was
added for (commit 6489572a, "Avoid boot-from-cart infinite loop", Jan 2021).

Symptom: a completely ordinary boot log with no sign of the cart, identical to
having no cart fitted. Indistinguishable from the EXP1-not-mapped case above
without the POST traces.

Test against a retail BIOS instead, or on hardware.

### Known gap

The stub does not invalidate the instruction cache before jumping into the
memory it just wrote. On a cold boot this happens to work; it is not reliable
in general. The fix is the COP0 isolate-and-invalidate sequence written out
inline, because the kernel's `FlushCache` is not available on the pre-boot
path. This is the single most important thing to fix before trusting the ROM
target on real hardware.

## Sizes

`TINROM_ROM_SIZE` defaults to `0x80000` (512K). 128K (`0x20000`) and 384K
(`0x60000`) carts exist; the `minimal` preset targets 128K. `makerom.py`
fails with a clear message if the payload does not fit, and reports how full
the image is when it does.

### The 128K image

Alongside `tinrom.rom` the build makes `tinrom-128k.rom`, for carts with a
128K chip - a 29F010, which many Datel carts (Action Replay, GameShark) and
their clones have. `tinrom.rom` carries everything and does not fit one.

Its payload is the memory card's by default - no model, no debug monitor,
and without terminal, irc, siocons and flasher - and while the settings
match, the two images share `tinrom_card.psexe`, so nothing is built twice.
That is also why it always fits: the card's 15 blocks hold at most 120K of
payload, and the header, stub and crash screen take at most 8K
(`TINROM_CRASH_MAX_SIZE` ends at 2000h). A debug build leaves about 4.7K
spare.

| Setting | Default | |
| --- | --- | --- |
| `TINROM_ROM_SMALL` | `ON` | build it at all |
| `TINROM_ROM_SMALL_SIZE` | `0x20000` | the file is named for it: `tinrom-128k.rom` |
| `TINROM_ROM_SMALL_MODEL` | `OFF` | as `TINROM_MODEL` |
| `TINROM_ROM_SMALL_DEBUGGER` | `OFF` | as `TINROM_DEBUGGER` |
| `TINROM_ROM_SMALL_OPTIMIZE` | `-Os` | as `TINROM_OPTIMIZE` |
| `TINROM_ROM_SMALL_EXCLUDE_MODULES` | `terminal;irc;siocons;flasher` | modules to leave out of it alone |

It has booted on a real console, from a Datel-style cart with a 29F010 -
the first tinrom cart seen running off hardware rather than in pcsx-redux.
That run found two things the emulator hides, both fixed: the pads read one
byte late (`hal/sio0.c`), and a PAL console came up in NTSC, because a cart
that takes over mid-boot gets there before the BIOS has set the video mode
(`hal/video.c` now goes by the BIOS's region letter).

Change any of them and it gets a payload of its own, `tinrom_small.psexe`;
take an id off the list to put a module back, and makerom says if it no
longer fits. The `minimal` preset turns it off, being 128K already.

Two things about Datel carts that make 128K the safe size, from what has
been found so far:

- **Their decoder does not map a bigger chip in one piece.** The first 128K
  is at `1F000000h`, the cart's I/O registers (the switch, the comms link)
  at `1F020000h`, and a second 128K, where fitted, at `1F040000h`. The stub
  copies one straight run, so past 128K it would read the registers. A
  layout that follows the gap is not written yet.
- **A bigger chip on a 128K board may not boot at all.** Pins 1 and 30 are
  unconnected on a 29F010 and are A18 and A17 on a 29F040, so on a board
  that leaves them floating or tied high the console sees another quarter of
  the chip - blank, if the image was written at the start. Writing the 128K
  image four times over fills every quarter.

The hex editor's EXP1 region is `TINROM_ROM_SIZE` long in every payload,
this one included, so on a 128K cart its end shows mirrors and the I/O
registers rather than flash.

## What the cart carries

`tinrom.psexe`, the same payload as the boot disc, unless
`TINROM_ROM_EXCLUDE_MODULES` leaves something out of the cart alone. By
default it leaves out the flasher, so that a cart is never asked to rewrite
the chip it booted from; the boot disc keeps it, and can recover a cart.
The cart then gets `tinrom_cart.psexe`, built from the same sources with
those modules left out, and the configure summary says so:

```
--   cart rom       : tinrom_cart.psexe - left out: flasher
--   boot disc      : same payload as the rest
```

That is a second build of most of tinrom; an empty list skips it.
`TINROM_ISO_EXCLUDE_MODULES` is the boot disc's, empty by default, and a
disc whose list matches the cart's shares its payload. The
`TINROM_MODULE_*` options apply to every image on top; the memory card's own
list is in [memcard-boot.md](memcard-boot.md).

## The switch

Cheat carts have a switch, and it does not bank the ROM: it is one bit the
cart's logic hands back from a register. tinrom can read it and keep two
**profiles**, one for each position - the feature level, which modules are
on, and what to autoboot. Everything else in Settings (video, serial, the
log) is the console's and stays one set.

Settings -> **Cart switch**:

| Row | |
| --- | --- |
| Read it | Not at all (the default), Xplorer-style or Action Replay-style |
| Swap when flipped | In the main menu, or only at power-on |
| Setting up | Which position's profile is in use, for setting up the other without flipping |
| Copy to the other position | Start the other profile from this one |
| Test the switch | Reads it now |

Where the bit is, from psx-spx "Cheat Devices", bit 0 and 1 for on in both:

| Kind | Register | Carts |
| --- | --- | --- |
| Xplorer-style | `1F060000h` | Xplorer; pcsx-redux's PIO cart switch reads here too |
| Action Replay-style | `1F020018h` | Datel: Action Replay, GameShark |

Nothing on a cart says which kind it is, and on one without that register
the address reads as ROM - which is why choosing is the user's, and why
"Test the switch" is there: flip it, and the reading should follow. Each
read takes the majority of three, a millisecond apart.

At power-on the switch is read once, after the settings are loaded and
before autoboot, so "off" can boot straight into GDB while "on" shows the
developer menu. With **Swap when flipped: in the menu** it is read again
four times a second while the main menu is up, and two readings running
must agree before the menu swaps - a bounce does nothing. In any other
screen a flip waits until you are back at the main menu. The main menu's
title says which position is in use.

Holding Select and Start at power-on still skips autoboot, whichever
profile asked for it.

**Saving.** The switch-off profile is in the settings section, exactly where
a build without any of this reads it; the switch-on profile and the switch
choices are a section of their own (`SWCH`) in the same save. Settings saved
by a build with other modules redo both profiles' module choices and drop
both autoboots, since all of them are registry indices.

