# Booting a disc

> **Since this was written:** the load-and-hand-over sequence described below
> now lives in `ui/launch.c`, shared with the file browser, which launches
> arbitrary executables through the same code. The behaviour is unchanged
> except that the confirmation names what is being launched rather than saying
> "the disc". See docs/file-browser.md.

Three separable problems. Each is its own file, and they fail independently.

| Step | Where | State |
| --- | --- | --- |
| Make the drive read the disc | `src/hal/cdrom.c` | written, untested on hardware |
| Find the executable | `src/core/iso9660.c`, `src/core/psexe.c` | written, tested on a host |
| Hand the console over | `src/hal/handoff.c`, `handoff.S` | written, untestable from here |

## The unlock

A retail drive refuses any disc whose `SCEx` string does not match the
console's region. That is every CD-R, every homebrew burn, and every import.

The drive's sub-CPU accepts a documented sequence that turns the check off:
commands `50h` through `56h`, carrying between them the licence sentence it
compares against, with the last word depending on the console. psx-spx
documents it in full under "CDROM - Secret Unlock Commands"; it has been
public since 2013. tinrom sends documented commands to documented ports and
copies no code from anywhere.

The region word comes from the console's own answer to test command `19h,22h`:

| Drive answers | Console | Unlock word |
| --- | --- | --- |
| `"for U/C"` | NTSC, North America | `"of America"` |
| `"for Europe"` | PAL, Europe | `"(Europe)"` |
| `"for NETNA"` / `"for US/AEP"` | Yaroze, debug | `"World wide"` |
| `"for Japan"` | NTSC, Japan / Asia | `"Inc."`, but see below |

Two limits, both documented and both reported honestly by the module rather
than papered over:

- **Japanese and Asian consoles cannot be unlocked this way.** They contain the
  code to process the commands but do not execute it, and would ignore the
  resulting flag if they did.
- **Drive firmware older than vC1 has no such commands.** It answers `INT5` to
  the region query, which is how `tinromCdDriveInfo` spots it.

On those consoles the door-swap sequence is the only route, and that is not
implemented.

**Every one of the seven commands answers `INT5` whether or not it worked**, so
there is nothing useful to check as they go. `tinromCdUnlock` judges success
the only honest way available: it reads a sector afterwards.

The unlock does not survive a `Reset` (`1Ch`), and `GetID` still reports the
disc as unlicensed afterwards - that is the drive telling the truth about the
disc, not the unlock having failed. Opening and closing the lid does not undo
it.

The licence strings are interface constants in the sense
[attribution.md](attribution.md) already defines: values a chip compares
against, like a file format's magic number. They are documented, they are
emitted rather than embedded as data, and `NOTICE.md` says so.

## Finding the executable

`core/iso9660.c` reads through a caller-supplied function rather than calling
the drive itself, which keeps the format code out of the HAL and means the
whole walker can be run on a host against a disc image. `tools/test_cdboot.c`
does exactly that: mount, path lookup in every spelling a PS1 disc uses
(`cdrom:\NAME;1`, `\NAME`, `/name`), directories past the first sector,
`SYSTEM.CNF` parsing, `PS-EXE` header validation, and the rejection cases.

A disc with no `SYSTEM.CNF` falls back to `\PSX.EXE`, which is what the
console's own shell does. One whose `SYSTEM.CNF` is there but will not read
says so, rather than going looking for a `PSX.EXE` the disc never named.

Every read ends with Pause, and waits for Pause's second answer before it
returns. The drive goes on reading until then, a sector or several later,
and a sector landing after the read had returned used to be taken by the
next command as its own answer. The boot path reads the volume descriptor,
the root directory and `SYSTEM.CNF` back to back, so it was the one that
failed: an I/O error on the second read, on a disc the file browser read
without trouble. `tools/test_cdaudio` now models the drive's timing - a
sector every 1/300 s at double speed, and a pause that takes 35 or 70 ms -
and checks that nothing from one read is still coming when the next begins.

## Handing over

A game's executable loads to `80010000h`. So does tinrom. The final copy
therefore cannot run from the memory it is overwriting, and that constraint
admits only three answers:

1. **Do not be there.** Link the loader somewhere a game will not reach, or
   run it from the cart's ROM with only a small RAM footprint. This is what a
   small save-game exploit payload does, and what a cart firmware gets for
   free.
2. **Get out of the way first.** Relocate the loader - CD driver included - to
   high RAM, then read sectors straight to their destination from there.
3. **Only move what actually collides.** Read the part of the payload that
   lands on the loader somewhere else, read the rest straight to where it
   belongs, and copy the collision into place at the very last moment from a
   stub that is itself out of the way.

tinrom does (3). It needs no linker changes and no relocatable driver, and
because only the collision is buffered, the staging area has to be as big as
**tinrom**, not as big as the game:

```
 destination        protected end                    staging       stub
 |                  |                                |             |
 +-- staged prefix -+------ read straight here ------+-- prefix ---+
 80010000           80041000                              801bf000  801f0000
```

A 282 KB game at `80010000h` with tinrom ending at `80041000h` splits into
196 KB parked at `801bf000h` and 86 KB read directly to `80041000h`. Only the
196 KB is copied at handoff.

The split is on a sector boundary, which is what makes it two reads rather
than a read plus a memcpy.

Then:

1. A small position-independent stub is copied to the **top of RAM**
   (`801f0000h`) and entered through its **uncached mirror** at `a01f0000h`,
   so its own instruction fetches cannot come from cache lines that are about
   to be invalidated underneath it.
2. It copies the staged prefix into place, calls the kernel's `FlushCache`,
   sets `gp`/`sp` and jumps.

`FlushCache` is a `TINROM_KERNEL_CALL`, and it is called with interrupts
already disabled. psx-spx documents a bug in it - it uses `k0` without masking
interrupts and loops forever if one lands - and disabling them first is the
documented workaround. Nothing in the handoff needs an interrupt, so we can.

The scratchpad at `1f800000h` would have been the obvious home for the stub.
It is not executable: psx-spx is explicit that an instruction fetch there
raises a bus error.

### What is still refused

A payload big enough to reach the staging area - about 1.7 MB from
`80010000h` - has nowhere to park its collision, and `tinromHandoffPlan`
refuses with the numbers rather than locking up. Answer (2) above is the fix
for those, and it is the remaining work: it needs the CD driver to be callable
from a relocated copy of itself, which is a bigger change than it sounds.

`scripts/test-handoff.sh` runs the planner against a fake linker layout and
checks the split for a spread of sizes, including the boundary cases.

## Attribution

Written from psx-spx, which documents every command, register and format used
here. No code is taken from tonyhax, UniROM or any other implementation.
`NOTICE.md` records the specification as documented knowledge.

An earlier revision of this file claimed tonyhax's licence made borrowing from
it impossible. That was wrong: tonyhax is WTFPL, which is permissive. Nothing
has been borrowed from it, but that is now a choice rather than a constraint -
and its relocating loader is the obvious prior art for the one problem left
open above.
