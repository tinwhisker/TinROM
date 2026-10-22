# The debugger

tinrom can be debugged with GDB over the serial port, either directly or
through `nops /gdb`. Both talk to the same thing: a small **resident monitor**
that owns the exception handler, the breakpoints and the stop loop.

## Connecting

Pick **GDB → Wait for a debugger** on the console first. tinrom keeps its menu
running, and halts the moment a debugger makes contact. Circle stops waiting.

**GDB straight at the serial port** (no nops):

```
(gdb) set serial baud 115200
(gdb) target remote COM3            # or /dev/ttyUSB0
```

Use the rate the console shows on the status screen. The console speaks GDB's
remote protocol itself: GDB's `load`, `break`, `stepi`, `continue`,
`info registers`, `x`, `set var` and `detach` all work.

**Through nops** (the UniROM workflow):

```
nops /gdb 127.0.0.1:3333 /dest COM3
(gdb) target remote localhost:3333
```

nops is the GDB server in this setup; it turns GDB's requests into nops serial
commands, and the console answers those. What works is what nops implements.
Two nops limitations are worth knowing:

- nops does not implement register writes for `r1`..`r15` (its `P` parser
  wants a two-digit register number). `set $pc` works; `set $a0` does not.
- `detach` in GDB does not resume the console through nops. `nops /cont`
  does.

## Loading a program

`load` in GDB writes the ELF's sections straight into RAM, sets `pc` and
waits for `continue`. That program lands at 80010000h, on top of tinrom. This
is why the debugger is not part of tinrom:

| Range | What |
| --- | --- |
| `80010000h`–`~80040000h` | tinrom. A loaded program overwrites it; that is fine. |
| `801E8000h`–`801EFFFFh` | **the resident monitor**. Writes here are refused. |
| `801F0000h`– | the launcher's handoff stub, then stacks. |

The monitor is linked on its own (`src/monitor/monitor.ld`), embedded in
tinrom as a blob, and copied into place at boot (`hal/debugger.c`). Because it
is a separate link, it cannot call into tinrom by accident. Such a call is an
undefined symbol at build time, not a crash after `load`.

A program bigger than about 1.9 MB would reach the monitor. Its writes there
are refused rather than letting the debugger vanish mid-session. Directly,
GDB's `load` then fails with an error. Through nops the bytes are dropped,
because nops reports every write as a success.

## What stops the program

- A breakpoint GDB set (`break`, `tbreak`, `hbreak`). Directly, these arrive
  as `Z0` and the monitor writes and removes the break instructions. Through
  nops, GDB writes them with `M`.
- A single step. The monitor puts a temporary break on every place the next
  instruction can lead: both sides of a conditional branch, and the register
  target of `jr`/`jalr`. A branch and its delay slot are stepped as one,
  because the CPU cannot split them.
- Any other `break`, such as a compiler trap or an `assert`: `SIGTRAP`.
- A fault: address error, bus error, reserved instruction, overflow. This
  reports `SIGSEGV`, `SIGBUS`, `SIGILL` or `SIGFPE`.
- **Not** Ctrl-C while a program runs. Serial is polled, so a running program
  cannot be interrupted from the host. Ctrl-C works while tinrom's own menu
  runs, because the menu polls the port.

A breakpoint that cannot be written, in the BIOS ROM say, is refused when it
is set. GDB then says so, instead of the program running past it. Stepping
into ROM code (the second half of a BIOS call) is refused the same way; use
`finish` or a breakpoint after the call.

Breakpoints are only in memory while the target runs. They come out the
moment it stops, so GDB reading code sees the real instructions. They go back
in on resume, all except one at the resume address, which is stepped over
first.

## What the handler does for a running program

The monitor does not chain to the kernel's handler (see
[exceptions.md](exceptions.md)), so it covers the few things programs
commonly need:

- **Interrupts** are masked and the program resumed, exactly as before.
  Programs that depend on interrupts (VSync callbacks, most games) do not run
  properly under the debugger.
- **`syscall` 1 and 2** (`EnterCriticalSection`/`ExitCriticalSection`)
  return as if they had worked. With interrupts masked there is nothing to
  enter, and most SDKs call them on the way in. Any other syscall stops as
  `SIGSYS`.
- **An exception inside the handler** can happen, for example when tinrom's
  PCDRV code is given a bad pointer by the program. It does not overwrite the
  program's frame. The debugger stops on the program, at the instruction that
  raised the first exception, and the inner one is reported and abandoned.
- **PCDRV breaks** are serviced by tinrom's file server, but only while
  tinrom is intact. Once something has been written over tinrom's image, a
  PCDRV break stops in the debugger instead of jumping into whatever now
  occupies tinrom's old address.

## Registers

GDB gets 72 registers: `r0`–`r31`, `status`, `lo`, `hi`, `badvaddr`, `cause`,
`pc`, then the FPU. The PS1 has no FPU, but GDB's MIPS support will not
accept a target without one, so those registers read as zero. The monitor
sends this layout as a target description (`qXfer:features:read`, with
architecture `mips:3000`). `pc` is the resume address. After a stop in a
branch delay slot, it is the branch, which is where the CPU restarts.

nops reads registers a different way. It follows the kernel's pointer at
80000110h to the first thread control block and reads 0C0h bytes there. So
whenever the target is stopped, the monitor publishes the frame into that
block, and reads edits back out of it on resume:

| Word | Contents |
| --- | --- |
| 0 | SR (the kernel's status word is saved and put back) |
| 1 | BadVaddr (the kernel's word is saved and put back) |
| 2–33 | r0–r31 |
| 34 | resume address |
| 35, 36 | hi, lo |
| 37, 38 | SR, cause |
| 39 | non-zero if stopped in a delay slot (word 34 is then the branch) |

If the pointer is not a plausible kernel address (on the cart's pre-boot
path the kernel is not up yet), the monitor points it at a block of its own,
and puts the old value back when the handler is removed.

## One port, two protocols

The serial console (`siocons`) keeps SIO1. A GDB packet always starts with
`$`, and no nops command does, so the first byte is enough to tell them
apart:

- `$` or Ctrl-C: GDB. tinrom halts in the monitor, which reads the rest.
- `+` / `-`: GDB acknowledgements. They are ignored.
- Anything else is a four-byte nops token. The debug commands, the memory
  commands and the rate tokens go to the monitor, which answers them the same
  way whether tinrom is running or halted. The rest are handled by tinrom
  itself. See [nops-protocol.md](nops-protocol.md).

While halted, the monitor runs the same demultiplexing with its own serial
driver. It never calls tinrom's.

### nops' halt bracket

When a resumed target stops, the monitor sends an unsolicited `HLTD`. nops
then reads the registers, and because it still thinks the target is running
it brackets the read with `HALT` … `CONT`. Taken literally, that `CONT` would
set the target going again straight after reporting the stop. The monitor
therefore remembers the `HALT` that answers its own `HLTD`, and acknowledges
and ignores the `CONT` that closes the bracket. A side effect: typing
`nops /halt` then `nops /cont` straight after an announced stop takes two
`/cont`s.

The unsolicited `HLTD` also goes out when nops was the last host to talk to
the console but did not resume it itself. A hook set with `nops /hookex`
while tinrom's menu runs is the usual case, and the `HLTD` is how nops reports
that the hook was hit. Every other `HALT` is
answered with `HLTD` even when already halted, because nops waits for it
forever otherwise.

## Limits

- Read/write watchpoints (`nops /hookread`, `/hookwrite`, GDB `watch`) are
  not done. They need the COP0 data breakpoint, which vectors to 80000040h,
  and the cart's mid-boot takeover already uses that vector. `HKRD`/`HKWR`
  answer `UNSP`. GDB falls back to software watchpoints, which single-step
  and are very slow.
- No Ctrl-C into a running program (above).
- Programs that rely on the kernel's exception handling (interrupt handlers,
  events, threads) do not run properly while the monitor owns the vector.
- The monitor is removed before tinrom launches a game or an executable,
  exactly as the exception handler always was. A game that crashes after
  that gets the cart's crash screen instead (see
  [exceptions.md](exceptions.md#the-crash-screen-games-get)).
- A build can leave the monitor out altogether: `TINROM_DEBUGGER` for the
  main payload and `TINROM_MCR_DEBUGGER` for the memory card image, which
  leaves it out by default for space (see [memcard-boot.md](memcard-boot.md)).

## Files

| File | What |
| --- | --- |
| `src/monitor/monitor_api.h` | the contract with tinrom: header, state, constants |
| `src/monitor/entry.S` | the exception handler, and guarded memory access |
| `src/monitor/core.c` | dispatcher, stop loop, breakpoints, stepping, the thread-block view |
| `src/monitor/rsp.c` | GDB's remote protocol |
| `src/monitor/nops.c` | the nops debug commands, `DUMP` and `SBIN` |
| `src/monitor/sio.c`, `util.c` | the monitor's own serial driver, hex, `memcpy` |
| `cmake/monitor.cmake` | builds and embeds it |
| `src/hal/debugger.c` | loads it; tinrom's side of the API |
| `src/modules/gdb/gdb.c` | the screens |

Memory the debugger asks for goes through `monitorPeek`/`monitorPoke` in
`entry.S`. They set a guard word first, and if the access faults, the handler
resumes at the accessor's recovery label instead of reporting a crash. So
`x/4x 0xfffe0000` in GDB gets an error reply, not a dead console. SIO1's own
registers are refused outright, because reading the data register would eat
the host's bytes.
