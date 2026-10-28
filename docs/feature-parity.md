# Feature parity with UniROM

UniROM's feature list is the specification tinrom is working towards. This
table maps it onto modules. Nothing here is copied from UniROM - the list is
what it does, not how.

| UniROM feature | tinrom module | State |
| --- | --- | --- |
| Mod-free boot (unlock / swap) | `cdboot` | partial: the unlock, on vC1 and later drives outside Japan; no swap trick |
| Disc identification | `sysinfo` | done: the drive, region, SCEx, licence, label and boot file, in System information -> Drive and disc |
| PAL↔NTSC soft patching | `regionpatch` | stub |
| CD file browser, launch EXE/ELF | `filebrowser` | partial: browse, launch a PS-EXE, copy a file to a host; no ELF |
| Memory card manager, format | `memcard` | partial: both cards' saves, icons and details; copy, delete, undelete and format |
| Card imaging to/from host | `memcard` + `MCDN`/`MCUP` | stub |
| Fake 512K card detection | `memcard` | partial: the quick test - a read past the end - on every card read; the destructive full test is a stub |
| Homebrew from memory card | `filebrowser` | stub: saves can be listed and copied to a host, not launched |
| Install to / backup from memory card | `memcard` | stub |
| FreePSXBoot install | `tools/makemcard.py` | stub: the image builds, but has no exploit, so a console does not boot it |
| Cart flashing, multiple EEPROM types | `flasher` | stub |
| Cart detection | `sysinfo`, `flasher` | done: what the cart holds, its switch, and its flash chip by JEDEC ID, on request |
| Kernel-resident SIO, debug a running game | `siocons` + resident monitor | partial: programs that run without interrupts; most games need them, and the monitor does not chain to the kernel's handler |
| `.EXE` upload over serial | `siocons` (`SEXE`) | stub |
| Serial up to 500k | `hal/sio` + `BAUD` | **done** |
| TTY redirected to SIO | `core/log` | **done** |
| Exception handler / crash screen | resident monitor; for games, the cart's crash screen (`src/crash`) | partial: a game that crashes gets a screen of registers and memory, from a cart only; not yet seen on a console |
| GDB debugging (direct, or via `nops /gdb`) | `gdb` + resident monitor | partial: load, break, step, continue; no Ctrl-C into a running program |
| Hex editor, RAM, ROM and memory card | `hexedit` | **done**: edit, find, follow, undo; cards frame by frame |
| PCDRV | `pcdrv` | **done**: tinrom's own file access, and serving a running program's calls while the trap is armed |
| - | `terminal` | **done**: ANSI BBSes over a WiFi modem; see terminal.md |
| - | `irc` | **done**: one channel at a time over the same modem; see irc.md |
| - | `cdplay` | **done**: audio CDs and the music on game discs; see cd-audio.md |
| Cheat engine | `cheats` | stub |
| Settings menu, autoboot | `settingsui` | partial: saved to and loaded from a memory card; not to the cart |
| Dual-boot with Caetla | - | not planned yet |
| Hidden button combinations | - | see roadmap |

"Partial" means the module works but some of what it offers is stubbed.

## Things tinrom does that UniROM does not

- Feature levels (basic / advanced / developer) so the menu can be short
  without maintaining a separate build.
- Compile-time module selection, so a 128K cart is a preset rather than a fork.
- A serial terminal for BBSes through a WiFi modem (`terminal`), and an IRC
  client on the same modem (`irc`).
- An audio CD player (`cdplay`), for audio CDs and the music tracks on game
  discs: repeat, stereo or mono, room effects on the SPU's reverb, and a 3D
  visualiser - spectrum, level meter or waveform - behind the track grid.
  See `docs/cd-audio.md`.
- A documented protocol spec rather than a reference implementation, so a
  third tool can be written against it.
