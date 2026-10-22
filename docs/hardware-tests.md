# Hardware test sheet

Everything below has been written and never run on a console. It is ordered so
that each test only depends on the ones above it: a failure high up explains
failures below it, and stopping at the first one saves chasing ghosts.

Take one column: **pass**, **fail**, or **not reached**.

## Before you start

- [ ] Build with `-DTINROM_LOG_TO_SERIAL=ON`. The mirror is then armed before
      the first line, so a boot that dies early still says something.
- [ ] Have `python -m tinrom_host monitor <port>` running, or PCSX-Redux with
      **PIOConnected** ticked. Redux needs nothing else - tinrom finds its
      console at `1F802080h` by itself.
- [ ] Note the console: model, region, and whether the drive firmware is vC0
      or later. Several results below depend on it.

Serial and the emulator console carry the same lines, so either is enough.

---

## 1. The exception handler

The cheapest thing to prove, and three other features sit on it.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 1.1 | Host filesystem -> Status | "trap not armed" | - |
| 1.2 | Host filesystem -> Arm the trap | "Trap armed", log line `exc: installed, handler in the monitor at 801e8000` | The vector write or the cache flush went wrong. At boot the log should also show `dbg: monitor at 801e8000, ...`; without it the monitor did not load and nothing in this section can work |
| 1.3 | Host filesystem -> Status | "trap armed" | - |
| 1.4 | **Host filesystem -> Test the trap** | A result screen appears, "serviced after" is one higher | **This is the test.** Reaching the screen at all means a break was trapped and execution resumed. A lock-up means the handler, the frame or the `rfe` path is wrong |
| 1.5 | Move around the menus for a minute | Nothing changes | A hang or reset means the handler is corrupting state it should have saved |
| 1.6 | Host filesystem -> Disarm the trap, then Status | "Trap disarmed", then "trap not armed" | The saved words were not restored |
| 1.7 | **With a cart built with the crash screen in: GDB -> Crash on purpose**, and confirm | The crash screen: "The game crashed.", "Address error on a load", `badv 0badc0de`, registers filled in. Log line `crash: A(40h) ... -> the cart's crash screen at bf00....` before it | **This is the crash screen's test** (see [exceptions.md](exceptions.md#the-crash-screen-games-get)). Frozen on tinrom's menu: the kernel does not reach A(40h) through its table - report it. A blank or garbled screen: the screen's GPU setup. "could not be found" where the registers should be: the kernel keeps them somewhere other than the TCB at `[[108h]]` |
| 1.8 | Boot a game that is known to crash (or a test program that divides by zero) with the cart in | The crash screen instead of a freeze, "Division by zero" for the test program | A freeze: as 1.7. A game that misbehaves *before* crashing never did so without the cart: nothing else of tinrom's stays behind |

A report naming the wrong exception is its own symptom: if the handler says
"interrupt" at an address like `24h` when you asked for a break, the COP0
reads have lost their load delay slots and every field is one late.
`scripts/check-hazards.py` catches that before it reaches a console.

If 1.4 hangs, stop. Everything in sections 5 and 6e depends on it, and
the monitor's `exc:` report will not print because the console never got that
far.

## 2. The CD-ROM command layer

No disc needed for the first two.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 2.1 | Boot disc -> Drive and disc, with an empty drive | A `drive` line with a version string | The command/response plumbing or the bank switching is wrong - nothing below will work |
| 2.2 | Same screen | A `console` line naming your region | Test `19h,22h` failed; on a vC0 drive this is expected and correct |
| 2.3 | Insert an original game for this console, re-enter | `disc data`, `SCEx SCEE`/`SCEA`/`SCEI`, `licensed yes` | GetID's second response is not arriving |
| 2.4 | Same screen | A `label` line with the disc's volume name | The ISO9660 mount worked, so sector reads work |
| 2.5 | Same screen | A `boot` line naming an executable | SYSTEM.CNF was read and parsed |

2.4 passing is the big one: it means `SetLoc`/`ReadN` and the sector copy all
work, which is most of the driver.

## 3. The unlock

Skip on a Japanese or Asian console - it is documented not to work and tinrom
will say so.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 3.1 | Insert a CD-R or an import. Boot disc -> Drive and disc | `licensed no` | - |
| 3.2 | Boot disc -> Unlock the drive | "Unlock: ok" | It reports honestly: the seven commands always answer INT5, so tinrom judges by reading a sector afterwards |
| 3.3 | Drive and disc again | `unlocked yes`, and a `label` line now appears | The sequence ran but the drive did not take it |
| 3.4 | Boot disc -> Re-lock, then Drive and disc | The label is gone again | Not important, but it proves 3.2 was real |

If 3.2 fails on a PAL or US console with vC1+, note the exact `console` string
from 2.2 - the region word sent in command `55h` is chosen from it.

## 4. Booting a disc

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 4.1 | An original disc, Boot disc -> Boot the disc | A confirmation prompt | - |
| 4.2 | Confirm | "Loading...", then the game starts | See below |
| 4.3 | Try a game whose executable is over ~1.7 MB | A "Cannot load" screen with numbers | Correct behaviour, not a failure |

4.2 is the least tested code in the project. If it hangs, the last log line
says which half got there:

- No `handoff:` line -> it died in the two reads, so the CD driver.
- A `handoff:` line and then nothing -> the relocated stub, the cache flush or
  the jump. Note the staged and direct sizes from that line.

## 5. PCDRV

Needs section 1 for the trap, but the client half does not.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 5.1 | `python -m tinrom_host pcdrv <port> <dir>` on the PC, with a `TINROM.TXT` in that directory | - | - |
| 5.2 | Host filesystem -> Check the host | "Host is serving", with a byte count | The framing, or the serial console is eating the reply |
| 5.3 | Stop the host, Check the host again | "No host", after a pause | A hang means the read timeout is not bounding the wait |
| 5.4 | Host filesystem -> Arm the trap | "Trap armed" | - |
| 5.5 | Upload a homebrew PS-EXE that uses PCDRV, run it | It reads its files | The break dispatch, or the A1/A2/A3 argument order |

5.2 and 5.5 test different things: the first is tinrom calling out, the second
is a program's break being trapped.

## 6. Memory cards

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6.1 | One card in slot 1, none in slot 2. Memory cards | Slot 1 shows saves, slot 2 says "no card" | If the *wrong* pane fills, the slot select is wrong again |
| 6.2 | Look at a save's cell | An icon, animating if it has frames | Blank cells mean the upload or the page/`u` split; garbage means the same |
| 6.3 | Move the cursor onto a save | Its title in readable ASCII | Wrong characters mean the Shift-JIS table |
| 6.4 | A save that spans two blocks | Two cells joined into one bar | The block-chain walk |
| 6.5 | Pull the card out while the grid is up | Within about a second it says "no card" | The poll |
| 6.6 | Put it back | It reloads | The debounce may be eating the change |
| 6.7 | A card you believe is counterfeit | "capacity suspect" in the card's line | Only catches clones that alias at the top of the range |

## 6b. Writing to a card

**Use a card you do not care about.** This is the first code in tinrom that
writes to one, and none of it has run on hardware. Back the card up first if
that is not true.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6b.1 | Pick a save, Delete, confirm | The cell turns amber and its icon goes half-transparent | If the icon vanishes instead of fading, the veil is opaque - check the blend mode word. If the screen is unchanged, the write was refused |
| 6b.2 | Leave the menu and come back | Still shown as deleted | The directory was not really written; the first screen was reading its own memory |
| 6b.3 | Put the card in a PlayStation's own card screen | The save is gone from it | The states were written but not the ones the BIOS looks for |
| 6b.4 | Back in tinrom, Undelete the same save | It returns, fully opaque, with its title | - |
| 6b.5 | A save spanning two blocks: delete it | **Both** cells fade, joined | Only the first fading means the `deleted` flag is not reaching continuation cells |
| 6b.6 | Two cards in. Copy a save to the other | A progress bar, then the save appears on the other card | - |
| 6b.7 | Copy the same save again | "Already there", naming the file | The duplicate-filename check |
| 6b.8 | Delete a save on card 2, then copy the same-named save from card 1 | It copies, and is in use | A deleted file of the same name must not block a copy |
| 6b.9 | Copy a **deleted** save to the other card | It arrives fully opaque and in use | The destination states were copied rather than rebuilt |
| 6b.10 | Load the copied save in its game | It loads | The data frames or the size field |
| 6b.11 | Fill a card, then try to copy | "Not enough room", with the block counts | - |
| 6b.12 | Pull the card out mid-copy | An error, and the menu comes back | A hang means an /ACK timeout is missing on the write path |

## 6c. The file browser

Section 4 covers booting what SYSTEM.CNF names; this covers launching anything
else, and the two now share one loader, so **a failure here is a failure
there**.

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6c.1 | An original disc in. File browser -> Browse the disc | The root listing, directories marked with `/`, sizes beside files | Nothing listed means the mount; a wrong disc label means the PVD read |
| 6c.2 | Descend into a directory, then back out with Circle | You land back on the directory you came out of, not the top | The per-depth cursor |
| 6c.3 | Select the game's main executable | The popup offers Launch, enabled | Launch greyed out means the PS-EXE sniff read the wrong sector |
| 6c.4 | Select a data file (a .STR, say) | Launch greyed out, "Not a PS-EXE" | It is guessing rather than reading the header |
| 6c.5 | Details on the executable | Entry point and load range, matching what cdboot's own screen reports | - |
| 6c.6 | Launch it | The game starts, exactly as Boot disc does | **Section 4 re-applies here.** This is the shared loader |
| 6c.7 | With the host tool running, Copy to host on a small file | A progress bar, then a byte count, and the file appears on the PC | A stall means PCDRV write; a short file means the partial last sector |
| 6c.8 | Compare that file against the one on the disc image | Byte identical | The last-sector handling is the likely culprit |
| 6c.9 | With no host tool running, select any file | Copy to host greyed out, "No host is listening" | The presence check |
| 6c.10 | File browser -> Browse memory card 1 | Saves listed with block counts, deleted ones marked | - |
| 6c.11 | Copy a save to the host | A file of blocks x 8192 bytes appears | The chain walk, if the size is wrong |
| 6c.12 | A disc with a directory of more than 128 entries | "Long directory", then the first 128 | - |

## 6d. After moving to ps1-bare-metal 0ef58212ab3a

Two separate changes, applied as separate commits so a failure can be pinned
to one of them.

**The migration** renamed 54 register and flag uses in `sio0.c`,
`uart_sio.c` and `dbgcon.c`. Every old name was checked against its
replacement and all 28 have identical values and addresses, so nothing should
change. Section 6 (memory cards, pads) and the serial log are the check.

**Leaning on upstream** does change behaviour, in three places:

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6d.1 | Boot. Watch the log | The usual `cd: <version>, <region>` line | The CD reset now writes `BIU_DEV5_DELAY`, masks `HINTMSK` to 0 and clears the buffer flags. No such line means the drive stopped answering - suspect `HINTMSK` first |
| 6d.2 | Sections 2 and 4, in full | As before | `interruptFlags()` now waits for two identical `HINTSTS` reads. A hang here would mean it never settles, which should be impossible - report it |
| 6d.3 | Section 6c.6 (launch from the file browser) and 6c.7 (copy to host) | As before | The long multi-sector reads are where a mis-read interrupt type would show |
| 6d.4 | Section 1 in full, especially 1.4 and 1.6 | As before | The exception handler's cache flush is now ps1-bare-metal's instead of the BIOS's. A lock-up on arming or disarming points there |
| 6d.5 | Repeat 6d.1-6d.4 on an early console (SCPH-1000 to -5000) if you have one | As before | The `HINTSTS` debounce exists for those models specifically - the mechacon and decoder run on different clocks there |

## 6e. The debugger

Needs a serial cable and GDB for MIPS (on Windows, the prebuilt
`gdb-multiarch` from the PCSX-Redux project works), plus any small homebrew
ELF linked at 80010000h. Use the rate the Debugger status screen shows.
Background is in [debugger.md](debugger.md).

**GDB directly**, no nops running:

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6e.1 | GDB -> Status | "monitor 801e8000", handler "not installed" | "No debugger: ..." says why, and the log has the same `dbg:` line |
| 6e.2 | GDB -> Wait for a debugger | The waiting screen, still animating; Circle leaves it | - |
| 6e.3 | Wait again. On the PC: `gdb prog.elf`, `set serial baud 115200`, `target remote COM3` | GDB reports a stop with `pc` inside the monitor (801exxxx); the console screen freezes | Nothing at all: the port or rate. "Remote replied unexpectedly" or a target description error: send the GDB output |
| 6e.4 | `info registers` | Plausible values: `sp` near 801fxxxx, `pc` in the monitor | All zero: the frame was not published |
| 6e.5 | `x/4x 0x80000000`, then `x/4x 0xfffe0000` | Four words, then "Cannot access memory" | A console lock-up on the second: the address was not refused |
| 6e.6 | `load` | Sections listed with sizes, no error | A write error near 801e8000: the ELF is too big for the monitor's window |
| 6e.7 | `break main`, `continue` | Stops at `main`, source line shown | Runs away: breakpoints are not reaching memory, or the cache flush is missing |
| 6e.8 | `stepi` x5, then `next` x3 | Each moves forward one instruction or line | Stuck on one address: step-over of a breakpoint is broken |
| 6e.9 | `continue` into a deliberate crash (e.g. a store to `*(int*)1`) | `Program received signal SIGSEGV` at the store | The console just hangs: the fault path is not reaching the stop loop |
| 6e.10 | `detach` | GDB detaches. If `pc` was still in tinrom, tinrom's menu comes back | - |

**Through nops**:

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6e.11 | GDB -> Wait for a debugger, then `nops /gdb 127.0.0.1:3333 /dest COM3` | nops: "GDB server initialised"; the console freezes | nops sits after "Opening a listen server": `DEBG` got no answer |
| 6e.12 | `gdb prog.elf`, `target remote localhost:3333`, `load`, `tbreak main`, `continue` | Stops at `main` | nops prints "PSX may have halted" and GDB never stops: the stop's `HLTD` or the bracket handling |
| 6e.13 | `stepi` a few times, `info registers`, `continue` | As 6e.8 | - |
| 6e.14 | Stop nops, then `nops /regs` and `nops /cont` | Registers print; the console carries on | - |

**Without a debugger**:

| # | Do | Expect | A failure means |
| --- | --- | --- | --- |
| 6e.15 | `python -m tinrom_host halt COM3`, then `regs`, then `cont` | "halted", a register table, "resumed"; the menu freezes and comes back | - |
| 6e.16 | `python -m tinrom_host dump COM3 0x80000000 0x10000 ram.bin` | 65536 bytes written | "checksum mismatch" or a short read: `DUMP`'s `MORE` blocks |
| 6e.17 | Launch a game (sections 4 or 6c) after any of the above | It boots as before | A crash right at launch: the handler was not removed before the load overwrote the monitor |

## 7. Regressions

Things that worked before and should still work.

- [ ] The cart ROM boots with no disc in the drive (the mid-boot COP0 hook).
- [ ] The spinning model is visible on the menu and not inside-out.
- [ ] The model no longer draws over parts of itself as it turns
      (see docs/model-sorting.md; some of it is expected to remain).
- [ ] Credits and licences opens without locking up.
- [ ] The PS-EXE build still runs over serial.

## 8. The cart switch

On a cart with a switch, or pcsx-redux with its PIO cart switch.

| # | Do | Expect | If not |
| --- | --- | --- | --- |
| 8.1 | Settings -> Cart switch -> Read it: Xplorer-style (pcsx-redux) or Action Replay-style | "It reads ON/OFF now", matching the switch | The wrong kind of cart, or no readable switch |
| 8.2 | Flip it, Test the switch, flip it back, test again | The reading follows | As 8.1 |
| 8.3 | Set up one position (say Developer, GDB autobooted) and the other (Basic), Save | - | - |
| 8.4 | Power-cycle in each position | Each comes up with its own level, modules and autoboot; the menu title says "switch on"/"switch off" | Log line `switch: on (...)` missing: the source was not saved |
| 8.5 | Swap when flipped: in the menu; flip it on the main menu | The menu changes within half a second | - |
| 8.6 | Hold Select and Start at power-on in the autoboot position | The main menu, not the autoboot | - |

---

## Reporting a failure

The useful things, in order: the **last log line** before it stopped, the
**test number**, and the **console model and drive region string** from 2.2.
For a lock-up, whether the emulator console printed anything the serial port
did not - that difference says whether the serial writer was the thing that
hung.
