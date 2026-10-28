# Roadmap

`grep -rn UNIMPLEMENTED src/` is the authoritative list. This is the order
that list wants doing in, and why.

## 1. Make the ROM target trustworthy

Nothing else matters if the cart image is unreliable.

- **Instruction cache invalidation in `rom_stub.S`.** The stub jumps into
  memory it has just written without flushing I-cache. Works on a cold boot,
  not in general. Needs the COP0 isolate-and-invalidate sequence inline,
  because the kernel's `FlushCache` does not exist on the pre-boot path.
- **Boot source detection in `psexe_main.c`.** The stub passes a marker in
  `$a0`; crt0 forwards it as `argc`. Agree the contract and read it, instead
  of hardcoding `TINROM_BOOT_PSEXE` - a cart booted mid-boot still logs
  `boot source: 1`. Now matters more: there are three ways in (post-boot,
  mid-boot, uploaded) and they arrive in different states.
- ~~**Confirm the mid-boot trampoline survives kernel init.**~~ Done -
  confirmed on a retail SCPH-5552, cart with no disc, full trace through to
  the payload running. See docs/cart-rom.md.
- **`tinromExpInit()`** decides "I came from this cart" by the descriptor's
  load address and entry point, which an uploaded copy of the same build
  shares. Comparing the descriptor's `buildId` with the running payload's
  would tell the two apart.

## 2. Settings persistence

- ~~Memory card~~ - done: settings and the terminal's bookmarks are tinrom
  saves on either card, loaded from card 1 first. See "tinrom's own saves" in
  [memory-cards.md](memory-cards.md).
- ~~`moduleEnableMask` across builds~~ - done: the save carries a hash of the
  module ids, and a mismatch resets only the module selection.
- Cartridge flash/EEPROM: still a stub, and needs a reserved sector.
- Settings are loaded after the HAL is up (the cards need SIO0), so a saved
  video mode, serial rate or log mirror is applied a moment later than the
  defaults were - video is re-initialised once at boot.

## 3. The CD-ROM driver

~~Command and response plumbing, `GetID`, `SetLoc`/`ReadN`, the ISO9660
walker, and the boot path: `SYSTEM.CNF`, load the executable, hand over~~ -
done. `cdboot` and `filebrowser` both launch through `uiLaunchExe()`, and the
CD player plays audio. What is left:

- **Confirm the read pause fix** (`stopReading()`) on a console. In
  pcsx-redux it is confirmed: Crash Bandicoot boots from Boot disc -> Boot
  the disc.
- **`tinromCdStop()` waits only for Stop's first answer.** Stop answers
  twice, like Pause, and its second answer can land in the middle of the
  next command in the same way. Waiting for it costs up to a spin-down, so it
  wants doing where a pause in the UI is acceptable.
- **The swap trick**, for the consoles the unlock does not cover.
- Disc identification for `sysinfo`, and most of `cheats`.

## 4. The memory card driver

~~The SIO0 bus, probe, reading frames, directories, icons and whole
images~~ - done. ~~`tinromMcWriteFrame`, and copy, delete and undelete~~ -
done; see [memory-cards.md](memory-cards.md). tinrom's own saves write
through it too. What is left:

1. Format (`tinromMcFormat`).
2. Whole-card images: `tinromMcWriteImage`, and the host side of
   `MCDN`/`MCUP`, which back up and restore through them.
3. The capacity check. The quick one - a read past the end, which most
   clones answer wrongly - runs on every card read. The deep one writes a
   pattern across the whole card and reads it back, which also catches
   clones that alias within range (`tinromMcVerifyCapacity`).

## 5. Serial feature completion

- `SEXE`: parse the PS-EXE header, receive the body, and hand over through
  the same `uiLaunchExe()` path the disc uses.
- `SROM`: needs §6.
- `MCDN`/`MCUP`: needs §4.
- Retransmit on checksum mismatch, rather than reporting and moving on.
- **Modules' `shutdown` hooks are not called on a launch.** `uiLaunchExe()`
  shuts down the HAL but not the modules, and `ctx->handoff`, which would,
  is unused. Nothing has needed it yet; something holding hardware a game
  will trip over would.

## 6. The flasher

1. `tinromFlashIdentify()` - JEDEC software ID.
2. A command table keyed on manufacturer/device id (Sanyo, SST, AMD families).
3. Erase and program, **receiving and checksumming the replacement image
   before erasing anything.** A failed flash with the old image already gone
   is how people end up with a dead cart.

## 7. Debugging

0. ~~The resident exception handler~~ - done. It owns `80000080h` while
   armed, services PCDRV breaks and halts in the debugger on anything else.
   See [exceptions.md](exceptions.md).
1. ~~A crash screen for games~~ - done, from a cart: the kernel's A(40h) is
   pointed at `src/crash` in the cart's ROM. **Not yet seen on a console**:
   GDB -> Crash on purpose is the test (hardware-tests.md 1.7). If the kernel
   turns out not to call A(40h) through its table, the fallback is an
   `F0000010h` event, at the cost of an event slot for every game. Disc and
   memory card builds have nowhere a game cannot overwrite to keep one.
2. ~~The GDB stub~~ - done as the resident monitor (`src/monitor`): GDB's
   remote protocol directly on SIO1, and the nops debug commands `nops /gdb`
   uses. See [debugger.md](debugger.md). What is left:
   - Ctrl-C into a running program, which needs an SIO1 interrupt path.
   - Read/write watchpoints, which need the COP0 data breakpoint and a
     second vector at `80000040h` that the mid-boot takeover shares.
   - Chaining to the kernel's handler, so programs that use interrupts can be
     debugged.
3. **The "debugger at boot" setting** (`debuggerAtBoot`) is defaulted and
   saved, but nothing reads it and no screen shows it. Wire it up, or drop it
   at the next settings version.

## 8. Memory card boot

See `docs/memcard-boot.md`. This is what gets tinrom onto a PSone.

## Smaller things

- ~~IRC client~~ - done, one channel at a time; see [irc.md](irc.md). More
  than one window at once would want a screen's worth of history each.
- **A keyboard.** There is no standard PS1 keyboard adapter, so this waits on
  a choice of hardware. The terminal and the IRC client take key codes
  (`ui/keyboard.h`), so a driver only has to produce them.

- **A packet buffer of tinrom's own.** tinrom inherits ps1-bare-metal's chain,
  which upstream doubled to 4096 words in 2af848a - room for any screen
  tinrom has, at 16 KB more RAM for the pair. Budgeting and graceful
  degradation still guard it (see the packet budget section in
  docs/architecture.md). Owning the chain sizing would let tinrom pick the
  size itself, but needs its own allocator; the text renderer is already its
  own. The hex editor sidesteps it with the text
  canvas (`src/ui/canvas.c`), which the log viewer could use too.

- **Analog pads and multitap.** `pad.c` drops the extra half-words an analog
  pad sends. Harmless for digital use, wrong for anything else.
- ~~Autoboot picker~~ - done, in Settings.
- **Module list screen** in the about module.
- **Hidden button combinations**, as UniROM has. Wants a general input-gesture
  layer rather than special cases scattered through modules.
- **Host-side tests** for `makerom.py` - it is pure byte manipulation and
  deserves them. `scripts/test-unpack.sh` covers the packed payload's layout;
  the crash screen's placement and checks are not covered yet.
