# The hex editor

**Hex editor** (developer tier) views and patches six regions:

| Region | Address | Writable |
| --- | --- | --- |
| Main RAM | `80000000h`, 2 MB | yes |
| Scratchpad | `1F800000h`, 1 KB | yes |
| BIOS ROM | `BFC00000h`, 512 KB | no |
| Cart EXP1 | `1F000000h`, the ROM size | no; the flasher writes it |
| Memory card 1, 2 | offsets `00000h`–`1FFFFh` | yes, a frame at a time |

## The screen

```
Main RAM          80010048  byte        tinrom
80010000  00 04 09 0D 12 17 1B 20  ........
...                                (16 rows, 128 bytes)
u8 69   u16 6E69   u32 6F726E69
```

- **The top row** shows the region, the cursor's address and the unit size. It
  shows `EDIT` while editing. When the cursor is in RAM that tinrom depends on,
  it says whose: `kernel`, `tinrom` or `monitor`.
- **The data rows** show the address, then eight bytes, four halfwords or two
  words, then the same eight bytes as ASCII. Halfwords and words are shown as
  the CPU loads them, little-endian.
- **The bottom row** shows the value under the cursor at each size. It also
  carries messages, and for a card, whether the frame has unsaved edits.

Colours:

- zero bytes are dimmed;
- a byte that something else just changed stays orange for half a second;
- edited but unsaved card bytes are green.

## Buttons

| Browsing | |
| --- | --- |
| D-pad | move by a unit, or a row |
| L1 / R1 | a page (128 bytes) |
| L2 / R2 | 4 KB (256 bytes in the scratchpad); 8 KB (one block) on a card |
| Square | bytes → halfwords → words |
| Cross | edit |
| Triangle | menu |
| Start | find next |
| Select | go back (after following a pointer, going to an address, or jumping to the start or end) |
| Circle | leave |

| Editing | |
| --- | --- |
| Up / Down | roll the digit with the bar under it |
| Left / Right | move between digits; past the unit's edge, onto the next unit |
| Cross | finish |
| Circle | drop this unit's change and finish |

A unit is **written the moment you leave it**. That can mean moving on to the
next unit, or Cross. In the halfword and word views it is written as one
access of that size, so the value arrives whole.

The first edit in the kernel's RAM, in tinrom's own image, or in the debugger's
monitor asks before going ahead, once for each kind per visit. Writes there can
crash tinrom; the hex editor allows them because sometimes that is the point.

## The menu

- **Go to address.** Dial in eight hex digits. Any mirror of main RAM works.
  On a card this is an offset.
- **Find / Find next.** Search for a byte, halfword, word or up to 16
  characters of text, forwards from the cursor to the end of the region.
  Values match only at addresses aligned to their size, which is what a
  program loading them would see. A search over a card reads every frame, so
  it takes a while; Circle stops it.
- **Follow pointer.** Treat the word under the cursor as an address and go
  there. **Go back** returns.
- **Undo last write.** The last 16 writes are kept. On a card they are
  forgotten when the frame changes, because the frame is either written or
  discarded then.
- **Start of region / End of region.**
- **Write frame to card / Reload frame** (cards only).

## Memory cards

A card is shown one 128-byte frame per screen, read when it comes into view.
Edits change a copy, so nothing reaches the card until you choose **Write frame
to card**, or confirm when leaving a frame with changes. Leaving offers:

- write it,
- discard the changes,
- stay on the frame.

The frame is then read back and compared, and the bottom row says whether it
matched.

Frames 0–35 of block 0 (the header, the directory and the broken sector list)
end in an XOR checksum of the other 127 bytes. When one of those is written,
tinrom recomputes that checksum, so an edited directory entry is still valid.
The confirmation says so.

## How it is drawn

The data rows are a **text canvas** (`src/ui/canvas.c`): 16 rows of
monospaced glyphs, composed into an 8bpp bitmap by the CPU, uploaded to VRAM
beside the framebuffers and drawn as three sprites. The normal text path
spends about four packet words per glyph out of a 4096-word frame and spaces
glyphs proportionally. The rows are about 520 glyphs - more than half the
frame, and more than the whole of the 2048-word one the editor was written
for - and would not line up if they did fit. The canvas costs twenty-one
words. It uses the UI font,
each glyph centred in an 8-pixel cell; a byte is two of those and a 4-pixel
gap.

The lines above and below the rows are ordinary text, in colour.

The cursor is a filled rectangle drawn behind the canvas: the canvas is
transparent between the glyphs, so it shows through.

## Tests

`sh scripts/test-hexedit.sh` builds the module for the host, with the GPU,
pads, memory and card faked, and drives it with scripted button presses. It
covers:

- editing across units, and undo;
- read-only regions;
- value and text searches, including a match split across two card frames;
- writing a card frame back with its checksum fixed;
- discarding a card edit;
- the warning before editing kernel RAM.
