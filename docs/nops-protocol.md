# The nops wire protocol

tinrom's serial console is wire-compatible with
[NOTPSXSerial](https://github.com/JonathanDotCel/NOTPSXSerial) ("nops"), so
existing cables, scripts and builds work against it, and `tinrom_host` works
against a UniROM console.

**No NOTPSXSerial source is used in tinrom.** NOTPSXSerial is MPL-2.0; tinrom
is MIT. What is reproduced here is a description of the observable protocol:
which bytes go over the wire, in which order. The description was written by
a separate pass that studied nops' behaviour; the implementations in
`src/modules/siocons/nops_server.c`, `src/monitor/nops.c` and
`tools/tinrom_host/` were written from this document, not from nops. See
`NOTICE.md`.

The same port also carries GDB's remote protocol; see
[debugger.md](debugger.md). A GDB packet starts with `$`, and no nops command
does.

## Framing

Everything is little-endian. Commands and replies are four-character ASCII
tokens carried as one 32-bit word, so `"PING"` is `0x474E4950` on the wire.
Arguments follow as bare 32-bit words.

The host sends a command and waits for the reply token. It **then** sends the
arguments. The console acknowledges first, so the host knows it is listening
before it commits to a payload.

The host does not time out waiting for most replies. A console that never
answers leaves nops waiting forever, which is why tinrom answers every command
it recognises, including the ones it cannot do (`UNSP`).

## Commands

| Token | Meaning | Reply | Then the host sends |
| --- | --- | --- | --- |
| `SLOW` `MEDI` `FAST` `WHOA` | switch rate (see below) | *none* | - |
| `PING` | liveness check | `PONG` | - |
| `DEBG` | arm the debugger | `OKAY` | - |
| `HALT` | stop the target | `HLTD` | - |
| `CONT` | resume | `OKAY` | - |
| `HKEX` | execute hook | `OKAY` | address |
| `HKRD` | read hook | (`PONG`) | address |
| `HKWR` | write hook | `OKAY` | address |
| `UNHK` | clear hooks | `OKAY` | - |
| `DUMP` | read memory | `OKAY` | address, length |
| `SBIN` | write memory | `OKAY` | address, length, checksum, data |
| `SEXE` | upload and run a PS-EXE | `OKAY` | header, then body |
| `SROM` | flash a cart image | `OKAY` | address, length, checksum |
| `JUMP` / `CALL` | `jr` / `jal` to an address | `OKAY` | address |
| `REST` | reset the console | `OKAY` | - |
| `MCDN` / `MCUP` | memory card down / up | | slot |
| `STDI` | inject into stdin | `STOK` | string |
| `UPV2` / `UPV3` | protocol revision (see below) | | |
| `BAUD` | tinrom_host's rate change | `OKAY` or `NOPE` | rate token |

A command the console cannot service is answered with `UNSP`. nops shows that
as "Not supported while Unirom is in debug mode!" and gives up at once
instead of waiting.

## Memory transfers

### DUMP

1. Host: `DUMP`. Console: `OKAY`.
2. Host: address, length.
3. Console: the bytes. After every full 2048-byte block, the host sends
   `MORE`, including after the last block when the length is an exact
   multiple of 2048. The console reads it before sending anything else.
4. Console: a 32-bit checksum. nops accepts either the plain sum of the bytes
   or DJB2x (h = 5381, then h = (h·33) XOR byte). tinrom sends the sum.

nops treats anything arriving after the checksum as an error ("Extra bytes
still being sent from the PSX!").

### SBIN, revision 1

1. Host: `SBIN`. Console: `OKAY`.
2. Host: address, length, checksum (the sum of the bytes).
3. Host: the data, in one stream. Nothing else is exchanged and nothing comes
   back.

Revisions 2 and 3 add a per-chunk handshake (`CHEK`, the host's sum,
`MORE` or `ERR!`) and DJB2 checksums. A console offers them by sending
`OKV2`/`OKV3` while the host is waiting for its `OKAY`. The host then sends
`UPV2`/`UPV3` and keeps waiting. nops resets to revision 1 at the start of
every command, so a console that never offers a revision gets revision 1
throughout. **tinrom never offers one.**

`tinrom_host ping` still sends `UPV3` on its own and tinrom answers `OKV3`,
but that is only a version check, and it changes nothing about transfers.

## Rates

On start-up nops sends a bare rate token (`SLOW` by default, `FAST` with
`/fast`) at 115200, waits about 100 ms, and reopens the port at the new rate.
Nothing is sent back. The console switches when it reads the token.

| Token | Console rate |
| --- | --- |
| `SLOW` | 115200 |
| `MEDI` | 414720 |
| `FAST` | 518400 (the PC side uses 510000, which is within tolerance) |
| `WHOA` | 921600 |

`tinrom_host` instead sends `BAUD` followed by the rate token. The console
answers `OKAY` **at the old rate**, then switches, and the host switches after
reading the acknowledgement. An unknown rate gets `NOPE`. nops' own custom-rate
command (`BAUD`, then `BPLS`, then raw rates until `BACP`) is not supported.

## The debugger

`nops /gdb` is a GDB server on the PC that turns GDB's requests into these
commands. `/debug`, `/halt`, `/cont`, `/regs` and `/hookex` are the same
commands used one at a time.

- **`DEBG`** arms the debugger and installs the exception handler. It is
  answered `OKAY` every time, even when already armed: nops sends it at the
  start of every `/gdb` session and waits for the reply.
- **`HALT`** stops the target and is answered `HLTD`. The answer comes
  **whether or not it was already stopped**.
- **`CONT`** is answered `OKAY`, then the target resumes.
- **Registers** are not a command. nops reads the word at `80000110h` (the
  kernel's pointer to its thread control blocks), then `DUMP`s 0C0h bytes
  there. It writes registers by `SBIN`ing the same 0C0h bytes back. The layout
  is in [debugger.md](debugger.md).
- **Stopping on its own.** When a resumed target stops (a breakpoint, a hook,
  a fault), the console sends an **unsolicited raw `HLTD`**. That is the only
  stop notification nops understands. nops then reads the registers, and
  because it still believes the target is running it brackets the read with
  `HALT` … `CONT`. That `CONT` must not resume the target. See "nops' halt
  bracket" in [debugger.md](debugger.md).
- **Stepping** is done by nops: it sends `HKEX` with the next instruction's
  address, then `CONT`, and waits for the `HLTD`.
- **Breakpoints** are not a command. GDB writes break instructions itself
  with `M`, which nops turns into `SBIN`.

## What tinrom answers today

| While tinrom's menu runs | Handled by |
| --- | --- |
| `PING`, `UPV2`/`UPV3`, `BAUD` | the serial console, `nops_server.c` |
| `DEBG` `HALT` `CONT` `HKEX` `UNHK` `DUMP` `SBIN`, rate tokens | the resident monitor, `src/monitor/nops.c` |
| `HKRD` `HKWR` | `UNSP`, and the address that follows is read and dropped |
| everything else | `UNSP` |

While the console is halted in the debugger, the monitor answers the same
set on its own, plus `PING`.
