/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Cart ROM entry stub.
 *
 * This is assembled straight to a raw binary and dropped into the image by
 * tools/makerom.py; it is never linked, so it must not contain a single
 * relocation. Every branch here is PC-relative and every constant is either
 * immediate or read from a fixed EXP1 address, which is why it can sit at
 * whatever offset makerom.py chooses.
 *
 * Two entry points, at fixed offsets the header's entry words point at:
 *   +0x00  post-boot: the kernel is up, the logo is on screen. Load and run.
 *   +0x08  pre-boot:  almost nothing is initialised. Arms the mid-boot
 *                     breakpoint (TINROM_ROM_MIDBOOT), then returns.
 *
 * Those two offsets are a contract with src/boot/rom_header.h and
 * tools/makerom.py. Do not insert anything ahead of them - put new code in
 * the bodies further down, which is why each entry is a branch and nothing
 * more.
 *
 * The return address to the boot ROM is in $ra in both cases, so returning
 * cleanly is always an option.
 */

#ifndef TINROM_ROM_BASE
#define TINROM_ROM_BASE 0x1f000000
#endif

/*
 * KSEG1 (uncached) view of EXP1, spelled out as a lui immediate below rather
 * than as a symbol: %hi()/%lo() would emit relocations, and this object is
 * never linked.
 */
#define DESC_OFFSET  0x0100

/*
 * Where makerom.py places this stub inside the image. Must match
 * TINROM_ROM_STUB_OFFSET in src/boot/rom_header.h and ROM_STUB_OFFSET in
 * tools/makerom.py - the mid-boot trampoline computes ROM addresses from it.
 */
#define STUB_OFFSET  0x0140

/*
 * The COP0 breakpoint vector. Hardware jumps here on a BPC match - note this
 * is NOT the general exception vector at 80000080h. Written through KSEG1
 * (A0000040h) so the trampoline lands in RAM rather than sitting in cache.
 */
#define BREAK_VECTOR_UNCACHED 0xa0000040

/* The kernel's shell entry point - where we want to seize control. */
#define SHELL_ENTRY 0x80030000

/*
 * DCIC: TR (jump to debug vector) | UD | KD (break in either mode) |
 * PCE (pc breakpoint) | DE (gates bits 24-31). Without DE the rest is inert.
 */
#define DCIC_ARMED 0xe1800000

/*
 * The EXP2 POST register, addressed through KSEG1 so the write is uncached -
 * 1F802041h is the same register seen through cached KUSEG, which is wrong
 * for MMIO. Matches ps1-bare-metal's DEV8_BASE of 0xbf802000.
 *
 * The boot ROM writes its own progress here and emulators surface it -
 * PCSX-Redux logs each byte as "BIOS Trace1: 0xNN". It is the only way to see
 * whether this stub ran at all, because at this point tinrom owns no screen
 * and no serial port.
 *
 * Safe: the boot ROM has already written to it before either entry point is
 * called, so the register is known good by the time we get here.
 */
#define POST_REGISTER 0xbf802041

	.set noreorder
	.set noat
	.section .text, "ax"
	.align 2

#ifdef TINROM_ROM_TRACE
.macro TRACE value
	lui     $t9, (POST_REGISTER >> 16)
	ori     $t9, $t9, (POST_REGISTER & 0xffff)
	li      $t8, \value
	sb      $t8, 0($t9)
.endm
#else
.macro TRACE value
.endm
#endif

	.globl tinromRomStub
	.type  tinromRomStub, @function

tinromRomStub:
/* +0x00 - post-boot entry. Must stay at offset 0. */
	b       postBootBody
	 li     $t0, 2                  /* TINROM_BOOT_CART */

/* +0x08 - pre-boot entry. Must stay at offset 8. */
tinromRomPreBoot:
	b       preBootBody
	 li     $t0, 3                  /* TINROM_BOOT_PREBOOT */

#ifdef TINROM_ROM_MIDBOOT
/*
 * Where the trampoline lands us, back in ROM, with the kernel fully
 * initialised and the CD-ROM untouched. From here it is an ordinary load.
 */
midBootBody:
	TRACE   0xe4
	b       stubCommon
	 li     $t0, 6                  /* TINROM_BOOT_MIDBOOT */

/*
 * The trampoline itself. This is copied verbatim into 80000040h, so it must
 * be position-independent and must fit in the 40h bytes before the general
 * exception vector at 80000080h.
 *
 * $k0 and $k1 are the registers reserved for exception handlers; nothing
 * expects them preserved, which is exactly why they are safe to use here
 * before we have any idea what the kernel was doing.
 */
	.align 2
midBootHandler:
	/*
	 * Disable breakpoints FIRST. Until DCIC is cleared this handler is one
	 * breakpoint away from re-entering itself, and psx-spx calls this out
	 * explicitly.
	 */
	mtc0    $zero, $7               /* DCIC = 0 */
	nop
	nop
	lui     $k0, 0xbf00
	ori     $k0, $k0, (STUB_OFFSET + (midBootBody - tinromRomStub))
	jr      $k0
	 nop
midBootHandlerEnd:
#endif

postBootBody:
	TRACE   0xe0
	b       stubCommon
	 nop

preBootBody:
	TRACE   0xe1

	/*
	 * Pre-boot is far too early to do anything useful directly: the A0/B0/C0
	 * vectors do not exist yet and main RAM has barely been set up. So
	 * instead of trying to run here, arm a trap for later.
	 *
	 * Copy a small trampoline into the COP0 breakpoint vector, set an
	 * opcode-fetch breakpoint on the kernel's shell entry point, and return
	 * to the BIOS. The kernel then initialises everything normally, and the
	 * instant it reaches the shell the breakpoint fires into our trampoline -
	 * after the kernel is usable, but before the CD-ROM is touched.
	 *
	 * This is the technique Action Replay v2 and later use; see
	 * docs/cart-rom.md.
	 */
#ifdef TINROM_ROM_MIDBOOT
	/* $t1 = trampoline source in ROM, $t2 = the breakpoint vector. */
	lui     $t1, 0xbf00
	ori     $t1, $t1, (STUB_OFFSET + (midBootHandler - tinromRomStub))
	lui     $t2, (BREAK_VECTOR_UNCACHED >> 16)
	ori     $t2, $t2, (BREAK_VECTOR_UNCACHED & 0xffff)
	li      $t3, (midBootHandlerEnd - midBootHandler)

trampolineCopy:
	lbu     $t7, 0($t1)
	addiu   $t1, $t1, 1
	sb      $t7, 0($t2)
	addiu   $t3, $t3, -1
	bnez    $t3, trampolineCopy
	 addiu  $t2, $t2, 1

	/* Exact-match opcode breakpoint on the shell entry point. */
	li      $t1, SHELL_ENTRY
	mtc0    $t1, $3                 /* BPC  */
	nop
	li      $t1, 0xffffffff
	mtc0    $t1, $11                /* BPCM - compare every bit */
	nop
	li      $t1, DCIC_ARMED
	mtc0    $t1, $7                 /* DCIC - arm it */
	nop
	nop

	TRACE   0xe5                    /* trampoline installed, breakpoint armed */
#endif

	jr      $ra
	 nop

stubCommon:
	TRACE   0xe2

	/* $t1 = uncached pointer to the descriptor makerom.py wrote. */
	lui     $t1, 0xbf00             /* EXP1, KSEG1 */
	ori     $t1, $t1, DESC_OFFSET

	lw      $t2, 8($t1)             /* payloadOffset */
	lw      $t3, 12($t1)            /* payloadSize   */
	lw      $t4, 16($t1)            /* loadAddress   */
	lw      $t5, 20($t1)            /* entryPC       */
	lw      $t6, 24($t1)            /* initialSP     */

	beqz    $t3, stubReturn         /* nothing to load: give up politely */
	 nop

	/* $t2 = uncached source pointer. */
	lui     $t7, 0xbf00
	addu    $t2, $t2, $t7

copyLoop:
	/*
	 * EXP1 is an 8-bit bus in its default configuration, so this copies a
	 * byte at a time. It is slow (a 512K image takes a noticeable moment) but
	 * it is the only width that is safe before the bus has been reconfigured.
	 */
	lbu     $t7, 0($t2)
	addiu   $t2, $t2, 1
	sb      $t7, 0($t4)
	addiu   $t3, $t3, -1
	bnez    $t3, copyLoop
	 addiu  $t4, $t4, 1

	/*
	 * TODO: invalidate the instruction cache before jumping into freshly
	 * written memory. The kernel's FlushCache is available on the post-boot
	 * path but not the pre-boot one, so this wants the COP0 isolate-and-
	 * invalidate sequence written out here. Without it this stub is only
	 * reliable on a cold boot. See docs/cart-rom.md.
	 */

	TRACE   0xe3

	beqz    $t6, stubJump
	 nop
	move    $sp, $t6

stubJump:
	move    $a0, $t0                /* boot source, forwarded as argc */
	move    $a1, $zero
	jr      $t5
	 nop

stubReturn:
	TRACE   0xef
	jr      $ra
	 nop

	.size tinromRomStub, . - tinromRomStub
