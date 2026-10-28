/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * Self-extracting PS-EXE entry stub.
 *
 * tools/packexe.py turns a PS-EXE into a smaller one that unpacks itself:
 * this stub, the payload decoder (src/boot/unpack.c) and the original's
 * body packed with tools/tinlz.py, loaded just past where the original
 * goes. Whatever loads it - the kernel's Exec, an emulator, a loader on a
 * memory card - runs this; it unpacks the original to where it was linked
 * and jumps to the original's entry point, so from there on the program
 * cannot tell it was ever packed.
 *
 * Like src/boot/rom_stub.S it is assembled straight to a raw binary and
 * never linked, so it must not contain a single relocation. What it needs
 * to know is in the table at +0Ch, which packexe.py fills in, and every
 * offset in it is from the stub's first byte.
 *
 * Layout, a contract with tools/packexe.py:
 *
 *   +00h  entry: a call over the table, which leaves its address in $ra
 *   +0Ch  the decoder's offset from the stub
 *   +10h  the packed body's offset from the stub
 *   +14h  where the body unpacks to: the original's load address
 *   +18h  its size once unpacked
 *   +1Ch  the original's entry point
 *   +20h  code
 *
 * It is entered as any PS-EXE is, with $a0 and $a1 the loader's arguments
 * and $sp, $gp and $ra as the loader set them; the original gets the same.
 * Its $s0 to $s2 and $ra are put back as they came, so the original returns
 * to the loader, should it return, exactly as it would have.
 *
 * Nothing invalidates the instruction cache before the jump, as in the cart
 * stub (see its TODO). The kernel's Exec flushes the cache before it runs
 * this, and nothing between then and the jump runs from where the original
 * unpacks to, so no line of it can be stale.
 */

	.set noreorder
	.set noat
	.section .text, "ax"
	.align 2

/* Where the table is: the return address of the bal at +04h. */
#define TABLE         0x0c

#define TABLE_DECODER 0x0c
#define TABLE_PACKED  0x10
#define TABLE_DEST    0x14
#define TABLE_SIZE    0x18
#define TABLE_ENTRY   0x1c

/* o32: the 16 bytes at the bottom are the callee's to keep $a0-$a3 in. */
#define FRAME 32

	.globl tinromExeStub
	.type  tinromExeStub, @function

tinromExeStub:
/*
 * +00h - entry. The call puts the address of the table in $ra, which is how
 * the stub finds itself; the loader's $ra waits in $at meanwhile. Spelled
 * out as fixed offsets rather than label arithmetic, which not every
 * assembler gets right in a section that is never linked.
 */
	move    $at, $ra
	bal     body
	 nop

/* +0Ch - the table, zero here, filled in by tools/packexe.py. */
	.word   0                       /* decoder offset */
	.word   0                       /* packed body offset */
	.word   0                       /* destination */
	.word   0                       /* unpacked size */
	.word   0                       /* entry point */

/* +20h */
body:
	addiu   $sp, $sp, -FRAME
	sw      $at, 28($sp)            /* the loader's $ra */
	sw      $s0, 24($sp)
	sw      $s1, 20($sp)
	sw      $s2, 16($sp)

	move    $s0, $a0                /* the loader's arguments, for later */
	move    $s1, $a1
	addiu   $s2, $ra, -TABLE        /* $s2 = where this stub is */

	/*
	 * tinromUnpack(src, dst, size), the decoder packexe.py put after this
	 * stub. It is ordinary compiled C, so it may use $t0-$t9, $a0-$a3 and
	 * the stack, and keeps $s0-$s7. Position-independent code expects its
	 * own address in $t9.
	 */
	lw      $t9, TABLE_DECODER($s2)
	lw      $a0, TABLE_PACKED($s2)
	lw      $a1, TABLE_DEST($s2)
	lw      $a2, TABLE_SIZE($s2)
	addu    $t9, $t9, $s2
	jalr    $t9
	 addu   $a0, $a0, $s2

	/* The original's entry, with everything as the loader left it. */
	lw      $t0, TABLE_ENTRY($s2)
	move    $a0, $s0
	move    $a1, $s1
	lw      $ra, 28($sp)
	lw      $s0, 24($sp)
	lw      $s1, 20($sp)
	lw      $s2, 16($sp)
	jr      $t0
	 addiu  $sp, $sp, FRAME

	.size tinromExeStub, . - tinromExeStub
