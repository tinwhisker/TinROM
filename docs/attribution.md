# Attribution policy

`NOTICE.md` is the ledger. This is the rule for keeping it honest.

## Before adding any third-party material

Answer these, in the pull request:

1. **What is it, and where is it from?** Name and upstream URL.
2. **What licence?** Read it. Note it. If it is not compatible with MIT
   redistribution, it does not go in - full stop.
3. **Is it code, data, or knowledge?**
   - *Code or data* that ships in the image or repository gets a section under
     "Bundled / linked source" in `NOTICE.md`, with its licence text preserved
     wherever the licence requires it.
   - *Knowledge* - a documented register layout, a wire protocol, a file
     format - gets a section under "Documented knowledge", because the work
     would not exist without it even though nothing was copied.
4. **Did any code get copied?** If yes, say so plainly. "Derived from X" and
   "written from a description of X" are different claims; make the right one.

## The two hard rules

**No Sony code.** No PsyQ, no Psy-Q libraries, no SDK headers, no BIOS dumps,
no pasted kernel disassembly.

**BIOS kernel calls are allowed, and must be marked.** Calling a documented
A0/B0/C0 entry point is using an interface, not copying an implementation.
Every such call site carries a `TINROM_KERNEL_CALL` comment so it can be found
and, where a native driver becomes available, replaced. They live in
`src/hal/` and nowhere else.

## Reference implementations, and which ones may be copied

Reading someone else's working driver is the fastest way to find out what a
specification left out. Whether it can then be *used* depends entirely on its
licence, and the licences here are not what you would guess:

| Project | Licence | May tinrom copy from it? |
| --- | --- | --- |
| OpenBIOS (`pcsx-redux/src/mips/openbios/`) | MIT, per file | **Yes**, with attribution |
| pcsx-redux, everything else | GPL-2.0 | No, not under an MIT tinrom |
| tonyhax | WTFPL | **Yes** |
| NOTPSXSerial | MPL-2.0 | Only by keeping its files separate and marked |
| ps1-bare-metal | MIT | Yes, and already is |

Two of those catch people out. **pcsx-redux's repository licence is GPL-2.0 -
it is a PCSX fork - but the OpenBIOS files inside it carry their own MIT
headers**, and the file header governs. And **tonyhax is WTFPL, not GPL**, so
the thing everyone assumes is off limits is the most permissive of the lot.

None of this changes the rule: anything actually taken gets an entry in
`NOTICE.md` under "Bundled / linked source", its notice preserved, and an
honest claim about whether it was copied or written from a description. The
point of the table is that "I cannot use that" should be a checked fact, not
an assumption - it was an assumption here once, and it was wrong.

### Where OpenBIOS is worth reading

It is a from-scratch reimplementation of the kernel in readable C, which makes
it the best available second opinion on the parts of tinrom that psx-spx
describes but does not demonstrate:

- **The CD-ROM driver.** `src/hal/cdrom.c` reads discs and plays audio, but
  its timing - when a command's second answer comes, and what arrives before
  it - is still being learnt the hard way. OpenBIOS has a working
  command/response loop to compare against.
- **`FlushCache`.** `src/hal/handoff.S` calls the kernel's, working around a
  documented bug in it. OpenBIOS reimplements it, which is what a
  self-contained version would look like.
- **The kernel's exception handling.** How an exception nothing handled
  reaches `A(40h)`, which the cart's crash screen depends on
  (`docs/exceptions.md`). OpenBIOS is not Sony's kernel, but it is a second
  opinion on what that path does.
- **Memory card and controller drivers**, as a cross-check on `hal/mcard.c`
  and `hal/pad.c`.

## Magic values

Interface constants are not code. The expansion ROM ID string
(`src/boot/rom_header.h`, `tools/makerom.py`) is a value the console's boot ROM
compares against, in the same way a file format has a magic number. tinrom
emits it, documents exactly what it is in `docs/cart-rom.md`, and puts it
behind a build option so the choice is visible.

The same reasoning covers the licence text the CD-ROM sub-CPU compares against
in the unlock sequence (`src/hal/cdrom.c`): documented in psx-spx, emitted as
command parameters rather than embedded as data, and explained in
`docs/cd-boot.md`.

If a value like this is ever ambiguous, the answer is to document it and make
it optional - not to quietly include it and hope nobody looks.
