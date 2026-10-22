# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# The last few instructions tinrom ever runs.
#
# Handing the console to a game means overwriting the memory tinrom is
# executing from, so the final copy cannot run from there. This stub is
# relocated to the top of RAM and entered through its KSEG1 alias, which is
# uncached - so its own instruction fetches cannot come from cache lines that
# are about to be invalidated underneath it.
#
# On entry a0 points at a parameter block:
#
#   +00h  destination
#   +04h  source (the staged payload)
#   +08h  length in bytes, a multiple of four
#   +0Ch  entry point
#   +10h  initial gp
#   +14h  initial sp
#
# Position independent: no absolute references to itself, and every jump goes
# through a register.

.set noreorder
.set noat

.section .text.tinromHandoffStub, "ax", @progbits

.global tinromHandoffStub
.type tinromHandoffStub, @function
tinromHandoffStub:
	# The parameter block outlives the BIOS call below, and a0 does not: s0
	# is callee-saved, so it is the one register the kernel is obliged to
	# hand back. Relying on a0 surviving FlushCache was a bug waiting for a
	# BIOS version that used it.
	move  $s0, $a0

	lw    $t0, 0($a0)          # destination
	lw    $t1, 4($a0)          # source
	lw    $t2, 8($a0)          # length
	lw    $t3, 12($a0)         # pc
	lw    $t4, 16($a0)         # gp
	lw    $t5, 20($a0)         # sp

	# Interrupts off before anything else. The kernel's FlushCache has a
	# documented bug - it uses k0 without masking interrupts, and loops
	# forever if one lands - and psx-spx's own workaround is to disable
	# them first. We can, because nothing here needs an interrupt.
	mtc0  $zero, $12
	nop

.Lcopy:
	beq   $t2, $zero, .Lflush
	nop
	lw    $t6, 0($t1)
	addiu $t1, $t1, 4
	sw    $t6, 0($t0)
	addiu $t0, $t0, 4
	b     .Lcopy
	addiu $t2, $t2, -4

.Lflush:
	# TINROM_KERNEL_CALL: A(44h) FlushCache. The CPU has no cache coherency,
	# so code just written with stores would otherwise be fetched from stale
	# instruction cache lines. With interrupts already off, the call's one
	# known bug cannot bite.
	#
	# This is the one place tinrom cannot use ps1-bare-metal's flushCache(),
	# as the crash handler now does: that function lives in tinrom's image,
	# and by this point the payload may have overwritten it. Dropping this
	# kernel call would mean carrying a copy of its tag-clear loop in the
	# stub itself, which is small and position-independent - but it is also
	# the last code tinrom ever runs, so not a change to make untested.
	move  $sp, $t5             # the BIOS call needs a stack, and the new one
	move  $fp, $t5             # is the only one guaranteed not to be payload
	li    $t9, 0x000000a0
	jalr  $t9
	li    $t1, 0x44

	# The BIOS clobbers freely, so nothing above survives except what we
	# reload here, through the saved pointer rather than a0.
	lw    $t3, 12($s0)
	lw    $t4, 16($s0)
	lw    $t5, 20($s0)

	mtc0  $zero, $12
	nop

	move  $gp, $t4
	move  $sp, $t5
	move  $fp, $t5
	move  $a1, $zero
	move  $a2, $zero
	move  $a3, $zero
	jr    $t3
	move  $a0, $zero

.global tinromHandoffStubEnd
tinromHandoffStubEnd:
