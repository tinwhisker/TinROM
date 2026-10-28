# Memory cards

`src/hal/mcard.c`. Reading, frame writes, directory surgery and named files
are implemented; whole-card writes are not.

| Function | State |
| --- | --- |
| `tinromMcProbe` | real |
| `tinromMcReadFrame` | real |
| `tinromMcReadDirectory` | real |
| `tinromMcReadImage` | real |
| `tinromMcWriteFrame` | real |
| `tinromMcDelete` / `Undelete` / `Copy` | real |
| `tinromMcFindFile` / `ReadFile` / `WriteFile` | real |
| `tinromMcFormat` | real |
| `tinromMcWriteImage` / `VerifyCapacity` | stub |

The bus is `hal/sio0.c`, shared with the controller driver - see
"SIO0 is one bus with two drivers" in [architecture.md](architecture.md).

## The read command

```
send  reply  meaning
81h   N/A    address; nothing is driving the line yet
52h   FLAG   command, and the card's status byte
00h   5Ah    ID1
00h   5Dh    ID2
MSB   (00h)  sector number, high byte
LSB   (pre)  sector number, low byte
00h   5Ch    acknowledge 1     <- the late /ACK lands after this one
00h   5Dh    acknowledge 2
00h   MSB    confirmed sector, high byte
00h   LSB    confirmed sector, low byte
00h   ...    128 data bytes
00h   CHK    MSB xor LSB xor every data byte
00h   47h    end byte, "G" for good
```

Three details the code depends on:

**/ACK is a "more data" request, so the last byte of a packet never gets
one.** Waiting for an acknowledge after the end byte always times out.

**The seventh byte is slow.** Genuine Sony cards stall for about 31000 cycles
(~900 µs) before acknowledging the first `5Ch`, so that one wait uses
`TINROM_SIO0_ACK_WRITE` rather than the ordinary card timeout. Third-party
cards do not stall, which is one way to tell them apart.

**The confirmed sector is worth comparing.** A sector number the card does not
have is not wrapped silently: Sony cards answer `FFFFh` and then send nothing,
third-party cards answer with the number ANDed with `3FFh` and hand over the
wrong sector. `tinromMcReadFrame` treats a mismatch as `TINROM_ERR_BAD_ARG`
rather than returning data from somewhere else.

`TINROM_ERR_NOT_PRESENT` means no /ACK after the address byte - an empty slot.
`TINROM_ERR_UNSUPPORTED` means something answered `81h` but did not identify
as a card. `TINROM_ERR_CORRUPT` means the frame arrived but its checksum did
not match.

## The directory

Block 0 frame 0 is the header, frames 1..15 are the directory, and directory
entry *N* describes block *N*. Each entry carries an allocation state, a
filesize, a pointer to the next block in the file, and - only in a file's
first entry - a 20-character filename.

`tinromMcReadDirectory` makes three passes: read the fifteen directory frames,
walk each file's chain to count its blocks, then read one title frame per
file. That is up to thirty frame reads, so it belongs in a screen's `enter`,
not in a `tick`.

Block counts come from **walking the chain**, not from dividing the filesize,
because the chain is what decides which cells the browser joins together. When
the two disagree the size is logged and the chain wins. The walk is capped at
fifteen so a card with a circular chain cannot hang the menu.

A frame whose checksum fails is skipped and counted rather than parsed; one
bad directory frame should cost you one slot, not the screen.

Deleting a save only rewrites its directory entry (`51h` becomes `A1h`), so
the file's data and title frame are still intact. The browser shows deleted
saves, with their titles, as a distinct cell kind.

`TinromMcSlot` carries a `deleted` flag separately from `state`, because a
continuation block reports `TINROM_MC_SLOT_LINK` whether its file is deleted
or not. Without it, the second half of a two-block deleted save would not know
it was deleted, and the browser would fade one cell of the pair.

## The write command

`57h`, and the mirror image of the read: address, command, sector MSB and LSB,
128 data bytes, then the XOR checksum - which this time goes **up** to the
card, for the card to judge. Then `5Ch`, `5Dh`, and one byte that is the whole
result:

| byte | meaning |
| --- | --- |
| `47h` `'G'` | written |
| `4Eh` `'N'` | the card computed a different checksum; nothing was written |
| `FFh` | the card refused the sector number |

The card erases and programs flash after the last data byte, so the
acknowledgements from the checksum onwards use `TINROM_SIO0_ACK_WRITE`, the
long timeout, rather than the ordinary one.

**And it is busy for a while after the write.** pcsx-redux writes at once, so
none of this showed there; on a console the next command after a write found
first no answer at all - "no card" - and then, once that was waited out, a
card that answered its address but not with its IDs, which read as "not a
card" and failed a save as "unsupported here". So straight after a write, a
command header that fails in any way is tried again, a millisecond apart, for
up to 100 ms (`MC_BUSY_MAX_MS`); the longest wait seen is logged as
`mcN: busy for about N ms after a write`. Only a write starts the wait, so an
empty slot is found as quickly as ever.

## Format

`tinromMcFormat` writes block 0 the way psx-spx lays out an empty card: the
fifteen directory frames free and formatted (`A0h`, next `FFFFh`), the twenty
frames of the broken sector list with no entries (`FFFFFFFFh`), the write-test
frame 63 and the header frame 0 (`"MC"`) last. The saves' blocks are not
touched, as the BIOS does not touch them, but nothing refers to them any more,
so nothing can be undeleted. 37 frame writes; the card menu's Format asks
first.

**Nothing is read back.** The card's own answer is trusted, which is what save
managers generally do, and it covers a rejected checksum or sector but not a
cell that programmed wrong and reads back differently. If that ever matters,
the place to add it is `writeFrameSelected`, and it roughly doubles the time.

## Delete, undelete and copy

All three are directory surgery in `hal/mcard.c`; none of them touch a save's
data except copy, which only ever writes to blocks nothing points at.

| in use | deleted |
| --- | --- |
| `51h` first block | `A1h` |
| `52h` middle block | `A2h` |
| `53h` last block | `A3h` |
| | `A0h` free, formatted |

Delete adds `50h` to each state in the chain; undelete subtracts it. The
frame's trailing XOR has to be recomputed on every entry that changes, which
is what `signFrame()` is for.

**The first block's entry is written last, in both directions.** The first
entry is the one the browser and the BIOS read, so an interrupted run leaves a
file either wholly present or wholly gone, never one whose head and tail
disagree about what it is.

Undelete checks the whole chain before writing anything: a deleted file's
blocks are free, and something may have claimed one since. If any block is no
longer in a deleted state the operation refuses with `TINROM_ERR_BUSY` and
nothing at all has been written.

Copy:

- goes **between cards only**. A card cannot hold two saves under one
  filename - that is what the BIOS treats as corruption - and the filename is
  how a game finds its save, so renaming the copy would produce a save the
  game cannot see. A duplicate filename on the destination is a refusal.
- takes **any free blocks, adjacent or not**. The chain is a linked list, so
  contiguity was never required.
- prefers formatted blocks and falls back to deleted ones only when those run
  out, which keeps recoverable saves recoverable for as long as the card can.
- writes **all the data first, then the directory entries**. A failure part
  way leaves free blocks holding rubbish that nothing points at, rather than a
  directory entry pointing at data that was never written.
- always writes the destination **in use**, so copying a deleted save is how
  you recover one whose blocks have been taken on the original card.

## Named files

`tinromMcFindFile`, `tinromMcReadFile` and `tinromMcWriteFile` treat a save as
a filename and some bytes, for tinrom's own saves (below). Reading follows the
chain and stops after the bytes asked for. Writing takes whole blocks and goes
in the same order as copy, so a power cut leaves either the old file or the
new one:

1. the data into free blocks: first any that held an earlier copy of the
   same file, then formatted ones, then other deleted saves';
2. the old file of the same name, if any, marked deleted;
3. the new directory entries, the first block's last.

The first of those keeps a file that is saved again and again swapping
between two blocks: each save writes over the copy the save before it
deleted. Without it the file walked across the card, one block on each
time, leaving a trail of deleted copies of itself. Other games' deleted
saves are only taken once there is nothing else, so they stay recoverable
for as long as the card has room.

When the card is too full for a second copy but the old file has enough
blocks of its own, the new one is written over it in place, and any blocks it
no longer needs are freed. That one case is not atomic: a cut part way leaves
a file whose checksum (in tinrom's container) does not match, which reads as
"nothing saved" rather than as wrong settings. With neither, the answer is
`TINROM_ERR_NO_SPACE` and nothing has been written.

Before anything is written through an existing file's chain, every block in
it must be in that file's states - first, middles, last. A damaged pointer
into another save's blocks is refused as `TINROM_ERR_CORRUPT`, so a bad
directory can cost tinrom its own file but never someone else's. When an
in-place write shrinks a file, the shortened chain is written before the old
tail is freed, so nothing in use ever points at a free block.

## tinrom's own saves

`src/core/savefile.c`. Two files, each one block, with a title and a small
icon so the BIOS card manager and other save tools show them sensibly:

| Filename | Title | Holds |
| --- | --- | --- |
| `BETNRM-00001SETTINGS` | tinrom settings | the settings, and usually the bookmarks |
| `BETNRM-00002COMMS` | tinrom bookmarks | the bookmarks, when saved on their own |

After the title and icon frames, from byte 256, is a small container: a
`TNRM` header with a version, a section count, a length and an FNV-1a hash of
what follows, then tagged sections - `SETS` for the settings, `SWCH` for the
cart switch's choices and second profile, and `COMM` for the terminal's
bookmarks, phrases and modem profile. Saving one section reads
the file first and keeps the others, so saving bookmarks never loses settings
and the other way round. A file whose hash does not match, or whose
sections do not exactly fill its length, is treated as absent.

**Which card.** At boot, settings come from card 1 if it has a settings file,
otherwise card 2. That card is then the default for every save; with no
settings anywhere it is card 1 if present, else card 2. So a card 1 full of
game saves and a card 2 for tinrom works without asking: save once to card 2
from **Settings → Save settings to...**, and from then on it is found there.

The settings also record a hash of the build's module ids, because
`moduleEnableMask` is a bit per registry position. Settings written by a build
with different modules keep everything else, but put both profiles' module
choices back to their tier's default and drop their autoboot, which is a
registry position too.

## Showing a deleted save

The GPU has no alpha. For a 4bpp textured sprite, semi-transparency is decided
per texel by bit 15 of its palette entry, and a card's icon palette sets that
bit for its own reasons - so drawing the icon with blending on would make some
texels translucent and leave others solid.

`uiVeilRect` takes the other route. It paints an untextured rectangle over the
cell in semi-transparent mode 0 - `B/2 + F/2` - using the cell's own
background colour as `F`. Over the icon that is a true 50% blend towards the
background, and over the parts of the cell where the background is already
showing it changes nothing, because half a colour plus half of itself is
itself.

The blend mode lives in the draw-mode register rather than in the primitive,
so the packet sets it itself. That is safe because everything textured in
tinrom - icons, glyphs and the text canvas - sets its own page before
drawing.

## Titles

Titles live in the first frame of a file's first block, 64 bytes of Shift-JIS
after the `SC` magic. psx-spx documents which ranges carry English text:

```
81h,40h      --> SPC
81h,43h..97h --> punctuation marks
82h,4Fh..58h --> "0..9"
82h,60h..79h --> "A..Z"
82h,81h..9Ah --> "a..z"
```

tinrom's font is ASCII, so the driver folds what it can and substitutes `?`
for what it cannot - kana and kanji have no ASCII form, and a `?` is more
honest than a blank. The punctuation table in `mcard.c` is the Unicode NFKC
folding of each of those code points, generated rather than typed by hand, so
a full-width comma becomes a comma and nothing is guessed. Twelve entries are
marked as deliberate approximations, because games use them where ASCII has
an equivalent and NFKC does not fold them:

- the three dash characters and the full-width minus become `-`;
- the wave dash becomes `~`;
- the curly single quotes, the acute accent and the prime become `'`;
- the curly double quotes and the double prime become `"`.

Spyro 2's title has a curly double quote where "Ripto's" has its
apostrophe, which the console's own card manager shows as `Ripto"s`; it read
as `Ripto?s` before these were added. The degree sign beside the primes,
`818Bh`, has no ASCII form and stays `?`.

The BIOS card manager also accepts plain 8-bit characters in this field and
some games use them, so bytes in `20h..7Eh` are taken at face value.

Trailing spaces are trimmed; `ＦＦ７：ＣＬＯＵＤ　Ｌｖ９９` comes out as
`FF7:CLOUD Lv99`.

## Icons

Each save keeps a 16x16, 4bpp icon in the sectors after its title frame, with
a 16-entry palette at offset `60h` of the title frame itself. The display flag
at offset `02h` says how many animation frames there are - `11h` one, `12h`
two, `13h` three - and anything else marks a file the BIOS boot menu treats as
corrupt, which `tinromMcReadIcon` reports as `TINROM_ERR_UNSUPPORTED` rather
than drawing.

`tinromMcReadIcon` assembles the frames into one 48x16 sheet, side by side,
which means interleaving them a row at a time. That way the whole icon is a
single texture upload and animating it is a `u` offset rather than a re-upload.
`uiIconUpload` doubles every texel across on the way to VRAM, for the
display's two pixels a unit, so an icon is drawn as a 32x16 sprite, texel for
pixel.

The palette needs no conversion. psx-spx notes that the BIOS draws these with
semi-transparency disabled, so a palette entry of `0000h` is fully transparent
and `8000h` is solid black - exactly the PS1's own texture convention. tinrom
draws them the same way: a raw-texture sprite, `gp0_rectangle(true, true,
false)`, blending off. Turning semi-transparency on would render every solid-black
texel as a 50% blend instead.

Animation follows the BIOS: a two-frame icon changes every 16 frames, a
three-frame icon every 11. Those are PAL frame counts, so on NTSC they run
20% fast - which is what the console does too.

### VRAM

Icons live at `(640, 208)`: thirty 96x16 strips - each frame doubled across -
on three rows of twelve, at y 208, 224 and 240. Their palettes are stacked
above them at `(640, 176)`, one row per handle.

They cannot simply run end to end. A 4bpp texture page is 64 VRAM halfwords
wide and a sprite's `u` is only eight bits, so a 24-halfword strip starting
more than 40 halfwords into a page would address past the end of it. Two
strips fit per page, six pages fill x 640..1023, and the three rows stay
clear of the framebuffers, the font sheet, the model texture and the text
canvas at y 256.

The browser allocates handles statically, fifteen per port, so a handle always
means the same slot on the same card.

### Two traps worth remembering

**DMA sources must be 4-byte aligned.** `sendVRAMData` asserts on it, and
ps1-bare-metal's `_assertAbort` ends in `for(;;)` - so a misaligned upload is
not a warning, it is a hang. `TinromMcIcon` declares `frames` as a `uint32_t`
solely to keep `palette` and `pixels` on 4-byte boundaries; as a `uint8_t` it
put them at offsets 2 and 34 and locked the console on the first icon. Static
asserts on both offsets now fail the build instead.

**Nothing on SIO0 may spin unbounded.** `tinromSio0Exchange` waits on status
flags with an iteration cap and returns `FFh` if it expires. That value fails
every ID, acknowledge and checksum comparison downstream, so a card that stops
responding mid-transfer unwinds through the normal error path instead of
taking the menu with it.

### Cost

Reading both cards in full - directory, titles and icons - is around 180 frame
reads and takes roughly a second, so the browser draws a "Reading card 1..."
frame first rather than going black. It all happens on `enter`; nothing here
runs per frame except the `u` offset that picks the animation frame.

## Watching for a card change

The browser re-probes both slots four times a second while the grid is up, so
pulling a card or pushing one in updates the screen without backing out and
coming in again. `uiGridRun` takes a `poll` callback for this: it runs once
per frame *before* the grid's own frame, so it is free to be slow and to draw
progress frames of its own when it decides to re-read a card.

**Presence is the signal that matters.** A PS1 slot has to be emptied before
it can be refilled, so a swap is always a removal followed by an insertion,
and polling four times a second sees the gap. The FLAG byte is compared too -
psx-spx says it returns to `08h` when a card is re-inserted - but on its own
it cannot tell two never-written cards apart, because bit 3 is cleared only by
*writing*, and tinrom writes only when asked to. The dummy write to sector
`003Fh` that games use would turn FLAG into a swap sense in its own right;
`tinromMcWriteFrame` makes that possible, but tinrom does not do it.

**A single odd reading is not a change.** psx-spx warns that plugging anything
into either port drags the shared data line low for a moment, and that
connecting a pad in slot 1 can corrupt a card access in slot 2 - so the first
poll that disagrees is as likely to be a connector still making contact as a
real event. Two consecutive polls must disagree with the known state before
anything is re-read, which costs half a second and avoids reloading a card
mid-insertion.

The probe itself is four byte exchanges; an empty slot costs the /ACK timeout
instead, so the worst case is about two milliseconds for a pair of empty slots
against a 16 ms frame. Only a detected change triggers the expensive path.

When a card goes away its icon handles are released, so a pulled card's icons
cannot linger on screen, and the grid moves the cursor out of a pane that has
become unavailable while leaving the cell where it was.

## The browser's shape

Selecting "Memory cards" goes straight to the grid. There is no list of
actions in front of it, because every action needs a subject - a save, or a
card - and picking the subject first is the natural order:

- **Cross on a save** opens what can be done to that save: copy, delete or
  undelete (whichever applies), and details. The menu is anchored under the
  cell it belongs to.
- **Cross on a pane's "..." button**, directly under each grid, opens what
  belongs to that card: back up to host, restore from host, format, check
  capacity, and details. It stays reachable when the pane is unavailable,
  because a card that failed to read is exactly when its menu is wanted.

Both are `uiPopupRun`, which repaints the grid underneath itself every frame -
so icons keep animating and card polling keeps running behind an open menu.
`uiGridRun` returns a `UIGridOutcome` saying which of the two happened, and
the cursor takes the value `UI_GRID_CELL_MENU` when it is on the button.

On a save, Copy, Delete, Undelete and Details all work. On a card, Format and
Card details do: Back up, Restore and Check capacity are still stubs.
Backing up needs only the host side of the `MCDN` flow, since
`tinromMcReadImage` is real; the rest wait on `tinromMcWriteImage` and
`tinromMcVerifyCapacity`.

## Counterfeit cards

Clones that advertise 15 blocks but physically alias are still common, and the
failure mode is quiet: saves a few blocks in silently overwrite each other.
There are two tests, and tinrom uses both.

**The free one runs on every card read.** `tinromMcCheckAliasing` asks for
sector `0400h`, one past the end of the address space. psx-spx: a genuine Sony
card answers `FFFFh` as the confirmed sector and aborts without sending data;
a third-party card answers with the sector ANDed with `3FFh` and hands over
the wrong one. A card that wraps there is flagged, and the pane's status line
says "capacity suspect". It costs about ten byte exchanges and catches clones
that alias at the top of the range - which is most of them.

A card has been seen on a console that does neither: it confirms `0400h`
itself, as though the sector were real - a card with more than 1024 frames,
or one that does not check. The test then says nothing either way
(`TINROM_ERR_UNSUPPORTED`), and the card is not flagged. Such a card goes
on to send the frame, and cut off part way it was still talking when the
pad was next polled on the same port - which read as Circle pressed. So the
test lets it finish; and the pad driver now takes only a reply that starts
like a controller's as buttons.

**The deep one is destructive and on demand.** Writing a distinct pattern into
every block and reading it all back is the only test that actually proves
capacity, including clones that alias within range. It needs a backup first,
so it lives behind the card menu with a danger confirm, and it waits on
`tinromMcVerifyCapacity`, which is still a stub.

`TinromMcInfo::suspectCapacity` carries the result of whichever ran last.

## Testing without a console

`tinromMcReadDirectory` sits on four `hal/sio0.h` entry points, so the whole
parser can be exercised on a host by implementing those four against a `.mcr`
image: a state machine that replies to the table above out of a byte array is
about sixty lines. That is how the block-chain, checksum and Shift-JIS paths
were checked before any of this ran on hardware.

Reference: psx-spx, "Controller and Memory Card" - see NOTICE.md.
