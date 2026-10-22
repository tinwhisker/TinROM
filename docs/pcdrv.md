# PCDRV

The host PC's filesystem, as seen from the console. A program running on the
PlayStation opens, reads and writes files that live on the developer's
machine - no disc to burn, no card to shuffle.

## What it actually is

Seven calls, documented by psx-spx under "BIOS PC File Server". They are not
kernel functions: they are **break instructions**, issued with arguments in
`A1`, `A2` and `A3` - not `A0`, unlike every kernel call - and returning two
values:

| Break | Call | Arguments |
| --- | --- | --- |
| `101h` | `PCInit()` | - |
| `102h` | `PCCreat(name, attributes)` | A1, A2 |
| `103h` | `PCOpen(name, accessmode)` | A1, A2 |
| `104h` | `PCClose(handle)` | A1 |
| `105h` | `PCRead(handle, length, destination)` | A1, A2, A3 |
| `106h` | `PCWrite(handle, length, source)` | A1, A2, A3 |
| `107h` | `PClSeek(handle, offset, mode)` | A1, A2, A3 |

`V0` is `0` or `-1`. `V1` is the handle, the byte count, or the file position -
or an error code when `V0` is `-1`.

On a devkit the break was caught by a debug stub, which forwarded it over the
DECI link to a card in the PC. Caetla did the same for cheat cartridges and
exposed it as a `pcdrv:` device to the kernel's own file functions. The
interface is SN Systems'; the transport was always whatever hardware you had.

One trap worth repeating from psx-spx: **`PCRead` does not stop at the end of
the file.** Ask for more than is there and the host pads with zeroes. The only
way to know a file's length is to seek to the end and look at `V1` - and then
put the position back, which is the part that gets forgotten.
`tinromPcdrvLength()` does both.

## The two halves

They are genuinely separate, and tinrom has both.

**tinrom as a client - works.** `src/core/pcdrv.c` speaks the protocol, so any
module can read and write host files: flashing a cart image straight off the
PC, writing a card dump back to it, loading a test executable without burning
anything.

**tinrom as a server for a running program - works.** That means catching
the break instructions a *game or homebrew program* issues and dispatching
them, which needs a resident exception handler: `break` raises a Breakpoint
exception through the general vector at `80000080h`, and something has to sit
there, decide whether the code is one of ours, service it and resume. The
resident monitor is that handler ([exceptions.md](exceptions.md)): Host
filesystem -> Arm the trap installs it, Test the trap makes a PCDRV call and
comes back, and Disarm the trap gives the kernel its vector back.

`tinromPcdrvCall(function, a1, a2, a3)` is the dispatch target - it takes
the arguments in the break interface's own order - so the handler calls it
with what it read out of the frame.

## The wire protocol

Every other command on this link is host-initiated: the host says `SBIN` and
tinrom answers. **PCDRV is the other way round.** The console asks and the host
answers, so the host has to already be listening:

```
python -m tinrom_host pcdrv /dev/ttyUSB0 ./shared
```

That is also why the request carries a token: it lets a host sitting in its
monitor loop tell a PCDRV request from console log output.

Request:

```
  u32  "PCDR"
  u32  function          101h..107h
  u32  a1, a2, a3        the break interface's arguments
  u32  payloadLength     0, or the filename, or the data being written
  u8   payload[payloadLength]
  u32  payloadChecksum   32-bit additive sum, only when there is a payload
```

Reply:

```
  u32  "PCOK" or "PCER"
  i32  v0
  i32  v1
  u32  payloadLength     0, or the data being read
  u8   payload[payloadLength]
  u32  payloadChecksum    only when there is a payload
```

Filenames travel with the request because `A1` is a pointer into the console's
memory and the host cannot follow it.

A host that is not there looks like a failure rather than a hang: every
exchange is bounded by a timeout, and `TINROM_ERR_TIMEOUT` (no host) is kept
distinct from `TINROM_ERR_NOT_PRESENT` (host said no) and `TINROM_ERR_IO`
(host said no to something that should have worked), because those three need
different things done about them.

**This framing is tinrom's own.** NOTPSXSerial supports PCDRV and its framing
is not documented here, because it has not been captured from a real `nops`
build - the rest of `docs/nops-protocol.md` was written from observation and
this has not been. Treat PCDRV as tinrom-to-`tinrom_host` only until someone
has watched the wire.

## The host side

`tools/tinrom_host/pcdrv.py` serves one directory. Paths arrive in whatever
form the program used - DOS style, often with a drive letter, usually with
backslashes - so they are normalised, joined to the root, and then
**`realpath`-checked against it**. Anything that resolves outside the root is
refused rather than served. A program on a console is not a threat model, but
a wrong path should fail visibly rather than reach into the home directory.

## Testing

`scripts/test-pcdrv.sh` runs the real console code against the real Python
server over a pair of pipes, so both halves of the protocol are checked
against each other rather than against one author's idea of it: open, read,
seek, length, create, a 1 KB write-and-read-back, a DOS path with a drive
letter, a subdirectory, a missing file, an attempt to escape the root, and the
documented zero-padding past end of file.
