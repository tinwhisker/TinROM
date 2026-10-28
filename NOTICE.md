# tinrom — attribution and provenance

tinrom is a boot rom, inspired by UniROM. It is licensed
under the MIT License (see `LICENSE`).

This file is the **attribution ledger**. Every piece of third-party material —
code, data, or documented knowledge — that tinrom depends on or was derived
from is recorded here. Adding third-party material without a matching entry
here is a bug.

---

## 1. Bundled / linked source

### ps1-bare-metal — `extern/ps1-bare-metal` (git submodule)

- Author: spicyjpeg
- Upstream: https://github.com/spicyjpeg/ps1-bare-metal
- Licence: MIT
- Usage: a git submodule at `extern/ps1-bare-metal`. tinrom's tree records
  the commit, not the files, and its MIT licence comes with the checkout in
  `extern/ps1-bare-metal/LICENSE`. `scripts/bootstrap.ps1` / `.sh` check out
  the commit tinrom records, or upstream's latest on request. It was a
  submodule once before, then until 2026-10 a git subtree, then a plain clone
  pinned by a text file; the squash commits of the subtree stay in the
  history. tinrom reuses its `cmake/toolchain.cmake`,
  `cmake/setup.cmake`, `cmake/tools.cmake`, `cmake/executable.ld`,
  `tools/convertExecutable.py`, the `src/libc`
  minimal C library, the `src/ps1` hardware register definitions (including
  `ps1/cdromdef.h` for the CD-ROM driver and `ps1/delay.h` for busy-waits),
  and the `src/common` GPU/GTE helpers and `flushCache()`.
  `common/cache.s` is linked a second time, unmodified, into the resident
  debug monitor (`cmake/monitor.cmake`), which has to be able to flush the
  instruction cache after tinrom's own copy has been overwritten.
- Techniques adopted, not code linked: `src/hal/cdrom.c` initialises the drive
  and reads `HINTSTS` the way `src/13_cdromInit/main.c` does, with that example
  credited at each point in the source. It deliberately does not copy the
  example's `BIU_COM_DELAY` write; see the comment in `tinromCdReset`.
  Its CD audio sets the drive's volume matrix and the SPU's CD input the way
  `src/14_cdAudio/main.c` does, and `src/hal/spu.c` uses the SPU bus timing
  value that `common/spu.c`'s `initSPU()` builds - but not `initSPU()`
  itself, which also writes `BIU_COM_DELAY`. Its sound RAM DMA read raises
  the bus's DMA cycles to two for the read, as `receiveSPURAMData()` does.
  The CD player's visualiser (`src/modules/cdplay/scene.c`) transforms,
  culls and depth-sorts with the GTE the way `src/ui/model.c` does, after
  examples 08 and 10, and uses `common/trig.c`'s `isin()`.
- Modifications: none. It is used unmodified; tinrom-specific behaviour
  lives in this repository, and fixes are offered upstream rather than
  carried. The bootstrap script refuses to move a checkout with local
  changes.

### tools/convertimage.py — derived from ps1-bare-metal

- Derived from: `extern/ps1-bare-metal/tools/convertImage.py` by spicyjpeg
- Licence: MIT (same as the original)
- Relationship: a reimplementation, not a copy. The conversion semantics —
  the 8-bit-to-5-bit colour rounding, the alpha thresholds, the black-pixel
  fixup, palette ordering and padding, and the 4bpp nibble packing — are
  upstream's, and were followed deliberately so the output is interchangeable.
  The code is new and uses only the Python standard library.
- Why: upstream's version needs NumPy and Pillow.

### tools/mktim.py, tools/mkmesh.py, tools/tmd.py, tools/bbmodel.py

- Author: TinWhisker. Written for my ps1-python
  project and brought across.
- Licence: MIT, under this repository's `LICENSE` — the author's own work, so
  nothing further is owed.
- Usage:
  - `mktim.py` supplies `read_png()`, a pure standard library PNG decoder.
    `tools/convertimage.py` is built on it, and that is what lets tinrom build
    with no third-party Python packages at all.
  - `mkmesh.py` (with `tmd.py` and `bbmodel.py`) exports the PS1M mesh the
    menu background model is built from. `ps1port/mesh.h` in ps1-python is the
    format's specification; `src/ui/model.h` is tinrom's reader for the subset
    it needs.

### The rom model — src/ui/objects/

- `rom.obj`, `rom.png`, `rom.mtl`, `rom.bbmodel`: the maintainer's own
  artwork, made in Blockbench. MIT under this repository's `LICENSE`.


### printf — vendored inside ps1-bare-metal

- Author: Marco Paland (`mpaland`)
- Upstream: https://github.com/mpaland/printf
- Licence: MIT
- Usage: reached transitively through ps1-bare-metal's `src/vendor/printf.c`.

### The terminal font — `src/ui/fonts/term4x8.txt`

- The 4x8 CP437 font the terminal draws with is an original design, drawn for
  tinrom as text, with its box-drawing characters laid out by rule. MIT, under
  this repository's `LICENSE`. No other font's bitmaps were copied. Its
  printable ASCII is also the cart's crash screen's font (`src/crash`).

### The UI font — `src/ui/fonts/font-20x20.png`

- From: the Superpowers asset packs, http://superpowers-html5.com/
- Licence: **CC0 1.0** (public domain dedication),
  http://creativecommons.org/publicdomain/zero/1.0/ - "Attribution is not
  required but appreciated. Placing a link to http://superpowers-html5.com/
  somewhere would be awesome." Credited here, in `src/ui/fonts/README.md`, and
  with the link on the About -> Credits screen.
- The 20x20 version of the font whose 10x10 sheet the maintainer's ps1-python
  project uses (`ps1python/font.tim`).
- Usage: converted at build time by `tools/convertfont.py` (tinrom's own,
  standard library only) into the tables in `src/ui/font.h` - every column
  kept for the 640-wide screen, rows scaled down to 10 - and used for all of
  tinrom's text, proportional and monospaced. The file in the repository is
  unmodified.
- ps1-bare-metal's own font (`extern/ps1-bare-metal/assets/font.png` and
  `src/common/font.c`) is no longer built or embedded. It stays in the
  checkout, which is used unmodified.

### mkpsxiso — fetched by the bootstrap script, not carried

- Author: John Wilbert Villamor (Lameguy64) and contributors
- Upstream: https://github.com/Lameguy64/mkpsxiso
- Licence: **GPL-2.0** (`LICENSE.md` upstream)
- Usage: a build tool, run to make the boot disc image (`cmake/iso.cmake`).
  tinrom neither carries nor links it: `tools/fetch_mkpsxiso.py`, run by the
  bootstrap script, downloads the v2.30 release for the platform from the
  project's own GitHub releases, refuses it unless its SHA-256 matches the
  one recorded when that version was pinned, and unpacks `mkpsxiso` and
  `dumpsxiso` into `tools/bin/`, which git ignores. The GPL covers the tool,
  not the disc images it builds from tinrom's own files.
- An earlier tree carried a copy as `tools/mkpsxiso.exe`, without an entry
  here; it was removed.

---

## 2. Documented knowledge (no code reused)

These sources were read to implement tinrom. No code was copied from them unless stated;
facts about hardware behaviour and wire formats are not themselves
copyrightable, but the sources are credited because the work would not exist
without them.

### psx-spx (Playstation Specifications)

- Author: Martin Korth (nocash), with the psx-spx community
- Upstream: https://psx-spx.consoledev.net / https://github.com/psx-spx
- Used for: the EXP1 expansion ROM header layout (`docs/cart-rom.md`),
  SIO0/SIO1 behaviour, CD-ROM command set, memory card block format,
  COP0 exception and hardware-breakpoint behaviour, and the Expansion
  Region 2 register map including the PCSX-Redux debug registers at
  `1F802080h` (`src/hal/dbgcon.c`). Also the memory card read command
  sequence, the directory and title frame layouts, the Shift-JIS ranges used
  in save titles, and the save icon format - bitmap depth, palette offset,
  display flag, animation rates and the transparent/solid-black palette
  convention (`src/hal/mcard.c`, `src/ui/ui.c`, `docs/memory-cards.md`), as
  read and as written for tinrom's own saves (`src/core/savefile.c`; the icon
  itself is original).
  Also the cheat carts' switch registers - `1F060000h` on Xplorer carts and
  `1F020018h` on Datel ones, bit 0 - from "Cheat Devices" (`src/hal/exp.c`).
  Also the CD audio commands (`GetTN`, `GetTD`, `GetlocP`, `Play` and its
  track parameter, `Forward`, `Backward`, `Pause`, `Mute`, `Demute`), Setmode's
  CDDA and AutoPause bits, the `ATV0`-`ATV3` volume matrix, and the SPU's
  control, status and CD input volume registers (`src/hal/cdrom.c`,
  `src/hal/spu.c`, `docs/cd-audio.md`). And the reverb unit: its registers,
  its formula, the work area and the manual sound RAM write sequence used to
  clear it (`src/hal/spu.c`); and the CD capture buffers - their place in
  sound RAM and the SPUSTAT bit that says which half is being written - with
  the DMA read that fetches them (`src/hal/spu.c`, the CD player's
  visualiser). The reverb settings themselves are original,
  tuned on a model of that formula; psx-spx's "Reverb Examples", which come
  from Sony's libraries, are not used. The effect names - dome, hall,
  church, studio, direct - are the ones the console's own CD player shows.
  Also the CD-ROM host interface and command set, the secret unlock sequence
  at `50h`-`56h` and the region strings it needs, the ISO9660 and PS-EXE
  layouts, and the documented bug in the kernel's `FlushCache`
  (`src/hal/cdrom.c`, `src/core/iso9660.c`, `src/core/psexe.c`,
  `src/hal/handoff.S`, `docs/cd-boot.md`). And the PCDRV break interface -
  the seven call numbers, their unusual A1/A2/A3 argument order, the two
  return values, and the documented fact that PCRead pads past end of file
  (`src/core/pcdrv.c`, `docs/pcdrv.md`). And for the cart's crash screen
  (`src/crash`, `src/hal/crash.c`): the kernel's handling of an unresolved
  exception - `DeliverEvent(F0000010h, 1000h)`, then
  `A(40h) SystemErrorUnresolvedException` - and the A(nnh) jump table at
  `200h` it goes through; the process and thread control block pointers at
  `108h` and `110h`; the GPU's reset, display and FIFO status bits; and the
  break codes of the compiler's division checks, `BRK(1C00h)` and
  `BRK(1800h)`. The thread control block's register layout is the one
  already used for nops (`docs/debugger.md`). The screen draws with
  tinrom's own terminal font.
  Also, for System information (`src/hal/console.c`): where the BIOS keeps
  its kernel date (`BFC00100h`), maker string (`BFC00108h`) and version
  text, from "BIOS Versions" - read at run time as text, nothing of the BIOS
  kept; `GP1(10h)` and its index `07h`, which only the later GPU answers;
  the RAM size register at `1F801060h` and the 2 MB mirror; and COP0 `PRId`.
  The empty card a format writes - header, directory, broken sector list
  and write-test frame - is "Memory Card Data Format" (`src/hal/mcard.c`).

### Flash chip datasheets: the JEDEC software-ID sequence

- Authors: AMD, Atmel, Fujitsu, Hynix, Macronix, SST, ST and Winbond, each
  for its own parts; the maker codes are JEDEC JEP106
- Used for: the software-ID command sequence (`AAh` to `5555h`, `55h` to
  `2AAAh`, then `90h` to enter and `F0h` to leave), how long Atmel's parts
  take to switch, and the maker and part codes in the table of chips carts
  use (`src/hal/exp.c`). Codes and timings only; no code.

### OpenBIOS (part of pcsx-redux)

- Author: the PCSX-Redux authors
- Upstream: https://github.com/grumpycoders/pcsx-redux
  (`src/mips/openbios/`)
- Licence: **MIT, per file.** Worth stating precisely, because the pcsx-redux
  repository's own `LICENSE` is GPL-2.0: it is a PCSX fork. The files under
  `src/mips/openbios/` each carry their own MIT header ("Copyright (c) 2019
  PCSX-Redux authors"), and that header governs them. So OpenBIOS may be
  borrowed from with attribution; the rest of pcsx-redux may not, under
  tinrom's current licence.
- **No OpenBIOS source is used in tinrom today.** Used so far for: confirming
  that the console's boot ROM gates the EXP1 entry points on the ID string,
  and how strictly it compares it. `src/mips/openbios/pio/pio.c` is quoted in
  `docs/cart-rom.md` as evidence for a claim about hardware behaviour, not
  copied.

### Unicode Character Database

- Author: the Unicode Consortium
- Upstream: https://www.unicode.org/ucd/
- Licence: Unicode Licence v3 - **no Unicode data ships in tinrom.**
- Used for: the 88-entry Shift-JIS punctuation table in `src/hal/mcard.c` is
  the NFKC folding of Shift-JIS `8140h`-`8197h`, computed once from the
  standard normalisation data and written into the source as plain ASCII
  characters. Twelve entries are hand-chosen approximations and are marked
  as such. No table, file or code from the UCD is redistributed.

### tonyhax (not used)

- Author: socram8888 and contributors
- Upstream: https://github.com/socram8888/tonyhax
- Licence: **WTFPL** - permissive, and compatible with tinrom either way.
- **No tonyhax source is used in tinrom.** The disc booting and drive
  unlocking in `src/hal/cdrom.c` and `src/hal/handoff.c` were written from the
  psx-spx specification, which documents every command and register involved.
  tonyhax is named here because source code was read to understand its workings.

### PCSX-Redux debug expansion registers

- Author: the PCSX-Redux authors
- Upstream: https://pcsx-redux.consoledev.net/mips_api/ and
  https://psx-spx.consoledev.net/iomap/
- Licence: MIT — **no PCSX-Redux source is used in tinrom.**
- Used for: the four emulator-only registers at `1F802080h`-`1F802084h` (ID,
  console putchar, debug break, exit code) and the fact that EXP2 must be
  widened from 128 to 256 bytes via `BIU_DEV8_CTRL` before they are reachable.
  `src/hal/dbgcon.c` was written from that description; Redux's own `crt0.s`
  was read as evidence for the widening step and is quoted, not copied, in
  `docs/architecture.md`. Its PIO cart emulation was read to confirm that
  its switch is the Xplorer-style register; nothing of it is used.

### SN Systems PCDRV interface

- Author: SN Systems, as documented by Martin Korth (nocash) in psx-spx
- Upstream: https://psx-spx.consoledev.net/ ("BIOS PC File Server")
- Used for: the seven break numbers and their register conventions only. The
  transport underneath them was always hardware-specific - a DECI link on a
  devkit, a cheat cartridge for Caetla - so tinrom's serial framing is its
  own, documented in `docs/pcdrv.md`. No SN or Caetla code is used.

### NOTPSXSerial ("nops")

- Author: JonathanDotCel and contributors
- Upstream: https://github.com/JonathanDotCel/NOTPSXSerial
- Licence: MPL-2.0 — **no NOTPSXSerial source is used in tinrom.**
- Used for: the observable wire protocol only. `docs/nops-protocol.md`
  documents the four-character tokens, the memory transfers and their
  checksums, the rate changes, and what `nops /gdb` sends to a console, so
  that tinrom's console side (`src/modules/siocons`, `src/monitor/nops.c`) is
  wire-compatible with existing `nops` builds. The description was produced
  by a separate pass that studied nops' behaviour; tinrom's implementation was
  written from that description and shares no code, names or comments with
  NOTPSXSerial.

### Terminal standards: ECMA-48, VT100, ANSI.SYS, telnet

- ECMA-48 ("Control Functions for Coded Character Sets"), the DEC VT100 user
  guide's description of its sequences, and the documented behaviour of DOS's
  ANSI.SYS, which is the dialect BBSes speak - read to write
  `src/comms/term.c`.
- RFC 854 (Telnet), RFC 1091 (Terminal-Type) and RFC 1073 (Window Size) for
  `src/comms/telnet.c`.
- CP437's character repertoire, for which glyphs `term4x8.txt` draws at each
  code. No code or font data from any of these is used.

### IRC: RFC 1459, RFC 2812, and the community's documents

- RFC 1459 and RFC 2812 for the message format, registration, the commands
  and numerics `src/comms/irc.c` handles, and RFC 1459's case mapping.
- modern.ircdocs.horse (the IRC "Modern" documentation project) for the
  ISUPPORT tokens PREFIX, CHANMODES and CHANTYPES, for CTCP (ACTION, VERSION,
  PING, TIME, CLIENTINFO) and for mIRC's formatting codes and its sixteen
  colours (`src/comms/irctext.c`).
- No client's code was read; `irc.c` and `irctext.c` were written from these.

### Cryptography: FIPS 180-4, RFC 2104, RFC 8018, RFC 8439

- SHA-256 from NIST FIPS 180-4 (its constants are the specification's own:
  fractional parts of the roots of the first primes), HMAC from RFC 2104,
  PBKDF2 from RFC 8018, ChaCha20 from RFC 8439 - written from the documents
  into `src/comms/crypto.c`. The tests use the published vectors of FIPS
  180-4's examples, RFC 4231 and RFC 8439, and PBKDF2 values computed with
  Python's hashlib.

### CP437 to Unicode

- The mapping from each CP437 byte to the character it shows, used by
  `src/comms/irctext.c` to put UTF-8 on the terminal's screen. The 128 pairs
  were produced once from Python's `cp437` codec, which follows the Unicode
  Consortium's published table, and written into the source as numbers. The
  look-alikes below it (curly quotes as straight, dashes as `-`) are
  tinrom's own choices.

### The Hayes command set, as WiFi modem adapters document it

- The result words and numbers, the `+++` escape with a second's guard time
  either side, `ATH`, `ATD` with a host and port, `&K` and `&D`, from the
  public documentation of the Hayes command set and of WiFi-to-serial adapters
  that implement it. `src/comms/modem.c` was written from that; no adapter
  firmware was read.

### GDB remote serial protocol

- Author: the GDB project (Free Software Foundation)
- Upstream: https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html
- Used for: the protocol and the MIPS target-description feature and register
  names GDB looks for. `src/monitor/rsp.c` and the target description it
  sends were written from GDB's documentation; no GDB source (GPL) is used.

### UniROM

- Author: JonathanDotCel and contributors
- Upstream: https://github.com/JonathanDotCel/unirom8_bootdisc_and_firmware_for_ps1
- Used for: the **inspiration and feature list only** (see `docs/feature-parity.md`). tinrom
  shares no source with UniROM.

### FreePSXBoot

- Author: brad-lin and contributors
- Upstream: https://github.com/brad-lin/FreePSXBoot
- Used for: the memory-card exploit concept. `tools/makemcard.py` is a stub;
  **before wiring a real exploit payload in, record its licence here and
  confirm it is compatible with MIT redistribution.**

---

## 3. Deliberately absent

- No Sony code, headers, libraries or SDK fragments (no PsyQ
  libraries, no BIOS dumps, no kernel disassembly pasted in).
- tinrom does call the BIOS kernel's public A0/B0/C0 vectors where that is the
  only way to reach a facility. Calling a documented entry point is not
  reuse of Sony code; no Sony code is copied into this repository. Every such
  call site is marked with a `TINROM_KERNEL_CALL` comment so the dependency
  can be audited and, where possible, replaced with a native driver.
- The string `"Licensed by Sony Computer Entertainment Inc."` appears in
  `tools/makerom.py`, and the header layout it belongs to is described in
  `src/boot/rom_header.h`. It is a magic value the console's boot ROM tests
  for before it will call an expansion ROM entry point — it is an interface
  constant, not Sony code. It is gated behind the CMake option
  `TINROM_ROM_LICENCE_STRING` so you can build without it; see
  `docs/cart-rom.md` for what that costs you.
