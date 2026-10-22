# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# The crash screen's header and its way in. See crash_api.h.
#
# The kernel gets here through A(40h), from inside its own exception
# handler, once nothing else has claimed the exception: the registers the
# program had are in the current thread's TCB, and COP0 still describes the
# fault, since nothing in between takes another exception. Nothing is ever
# returned to - the kernel's own A(40h) only hangs - so every register is
# free to use.

#include "crash/crash_api.h"

/*
 * The top of the 1 KB scratchpad, which is data memory inside the CPU and
 * works whatever state RAM, the caches and the program are in. Whatever the
 * program kept there is lost, and that is all.
 */
#define CRASH_STACK 0x1f8003f0

.set noreorder
.set noat

.section .crash.header, "a", @progbits

.global crashHeader
.type crashHeader, @object
crashHeader:
	.word TINROM_CRASH_MAGIC
	.word TINROM_CRASH_VERSION
	.word crashHeader          # base: where it was linked to run
	.word _crashEnd            # end
	.word crashEntry           # entry, for A(40h)
	.word 0, 0, 0
.size crashHeader, . - crashHeader

.section .text.crashEntry, "ax", @progbits

.global crashEntry
.type crashEntry, @function
crashEntry:
	mfc0  $a0, $13             # cause
	mfc0  $a1, $14             # epc
	mfc0  $a2, $8              # badvaddr
	mfc0  $a3, $12             # sr, as the exception left it

	# Kernel mode, interrupts off, and the data cache not isolated: a crash
	# in the middle of a cache flush would otherwise turn every store below,
	# the GPU's included, into a write to the cache.
	mtc0  $zero, $12
	nop
	nop

	lui   $sp, (CRASH_STACK >> 16)
	ori   $sp, $sp, (CRASH_STACK & 0xffff)
	move  $fp, $sp

	jal   crashMain
	nop

	# crashMain does not return. If it ever did, this is the hang the
	# kernel's own A(40h) would have given.
1:
	b     1b
	nop
.size crashEntry, . - crashEntry
