/*
 * tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The resident monitor is linked on its own, at a fixed address, so that the
 * link itself proves it depends on nothing in tinrom: a stray call into
 * tinrom is an undefined symbol here, not a crash after tinrom has been
 * overwritten. Keep TINROM_MONITOR_BASE/SIZE in monitor_api.h in step.
 */

ENTRY(monitorHeader)

MEMORY {
	MONITOR (rwx) : ORIGIN = 0x801e8000, LENGTH = 0x8000
}

SECTIONS {
	/* The header must be first: tinrom finds it at the base address. */
	.header : {
		KEEP(*(.monitor.header))
	} > MONITOR

	.text : ALIGN(4) {
		*(.text .text.*)
	} > MONITOR

	.rodata : ALIGN(4) {
		*(.rodata .rodata.*)
		. = ALIGN(4);
	} > MONITOR

	.data : ALIGN(4) {
		*(.data .data.* .sdata .sdata.* .got .got.*)
		. = ALIGN(4);
		_monitorImageEnd = .;
	} > MONITOR

	.bss (NOLOAD) : ALIGN(8) {
		_monitorBssStart = .;
		*(.bss .bss.* .sbss .sbss.* COMMON)
		. = ALIGN(4);
		_monitorBssEnd = .;
	} > MONITOR

	/*
	 * Only the sections above end up in the blob: cmake/monitor.cmake
	 * extracts them by name, so an allocated orphan such as .reginfo cannot
	 * pad the image out past the bss. Keep the two lists in step.
	 */
	/DISCARD/ : {
		*(.note.* .gnu_debuglink .gnu.lto_*)
		*(.MIPS.abiflags)
		*(.eh_frame .eh_frame_hdr)
	}
}
