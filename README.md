# tinrom

A modular, replacement utility ROM for the PlayStation 1, heavily inspired by UniROM.

## Preamble
This is a personal project! I do not assume it will meet nor exceed other mature and battle-tested projects that do this!

This is initially an AI co-authored project to get a rough layout, gap-fill and proof-of-concept the idea.
Assume all code is dirty unless specifically marked as NOT AI.
I want to replace that with human code - the only prerequisites is that the new code is:
- modular,
- maintainable,
- follows the goals,
- public, free and permissively licenceable code.

## Goals

- Everything must be modular!
- Rely heavily against ps1-bare-metal SDK:
    - If something exists there or has an example - use that.
    - If something didn't exist but now does, retire the tinrom version for the SDK's.
    - Try to write the module in anticipation of the SDK supporting the feature later, using the HAL layer as the intermedery, making the above simple to maintain.
- Replace any and all AI PoC.

## Why

UniROM is excellent and has been the answer for years. tinrom exists because:

- **Fun.** I wanted something I could tinker with and throw ideas at, without
  having to fight with it all again in a year when I revisit it.
- **Modular.** Every feature is a module which can be included/excluded
  by a CMake option and shown or hidden by a runtime tier. A build that only
  boots discs and manages saves is a preset, not a whole fork.
- **Attributed.** `NOTICE.md` records every piece of third-party material,
  including the documentation the work was done from.
- **No Sony code.** No PsyQ, no SDK fragments, no BIOS disassembly. BIOS
  kernel calls are allowed where they are the only route, and each is marked
  `TINROM_KERNEL_CALL` so it can be audited and replaced.
- **Built on [ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal).**
  Toolchain, libc, register definitions and GPU helpers come from there
  unmodified. It is a git submodule, which the bootstrap script checks out:
  the supported revision, or upstream's latest - see `extern/README.md`.

## What it builds

| Target | Output | What it is |
| --- | --- | --- |
| `tinrom` | `tinrom.psexe` | The payload. Upload over serial or run in an emulator. |
| `tinrom_rom` | `tinrom.rom` | Flashable EXP1 cart image (Xplorer / AR / Caetla-compatible). *my primary dev path, tested via pcsx-redux* |
| `tinrom_iso` | `tinrom.bin` + `.cue` | Boot disc. Needs `mkpsxiso`; see `docs/boot-disc.md`. |
| `tinrom_mcr` | `tinrom.mcr` | FreePSXBoot-style memory card image. **Stub! does not boot yet.** |

By default the `.rom` carries `tinrom_cart.psexe`: the same payload without
the flasher, so a cart is never asked to rewrite itself
(`TINROM_ROM_EXCLUDE_MODULES`). Around it the ROM adds a header, a loader stub
and the crash screen games get (`TINROM_ROM_CRASH_SCREEN`).

## Building

You need a `mipsel-none-elf` GCC toolchain on `PATH` (the
[pcsx-redux `mips` script](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/psyqo/GETTING_STARTED.md)
is the usual way to get one), CMake 3.25+, Ninja and Python 3.10+.

**No Python packages are needed to build.** Every build-time script uses only
the standard library, so a bare virtual environment is enough. In particular
tinrom uses its own standard-library version of ps1-bare-metal's
`convertImage.py` (`tools/convertimage.py`), which would otherwise pull in
NumPy and Pillow, which my setup was misbehaving with.
`pyserial` is optional and only needed to talk to a real console with `tinrom_host`.

`mkpsxiso` builds the boot disc. The bootstrap script fetches it into
`tools/bin/` (it is GPL-2.0, so tinrom does not carry it; see `NOTICE.md`),
and CMake looks there before `PATH`. Without it the boot disc target is
skipped and the other three still build.

Run the bootstrap script once, and again after a pull that moves the
ps1-bare-metal submodule. It checks ps1-bare-metal out - it asks whether you
want the supported revision or upstream's latest - creates the Python
virtual environment at `./env` that the build scripts expect, and fetches
mkpsxiso:

```powershell
# Windows PowerShell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```

```bash
# Linux, macOS, MSYS2, Git Bash
./scripts/bootstrap.sh
```

Then:

```bash
cmake --preset debug
cmake --build build/debug
```

Outputs land in `build/debug/`. If the toolchain is not on `PATH`, pass
`-DTOOLCHAIN_PATH=/path/to/toolchain/bin`.

### ps1-bare-metal

A git submodule at `extern/ps1-bare-metal`: tinrom's tree records which
commit of it to use, not its files. The bootstrap script asks which to check
out:

1. **supported**, the default: the commit tinrom records, which it is built
   and tested against.
2. **latest**: upstream's newest `main`, which may need fixes in tinrom
   first, because upstream changes its API.

`--supported` or `--latest` (`-Supported` or `-Latest` in PowerShell) skips
the question. The configure summary says which revision a build is using.
Once tinrom builds against the latest, committing the submodule makes it the
supported one; see `extern/README.md`. To build against
a checkout somewhere else instead, point CMake at it:
`cmake --preset debug -DTINROM_BARE_METAL_DIR=/path/to/ps1-bare-metal`.

### Notes:

Use the same environment for `bootstrap` and for `cmake`. CMake resolves the
Python virtual environment by path, and a native Windows Python creates
`env\Scripts\` while an MSYS2 Python creates `env/bin/`.
Using both against the same project will give you headaches.

Three presets are provided: `debug`, `release`, and `minimal` (basic tier,
developer modules compiled out, 128K ROM).

## Testing over serial

The console side speaks the nops wire protocol, so existing `nops` builds and
cables work against tinrom too. See `docs/nops-protocol.md`.

GDB can debug over the same cable, either pointed straight at the serial port
or through `nops /gdb`: load an ELF, set breakpoints, step, inspect. See
`docs/debugger.md`.

The resident debug monitor has host-side tests that need no console:
`sh scripts/test-monitor.sh`.

## Where things are

```
cmake/            build helpers: options, module table, rom/iso/mcr targets
docs/             architecture, protocol, roadmap, attribution detail
extern/           ps1-bare-metal, as a git submodule
iso/              mkpsxiso project for the boot disc
src/boot/         entry points: PS-EXE main, cart ROM stub and header
src/core/         module registry, settings, tiers, logging, main loop
src/hal/          hardware access (the UI's GPU flushes aside)
src/modules/      one directory per feature
src/ui/           menus, widgets, drawing
tools/            makerom.py, makemcard.py, tinrom_host (PC side)
```

## Status

See `docs/roadmap.md` for what is stubbed and the order it wants building in,
and `docs/feature-parity.md` for how the module list maps onto UniROM's
features.

## Requirements

Anything benificial to ps1-bare-metal (fixes, examples, etc.) must be offered upstream in accordance with the projects rules.
ps1-bare-metal must be definitive - fixes or workarounds must be applied in the tinrom HAL unless ps1-bare-metal accepts the fix.

## Licence

Usage in commercial applications require:
 - Permission from TinWhisker.
 - Permission from spicyjpeg.
 - The product provided as gifts for each contributor (as individually determined by TinWhisker & spicyjpeg).


MIT: see `LICENSE`.
Third-party attribution is in `NOTICE.md`.
Please read it before adding a dependency.

## Testing on a console

Most of tinrom is checked on a host before it ever runs. `sh scripts/test-all.sh`
runs all of it and lists what failed: `scripts/syntax-check.sh`, which parses
every source, and every `scripts/test-*.sh` - the disc, handoff and
host-filesystem code against real images and a real server, CD audio, data
reads and the visualiser against a fake drive, the cart's crash screen over a
software GPU, the hex editor, memory card saves, the terminal, IRC and the
debug monitor.

What that cannot cover is the hardware. `docs/hardware-tests.md` is an ordered
sheet for a console: each test depends only on the ones above it, so a failure
explains the ones below rather than needing to be chased separately.
