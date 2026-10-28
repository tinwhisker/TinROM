# The file browser

Browses a disc or a memory card, shows what a file is, launches it if it is an
executable, and copies it out to a host over PCDRV.

## What it is a view over

Nothing here parses a filesystem or drives hardware. The disc listing walks
`iso9660OpenDir()` / `iso9660NextEntry()`, the card listing calls
`tinromMcReadDirectory()`, and launching goes through `ui/launch.h`. The module
is the part that decides what a person sees.

The memory card view is **read only**. Copy, delete and undelete belong to the
memcard module and are not duplicated here; a second implementation of a write
path is a second implementation of every bug in it.

## One launcher, not two

`ui/launch.c` was lifted out of the cdboot module, which had the only copy of
the load-and-hand-over sequence: parse the PS-EXE header, plan around tinrom's
own footprint, refuse with an explanation, confirm, read the staged prefix and
the direct remainder, shut the HAL down, enter.

It takes a sector reader as a callback, so where the bytes come from is not its
business:

```c
typedef TinromResult (*UILaunchReader)(
    void *user, uint32_t sector, void *buffer, size_t count
);
```

`quiesce` is called once the payload is in RAM and the console is about to
change hands, for the source to stop its own hardware. The disc passes
nothing: every read already ends with the drive paused, spinning and idle,
as the BIOS leaves it for a game. It used to pass `tinromCdStop`, and on a
console Stop's second answer - sent only once the disc has spun down -
landed in the middle of the game's own CD start-up: the disc stopped, then
about 15 seconds passed before the game booted.

Two consequences worth knowing:

- **cdboot now boots through this code.** Section 4 of the hardware test sheet
  covers it, and re-applies after this change.
- The confirmation now names what is being launched rather than saying "the
  disc", because the browser can launch anything and the old wording would have
  been a lie there.

Every refusal is a message on screen rather than a code the caller renders.
There is exactly one way to explain "this executable will not fit" and it
should not be written twice.

## Walking the tree

The current directory is a path string in the `\DIR\SUB` form, and
`iso9660OpenDir` takes separators either way round, so building it with
backslashes costs nothing.

`..` is synthesised as the first row below the root rather than taken from the
directory's own records, which the walker filters out. It reads like a path
that way, and the alternative - showing the real `.` and `..` records - puts
two identical-looking rows at the top of every listing.

There is **one cursor per depth**, so descending and backing out lands where
you left rather than at the top:

```c
static size_t cursors[FB_MAX_DEPTH];
```

Limits, and what happens at them:

| limit | value | at the limit |
| --- | --- | --- |
| entries per directory | 128 | says so in a message box, lists the first 128 |
| depth | 8 | refuses to descend; ISO9660 itself only promises eight levels |
| path length | 128 | refuses to descend |

A listing that quietly stops is worse than one that admits it stopped, which is
why the first of those is a message rather than a silent truncation.

## Copying out to a host

PCDRV can write, so a disc or a card is exactly the sort of thing worth getting
onto a PC. `copyToHost()` opens the host file and pulls each chunk through a
`fill` callback, so the disc and the card only have to know how to fill a
buffer:

- **from the disc**, whole sectors straight into the buffer, and the file's
  last partial sector read into a scratch sector and memcpy'd, since a read
  lands whole sectors and the file does not end on one.
- **from a card**, frame by frame down the save's block chain, which
  `tinromMcSaveChain()` hands over. A chunk is 8192 bytes, exactly one block,
  so the two line up.

The ISO `;1` version suffix is stripped from the host filename. No host wants
it, and some will not accept it.

The file lands relative to whatever directory the host is serving - tinrom does
not choose, and says so in the message when it finishes.

## What is not here

**Browsing the host.** PCDRV has open, read, write and seek, and no directory
listing at all, so there is nothing to enumerate. Naming a path by hand would
need a text entry widget, and tinrom does not have one. When it gets a
keyboard, this is the first thing that should use it.
