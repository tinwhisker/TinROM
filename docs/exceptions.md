# The exception handler

`src/monitor/entry.S` and `src/monitor/core.c`, with tinrom's side in
`src/hal/exception.c`. Two features were blocked on this one thing: trapping
a running program's PCDRV breaks, and the debugger. Both now work.

Games get something else: a crash screen in the cart, which needs no handler
of tinrom's at all. See [The crash screen games get](#the-crash-screen-games-get)
below.

The handler lives in the **resident monitor**, a separately linked blob that
tinrom copies to `801E8000h` at boot, rather than in tinrom itself. A debugger
has to be able to load a program over tinrom and keep catching its
exceptions, and a handler inside tinrom would be overwritten by that program.
[debugger.md](debugger.md) covers the monitor.

## Taking the vector

The console vectors everything except the COP0 hardware breakpoint to
`80000080h` (psx-spx, "Exception Vectors"; the PSX only ever uses the BEV=0
set, and the BIOS ROM does not even contain the BEV=1 ones). The kernel puts
its own handler there.

tinrom overwrites four words with a trampoline - `lui k0` / `ori k0` / `jr k0`
/ `nop` - and keeps the originals so the vector can be handed back. `k0` and
`k1` are the two registers the architecture reserves for this, which is why
the trampoline can use one before anything has been saved.

`ori` rather than `addiu` on purpose: `ori` is zero-extended, so there is no
sign-extension adjustment to the upper half to get wrong.

After writing the vector, flush the instruction cache. psx-spx says changes
at `800000xxh` *seem* to reach it without a flush and recommends flushing
anyway - a vector is not the place to rely on a "seems".

The flush is ps1-bare-metal's `flushCache()` (`common/cache.s`). It used to be
the BIOS's `A(44h) FlushCache`, a `TINROM_KERNEL_CALL` that had to be wrapped in
an interrupt mask to dodge a known bug in it. Upstream's clears only the cache
tags instead of zeroing every line, runs from RAM instead of across the 8-bit
ROM bus, masks interrupts itself, and removes a kernel dependency.

## It does not chain

Decoding the kernel's trampoline and re-entering it correctly is a good way to
lose a console, so tinrom does not try. While installed, tinrom is the only
handler.

What that costs: a program's own exception handling will not run, and any
exception tinrom cannot service stops the console with a report instead of
reaching the kernel. What it buys: no guessing about someone else's code.

`tinromHalShutdown()` removes it, so the vector is always given back before a
handoff. The launcher removes it even earlier, before the first byte of a
payload is read. The monitor sits where large payloads and the launcher's
staging area land, so it has to be out of the way before it is overwritten.
It is also *forgotten* (`tinromDebugRelease()`): once game code is sitting in
the monitor's window, its header and state pointers are that code's bytes, and
following them reads from wherever they happen to point. An emulator stops on
that ("32-bit read from unknown address"); a console takes a bus error.
tinrom keeps its own copy of the kernel's four words for the same reason: it
can put them back even after the monitor itself has been overwritten
(`tinromDebugRemoveHandler()`). A trampoline left pointing into memory that a loaded game has since
overwritten is the worst kind of crash, because it looks like the game's
fault.

## The frame

Registers are saved at their own register number - `r4` at word 4 - because a
GDB stub wants them in that order anyway, and it makes the assembly a straight
run of stores with no table to drift out of step. The offsets live in
`hal/exception.h`, which `entry.S` includes, and a `_Static_assert` fails the
build if the struct and the offsets ever disagree.

The handler switches to its own stack before calling any C. The faulting
stack is often the reason for the exception.

It does not set `gp`. The monitor is built with `-G0` and never addresses
through it, so the dispatcher runs correctly with whatever `gp` the stopped
program had. The one call back into tinrom, PCDRV, swaps tinrom's own `gp` in
around the call.

## Coming back

`rfe` in the delay slot of the final `jr` pops the interrupt-enable and
kernel/user stack that entering the exception pushed. The resume address comes
out of the frame, so the dispatcher can change it.

Stepping over a serviced instruction has a case worth getting right rather
than refusing:

- **Normal:** `EPC + 4`.
- **In a branch delay slot** (`CAUSE.BD`): `EPC` points at the *branch*, and
  the faulting instruction is at `EPC + 4`. By the time a delay slot runs the
  branch has already been decided, so resuming means going where the branch
  was going: `TAR` if `CAUSE.BT` says it was taken, otherwise `EPC + 8`. COP0's
  `TAR` register exists for exactly this.

## What it services

`break` instructions with codes `101h`..`107h` - the PCDRV calls - are
dispatched into `tinromPcdrvCall()`, with `A1`/`A2`/`A3` read out of the frame
and `V0`/`V1` written back into it. Restoring from the frame on the way out is
how the results reach the program. This happens only while tinrom is intact.
Once a debugger has loaded something over it, a PCDRV break stops in the
debugger instead.

`syscall` with `A0` = 1 or 2 (`EnterCriticalSection`, `ExitCriticalSection`)
returns as though it worked. Interrupts are masked anyway, and most SDKs make
these calls on the way in.

Breakpoints, steps and hooks the debugger set are the monitor's own; see
[debugger.md](debugger.md).

Everything else, including breaks with codes tinrom does not know, is treated
as a fault: recorded, reported, and then the console **halts in the
debugger**. (While a debugger has armed the monitor, a `break` it does not
know stops as `SIGTRAP` instead; see [debugger.md](debugger.md).) That looks exactly like the old stop, except that the serial port
still answers, so a debugger can attach and look, and `nops /regs` works.
`nops /cont` or a debugger resumes it; resuming without changing anything
faults again on the same instruction. Stepping over a trap that someone meant
to be fatal is worse than stopping.

A one-line notice goes to the emulator console first, because that is a plain
store that cannot block. The full report then goes to the serial port, whose
writer spins waiting for the transmitter - and a crash is exactly when the
port might be in no state to drain.

## Load delay slots

The R3000 has no interlocks, and everything in this file is inside
`.set noreorder` because the branch delay slots have to be ours. That means
nothing fills the *load* delay slots either, and nothing warns. psx-spx:

> The loaded data is NOT available to the next opcode, ie. the target register
> isn't updated until the next opcode has completed.

and, for coprocessor reads:

> When reading from a coprocessor register, the next opcode cannot use the
> destination register as operand.

The first version of this handler had five of them, reading COP0 and storing
the destination on the very next instruction. It did not crash. Every value
landed one field late - SR into `cause`, `cause` into `epc`, `epc` into
`badVaddr` - so the handler ran perfectly and decided everything from the
wrong register. A real breakpoint was reported as an interrupt at address
`24h`.

That is the shape of this bug class: not a fault, but plausible-looking
nonsense. It is worth recognising in a crash report - if `SR` has bit 30 set
and nothing else, you are probably looking at a shifted frame, because that is
`CU2` and nothing else.

`scripts/check-hazards.py` walks the `.S` files and flags any load or
coprocessor read whose destination is read one opcode later. It runs as part
of `scripts/syntax-check.sh`. Writing it immediately found two more in the
restore path - `lw` into `$t0` followed by `mthi $t0` - which would have
restored garbage into `HI`/`LO` and corrupted any multiply or divide
interrupted by an exception.

## Testing it

Host filesystem -> Test the trap is a break test. It is not a demonstration
of catching a crash - a crash correctly stops the console - but of the one
exception that comes back: it issues `break 0x101` with no host listening, so
the handler traps it, PCDRV times out, and execution resumes. Reading the
result screen at all is the test passing.

The monitor's logic is also tested on the host (`scripts/test-monitor.sh`).
The same sources compile for x86 with the serial port and memory faked, and
scripted GDB and nops sessions are run against them. That covers breakpoints,
stepping over branches, stepping off a breakpoint, nops' halt bracket,
`DUMP`'s `MORE` blocks, faults, PCDRV and the refused ranges.

## The crash screen games get

Everything above is tinrom's handler, and it is gone before a game starts:
it does not chain to the kernel's, so a game's interrupts and events would
stop working under it. A game that crashes therefore meets the kernel's own
handling, and for a crash that is a hang. The kernel saves the registers in
the current thread's control block and runs its handlers. If none claims
the exception, it calls `DeliverEvent(F0000010h, 1000h)` and then
`A(40h) SystemErrorUnresolvedException`, which loops forever (psx-spx,
"BIOS Function Summary" and "Unresolved Exception Events"). That covers
`break` - GCC's division checks among them - and bus errors, address
errors, reserved instructions and overflow.

So tinrom changes nothing in that path except its last step. Before any
handoff, `tinromHalShutdown()` writes one word: entry 40h of the kernel's
A(nnh) jump table at `200h`, pointed at code in the cart. The kernel then
runs the game exactly as it would without tinrom, and only an exception
nobody handled - one that would have frozen the console - reaches the cart.

The code it reaches is `src/crash`, linked on its own to run straight out
of the cart's ROM: a loaded game can overwrite all of RAM, but not that.
`tools/makerom.py` puts it at `+400h` in the image, after the stub, and the
descriptor's `crashOffset` says where it is. tinrom checks its header before
pointing anything at it, so a cart without one, an older tinrom cart or some
other cart entirely is left alone. Whether this tinrom came from the cart, a
disc or a memory card makes no difference: what counts is the cart in the
slot when the game starts. `TINROM_ROM_CRASH_SCREEN` (on by default) builds
it into the image; it is about 5K of ROM and no RAM.

The screen trusts nothing the game left:

- It runs uncached, from the cart, with interrupts off and the data cache
  not isolated, on a stack in the scratchpad. It has no writable data at
  all; the link fails if any turns up.
- It stops the GPU's DMA and resets the GPU, then sets up a 320x240 picture
  in the game's own video standard and uploads its own font. The font is
  the terminal's 4x8 one, printable ASCII only, drawn twice as wide.
- It silences the SPU and CD audio, which a crash tends to leave droning.
- It reads memory only where memory is sure to be: main RAM in any of its
  mirrors, and the BIOS ROM. A second exception in here would be a hang with
  a blank screen, which is the thing it exists to replace.

It shows the exception, `pc`, `badv`, `cause` and `sr` from COP0, every
register the kernel saved (all but `r0`, `k0` and `k1`) with `hi` and `lo`,
the words around `pc`, and eight words of stack. A `break` is decoded:
`break 7` is shown as division by zero and `break 6` as division overflow.

Reset the console to carry on. The console is not left in a state worth
returning to a game from.

`scripts/test-crash.sh` draws it on the host over a software GPU and a
made-up RAM, crashes four imaginary programs, and reads each screen back
against the font. With a directory it writes the screens as PNGs.

### What is not proven yet

That the kernel calls A(40h) through its table rather than directly.
psx-spx names the call `SystemError_A_40h` and every sign points to the
table, but only a console can settle it. GDB -> Crash on purpose tests it. It points A(40h) at the cart, gives the kernel back its exception
vector, and makes an unaligned load from `0BADC0DEh`. The crash screen
should appear, saying "Address error on a load" with `badv 0badc0de`. A
freeze with tinrom's menu still on screen means the kernel went straight to
its own hang. If that happens, the fallback is an event:
`OpenEvent(F0000010h, 1000h, ...)` with a callback is called just before
A(40h). It is not done now because it costs one of the kernel's 16 event
slots for every game, crash or not.
