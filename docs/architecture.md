# Architecture

Ideally going to keep it layered, each only allowed to call downwards.

```
  src/modules/   Features. One directory each.
       |
  src/ui/        Menu stuff.
       |
  src/core/      Module registry, settings, logging, main loop.
       |
  src/hal/       Hardware stuff.
       |
  extern/ps1-bare-metal   Stuff that does the heavy lifting.
```

Three pieces stand apart, each built on its own because it has to keep
working after tinrom itself is gone:

```
  src/boot/      The cart's entry stub, assembled straight to a raw binary.
  src/monitor/   The resident debug monitor, copied to 801E8000h at boot.
  src/crash/     The crash screen games get, run straight out of the cart.
```

## Why make the HAL strict?

It keeps Modules readable, because a module that
wants a directory listing calls `iso9660OpenDir()` and `iso9660NextEntry()`,
which read through `tinromCdReadSectors()`, rather than driving the CD-ROM
state machine inline. And every hardware dependency ends up in one
directory, which is what hopefully keeps this auditable.
If a facility can only currently be reached through a BIOS kernel call,
that call is in `src/hal/` with a `TINROM_KERNEL_CALL` comment on it, and it
can be swapped for a native driver later where possible.

## SIO0 is one bus with two drivers

Controllers and memory cards sit on the same bus.
The only things that differ are the address byte
(`01h` for a controller, `81h` for a card), /CS per port,
and how long the device takes to acknowledge.
So the bus lives in `hal/sio0.c` and both drivers go through it:

```
tinromSio0Select(port)    assert /CS on that slot, after flushing the bus
tinromSio0Exchange(byte)  one byte out, one byte in
tinromSio0WaitAck(us)     wait for /ACK, or give up
tinromSio0Deselect()      release /CS, flush, settle
```

|  | PORT1 | PORT2 |
| --- | --- | --- |
| Controller | /CS1 & 0x01 | /CS2 & 0x01 |
| Mem Card | /CS1 & 0x81 | /CS2 & 0x81 |

/CS1 & /CS2

Two details are worth stating because getting either wrong produces results
that look like data:

**Slot select only means something while /CS is asserted.** `SIO_CTRL` bit 1
is /CS and bit 13 chooses the slot. Clearing bit 1 does not select the other
port - it selects nothing, and the data line then floats to whatever the pull-
ups say. A probe written that way reports plausible-looking bytes for a slot
it never addressed.

**The bus has to be left empty.** `tinromSio0Select()` and
`tinromSio0Deselect()` drain the RX FIFO and clear the SIO0 flag in
`IRQ_STAT`. Without that, a byte or a stale acknowledge left over from the
previous transaction is consumed by the next one, and every reply arrives one
transaction late - which reads as port 1 answering for the card in port 0.

Presence is `/ACK` arriving after the address byte, never the byte received
alongside it: nothing is driving the line at that point, because the device
has only just been selected.

## SIO1 when something talks back

tinrom polls SIO1, and the port keeps eight received bytes: a fraction of a
millisecond at the higher rates. That is fine for the serial server, which
only listens when it has asked for something, but not for a modem, which
sends whenever the far end does. So the terminal turns on **buffered
receive** (`tinromSioBufferStart()`): `tinromSioPump()` moves bytes into a
4 KB ring, and runs while `tinromWaitVSync()` waits - where a frame spends
most of its time - and between the rows the terminal draws. RTS drops when
the ring is nearly full, for a modem doing hardware flow control.

Buffering on is also the port's claim: while it lasts the serial server's
background tick and the log mirror stay off SIO1. See
[terminal.md](terminal.md).

## Drawing: the ordering table

The UI renders through a GPU ordering table rather than a flat command chain,
because the menu has a 3D model idling behind it and a flat chain cannot
depth-sort.

The chain is sent starting at the highest bucket, so a **higher index is drawn
earlier and therefore ends up further back**:

| Bucket | Contents |
| --- | --- |
| `UI_Z_CLEAR` (last) | framebuffer clear and drawing-area setup |
| `UI_Z_MODEL_FAR` … `UI_Z_MODEL_NEAR` | the model, bucketed by its own GTE-computed depth |
| `UI_Z_UI_FIRST` … 0 | panels and text, always in front |

One wrinkle is worth knowing before touching `ui.c`: within a single bucket
the chain is **LIFO** — the packet allocated last is drawn first. That is the
opposite of the painter's order the UI code is written in, where a selection
highlight is filled and then text is drawn on top of it. So UI primitives do
not share a bucket. Each `uiFillRect()` or `uiText()` takes the next bucket
down via `nextUIBucket()`, which restores the expected ordering. 128 buckets
are reserved for the UI (`UI_Z_UI_FIRST` is 127); past that the cursor sticks
at 0 and ordering within that bucket inverts. Primitives that cannot overlap
can share one with `uiShareBucket()`.

## The per-frame packet budget

Every GPU command in a frame comes out of one fixed buffer — ps1-bare-metal
sizes it at `GPU_CHAIN_BUFFER_SIZE`, 4096 words since its revision 2af848a
(2048 before) — and text is expensive in it:

| Thing drawn | Words |
| --- | --- |
| one glyph | 4¼ (a 4-word textured rect; four share one chain tag) |
| one string | +2 for its texture-page command |
| one filled rect | 4 |
| one model quad | 10 |

A screenful of text gets close to the limit on its own. Sixteen lines of the
credits screen once cost about 2170 words before any chrome, at five words a
glyph, which is why selecting it used to hang: overrunning the buffer trips an `assert` in
`allocateOrderedGP0Packet()`, and `_assertAbort()` is `for (;;)`. In a release
build `NDEBUG` removes the assert and it scribbles past the buffer instead,
which is worse.

So nothing draws in a loop without asking first:

- `uiFillRect()` and `uiText()` check `uiHasRoom()` and skip rather than
  overrun. `uiText()` is all-or-nothing — half a line reads as a bug, a
  missing line reads as the end of a list.
- `tinromModelDraw()` takes a word budget (`MODEL_WORD_BUDGET`, 500) and stops
  at it. The model draws first, so without a cap it would starve the menu
  drawn on top of it, and a missing menu entry matters more than a missing
  polygon.
- `uiTextPane()` draws until the budget runs out and scrolls by the number of
  lines that actually drew, so content stays fully reachable however long the
  text is. It also hides the model, which is behind a wall of text anyway.

A screen that is mostly text can use a **text canvas** instead
(`src/ui/canvas.c`): monospaced glyphs composed into a bitmap on the CPU and
drawn as one sprite per 256 texels - three at full width - for twenty-one
words however full it is. The hex editor is one; see
[hexedit.md](hexedit.md). There is one canvas slot in VRAM, so one canvas on
screen at a time.

If a screen ever needs more than this, the fix is for tinrom to own its chain
sizing rather than inherit ps1-bare-metal's. The text renderer is tinrom's
own now (see below), so that would only take a packet allocator.

## Text

There is one font, Superpowers' outlined sheet (CC0;
`src/ui/fonts/README.md`), drawn at 20x20. `tools/convertfont.py` keeps its
20 columns and scales its rows down to 10, and turns it into C tables at
build time - a 4bpp sheet cropped to the glyphs, a palette of its shades,
and each glyph's place, width and advance - and `src/ui/font.c` lays strings
out from them. Scaling at build time is the only option: the GPU draws
sprites one texel to one pixel.

The screen is 640x240, but everything is placed on a 320-wide grid of
units, as it was when the screen was 320 wide; `src/ui/ui.c` doubles x and
widths on the way to the GPU. The font is the one thing that uses the extra
columns: its glyphs are measured in display pixels, two to a unit across, and
the layout calls convert. The terminal's screen, a bitmap with one texel to
a unit, is drawn as textured quads, stretched to two pixels a texel, since a
sprite cannot scale. Save icons are small enough to upload already doubled
across and draw as sprites, which is exact: a stretched quad's right edge
sampled the texel column just past the icon. Two renderers draw the font:

- **`uiText()` and friends** (`src/ui/ui.c`) draw a glyph per GPU sprite,
  proportionally spaced. A glyph is modulated by the text's colour, so
  `uiTextColored()` takes any colour, and the font's outline and shading
  darken with it. Glyphs overlap by their shared outline column. Strings are
  laid out by the same code that measures them, so `uiTextWidth()` is exact,
  and `uiTextWrapped()` breaks at spaces for message boxes, confirmations and
  text panes. Tab stops are every 72 units, which is what key/value tables
  use to line up: every key in the UI is narrower than that.
- **The text canvas** draws it monospaced, each glyph centred in an 8-unit
  cell, by writing pre-expanded rows into an 8bpp bitmap 640 texels wide.
  Its palette is the font's shades tinted by sixteen inks, so a canvas shows
  the same glyphs in several colours.

`sh scripts/render-ui.sh` draws the main screens on the host into PNGs,
through a software GPU that executes the real packet chain
(`tools/render_ui/render_ui.c`), for checking layout and text fit without a
console.

## The module lifecycle

A module is a `TinromModule` struct of function pointers, defined in its own
`.c` file as `tinromModule_<id>`. There is no dynamic loading and no
allocation.

| Hook | When | Rules |
| --- | --- | --- |
| `init` | once at boot, in registry order, before the UI | Must be cheap and must not block. An error marks the module unavailable - it still appears in the menu, greyed out, so the user can see it exists and that something is wrong. |
| `enter` | when the user picks its menu entry | Runs its own frame loop. Returns when the user backs out. |
| `tick` | every frame while another screen is up | Only for `TINROM_MODULE_BACKGROUND` modules. Must not block; the serial server services at most a few commands per call. |
| `shutdown` | when the main loop hands over through `ctx->handoff` | Release anything a game would trip over. Launching a disc or an executable through `uiLaunchExe()` does not call it yet; see below. |

## Two independent axes

This is the part worth getting right, because it is easy to conflate.

**Compile-time (`TINROM_MODULE_<ID>`)** decides whether a module exists in the
image at all. CMake generates the registry table from the enabled set, so a
disabled module costs nothing - not even a table slot. This is how the ROM
fits in 128K.

**Runtime (the tier, plus per-module overrides)** decides whether a module
that *is* in the image gets shown. `TINROM_TIER_BASIC` hides the serial
console; it does not remove it. Somebody who wants it can raise the tier in
Settings without reflashing.

Consequences worth remembering:

- `minTier` is presentation, not security. Do not use it to gate anything
  dangerous; use a confirmation prompt.
- Changing the tier resets the per-module overrides, deliberately, so that
  raising the tier reveals modules in a predictable state.
- The registry index is stable for a given build but **not** across builds,
  and `moduleEnableMask` is indexed by it. So the settings save carries a
  hash of the module ids, and one written by a build with a different module
  set keeps everything but the module choices and autoboot, which go back to
  the tier's defaults.

## The context object

`TinromContext` is passed to every hook. It holds the settings, the UI
context, the boot source, and a `handoff` callback, which makes the main
loop shut the modules down before it calls it. Nothing uses it yet. Disc
boot and the file browser launch through one function, `uiLaunchExe()`
(`src/ui/launch.c`), which shuts down the HAL - giving the kernel back its
exception vector and pointing A(40h) at the cart's crash screen - and hands
over without calling the modules' `shutdown` hooks. The serial `SEXE`
command, still a stub, is meant to share that path.

## Logging

Two macros, both in `core/log.h`:

| Macro | Goes to | Gated by |
| --- | --- | --- |
| `TINROM_LOG(fmt, ...)` | 64-line ring buffer; SIO1 when the mirror is on; the emulator console whenever one is detected | the mirror setting, for SIO1 only |
| `TINROM_UART(fmt, ...)` | SIO1 and the emulator console, immediately | nothing |

`TINROM_LOG` is the default. The ring buffer is what the about screen's boot
log pages through, and lines are 80 characters because that is what fits on
screen. The serial mirror is the `ttyToSerial` setting, exposed as "Mirror
log to serial" in the settings screen.

The mirror is **off by default**, and almost every line tinrom logs is written
during boot - the banner, HAL bring-up, module init, the mesh loader - long
before anyone can reach the settings screen. Switching the toggle on therefore
replays the whole ring buffer out of SIO1 (framed by `--- tinrom log
(replaying buffered lines) ---`) before going live, so the boot history is not
lost just because the mirror was not armed in time.

That still cannot help with output from a boot that never reaches the menu.
For that, configure with `-DTINROM_LOG_TO_SERIAL=ON`: the mirror is then armed
by `tinromLogInit()`, before `tinromSettingsLoad()` and before any module runs.

`TINROM_UART` bypasses all of it. It formats into a 160-character stack buffer
and writes straight to the host regardless of any setting, so it is the right
tool for high-volume tracing and for paths that may not survive to draw a
frame. It needs `tinromHalInit()` to have run; anything earlier than that has
no output path at all and should use the ROM stub's POST-register trace
instead (see [cart-rom.md](cart-rom.md)).

### The emulator console

SIO1 is a cable, and under an emulator there usually is not one. PCSX-Redux
has no SIO1 output in its log window, so a mirror that only speaks SIO1 is
invisible there no matter how it is configured.

Redux does expose a putchar port in the top half of EXP2, and
`hal/dbgcon.c` drives it:

| Address | Width | Meaning |
| --- | --- | --- |
| `1F802080h` | 4 (R) | ID, reads back `PCSX` |
| `1F802080h` | 1 (W) | console putchar |
| `1F802081h` | 1 (W) | debug break |
| `1F802082h` | 1 (W) | exit code |

Characters written there come out of `biosPutc()` inside Redux - the same sink
as the BIOS TTY output, so they appear in the Logs window with nothing to
configure on the host side.

One catch: the BIOS leaves EXP2 128 bytes wide, so `1F802080h` is one address
past the end of the window. The POST register the ROM stub traces to is at
`+41h` and is inside it, which is why that has always worked. `hal/dbgcon.c`
widens the size field of `BIU_DEV8_CTRL` (`1F80101Ch`) from 7 to 8 before
probing - the same thing Redux's own `crt0.s` does - and puts it back if
nothing answers. Nothing on a retail console decodes EXP2, so the probe fails
there and every entry point in `dbgcon.c` becomes a no-op.

This sink is **not** gated on the `ttyToSerial` setting. If something is
listening on EXP2 it is a debugger, and it always wants the output; there is
no cable to be unplugged and nothing to turn on. `TINROM_LOG` and
`TINROM_UART` both reach it, and the sysinfo screen reports the detected host.

Sources: [psx-spx I/O map](https://psx-spx.consoledev.net/iomap/),
[PCSX-Redux MIPS API](https://pcsx-redux.consoledev.net/mips_api/).

Do not use ps1-bare-metal's `printf()`/`puts()`. They are linked in - tinrom
uses the same vendored `printf.c` for `vsnprintf` - but they write through
`_putchar()`, which differs from `hal/uart_sio.c` in two ways that matter. It waits
on **CTS** as well as the TX FIFO, so on a three-wire cable with CTS unasserted
it blocks forever; and its companion `initSerialIO()` re-initialises SIO1
without DTR, behind `hal/uart_sio.c`'s back and underneath whatever the serial
console module is doing with the port. `TINROM_UART` exists so nothing needs
them.

## Result handling

Every fallible call returns `TinromResult`. Stubs return
`TINROM_ERR_UNIMPLEMENTED`, and `uiShowResult()` renders it the same way
everywhere, so an unfinished feature reports itself rather than silently
doing nothing. `grep -rn UNIMPLEMENTED src/` is the to-do list.
