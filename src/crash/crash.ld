/*
 * tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The crash screen is linked on its own, to run where it sits in the cart:
 * TINROM_CRASH_BASE, which cmake/crash.cmake defines as the uncached view of
 * the ROM plus TINROM_ROM_CRASH_OFFSET. Running uncached keeps the cache
 * out of the question entirely, and it is fast enough for one screen.
 *
 * It runs from ROM after a game has had the console, so there is nowhere
 * for writable data to live: the link fails if any turns up. Its stack is
 * the scratchpad (entry.S).
 */

ENTRY(crashHeader)

SECTIONS {
	. = TINROM_CRASH_BASE;

	/* The header must be first: tinrom and makerom.py find it there. */
	.header : {
		KEEP(*(.crash.header))
	}

	.text : ALIGN(4) {
		*(.text .text.*)
	}

	/* The end is taken inside .rodata, where no orphan can come between. */
	.rodata : ALIGN(4) {
		*(.rodata .rodata.*)
		. = ALIGN(4);
		_crashEnd = .;
	}

	.data : ALIGN(4) {
		_crashDataStart = .;
		*(.data .data.* .sdata .sdata.*)
		*(.bss .bss.* .sbss .sbss.* COMMON)
		_crashDataEnd = .;
	}

	ASSERT(_crashDataEnd == _crashDataStart,
		"the crash screen runs from ROM: it can have no writable data")

	/*
	 * Only .header, .text and .rodata end up in the blob: cmake/crash.cmake
	 * extracts them by name. These two are placed here, after everything,
	 * because left to itself a linker may put .reginfo first - ahead of the
	 * header, moving every address in the blob. Nothing here is non-PIC
	 * code's to use: a GOT is empty, or only a linker's reserved entries.
	 */
	.reginfo : { *(.reginfo) }
	.got     : { *(.got .got.*) }

	/DISCARD/ : {
		*(.note.* .gnu_debuglink .gnu.lto_*)
		*(.MIPS.abiflags)
		*(.eh_frame .eh_frame_hdr)
	}
}
