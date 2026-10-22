# tinrom - (C) 2026 TinWhisker - MIT licensed
#
# The exception handler proper, and the monitor's guarded memory accessors.
#
# Entered from a trampoline at 80000080h with nothing saved and nothing safe
# except k0 and k1, which the architecture reserves for exactly this. The
# faulting context's stack may be unusable - that is often what the exception
# is about - so the handler switches to its own before calling any C.
#
# Offsets come from hal/exception.h so the frame layout cannot drift away from
# the C struct that reads it.
#
# This file used to be src/hal/exception.S. It moved into the monitor because
# the handler has to survive a program being loaded over tinrom.

#include "hal/exception.h"

.set noreorder
.set noat

# Bytes of stack for the dispatcher and the debugger's stop loop.
#define MONITOR_STACK_BYTES 3072
#define NESTED_STACK_BYTES  1024
# o32 lets a callee spill its arguments into 16 bytes its caller reserves.
#define ARG_AREA            16

.section .bss.tinromExceptionFrame, "aw", @nobits
.align 4
.global tinromExceptionFrame
tinromExceptionFrame:
	.space TINROM_EXC_FRAME_WORDS * 4

.align 4
exceptionStack:
	.space MONITOR_STACK_BYTES
exceptionStackTop:

# A second stack, for an exception taken while the handler itself is
# running. See "nested" below.
.align 4
nestedStack:
	.space NESTED_STACK_BYTES
nestedStackTop:

# Non-zero while a guarded access is in progress: the address to resume at if
# that access faults. See monitorPeek below.
.align 4
.global monitorGuard
monitorGuard:
	.space 4

# Non-zero while the handler is running, so a second exception taken inside
# it - in tinrom's PCDRV code, say - does not overwrite the first one's frame.
.align 4
.global monitorDepth
monitorDepth:
	.space 4

.section .text.tinromExceptionHandler, "ax", @progbits

.global tinromExceptionHandler
.type tinromExceptionHandler, @function
tinromExceptionHandler:
	# A guarded access faulted. Nothing has been saved and nothing needs to
	# be: the accessor was written to expect this. Clear the guard and resume
	# at its recovery address. rfe in the delay slot pops the mode stack the
	# exception pushed, exactly as the normal path does.
	lui   $k0, %hi(monitorGuard)
	lw    $k1, %lo(monitorGuard)($k0)
	nop
	beqz  $k1, 1f
	nop
	sw    $zero, %lo(monitorGuard)($k0)
	jr    $k1
	rfe

1:
	# Already inside the handler? Then the frame holds the context that is
	# worth keeping, and this exception must not touch it.
	lui   $k0, %hi(monitorDepth)
	lw    $k1, %lo(monitorDepth)($k0)
	nop
	bnez  $k1, nested
	li    $k1, 1
	sw    $k1, %lo(monitorDepth)($k0)

	lui   $k0, %hi(tinromExceptionFrame)

	# General purpose registers, at their own register numbers. k0 and k1 are
	# not saved: they belong to the handler, and nothing may rely on them
	# surviving an exception.
	sw    $at, %lo(tinromExceptionFrame) + TINROM_EXC_R(1)($k0)
	sw    $v0, %lo(tinromExceptionFrame) + TINROM_EXC_R(2)($k0)
	sw    $v1, %lo(tinromExceptionFrame) + TINROM_EXC_R(3)($k0)
	sw    $a0, %lo(tinromExceptionFrame) + TINROM_EXC_R(4)($k0)
	sw    $a1, %lo(tinromExceptionFrame) + TINROM_EXC_R(5)($k0)
	sw    $a2, %lo(tinromExceptionFrame) + TINROM_EXC_R(6)($k0)
	sw    $a3, %lo(tinromExceptionFrame) + TINROM_EXC_R(7)($k0)
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_R(8)($k0)
	sw    $t1, %lo(tinromExceptionFrame) + TINROM_EXC_R(9)($k0)
	sw    $t2, %lo(tinromExceptionFrame) + TINROM_EXC_R(10)($k0)
	sw    $t3, %lo(tinromExceptionFrame) + TINROM_EXC_R(11)($k0)
	sw    $t4, %lo(tinromExceptionFrame) + TINROM_EXC_R(12)($k0)
	sw    $t5, %lo(tinromExceptionFrame) + TINROM_EXC_R(13)($k0)
	sw    $t6, %lo(tinromExceptionFrame) + TINROM_EXC_R(14)($k0)
	sw    $t7, %lo(tinromExceptionFrame) + TINROM_EXC_R(15)($k0)
	sw    $s0, %lo(tinromExceptionFrame) + TINROM_EXC_R(16)($k0)
	sw    $s1, %lo(tinromExceptionFrame) + TINROM_EXC_R(17)($k0)
	sw    $s2, %lo(tinromExceptionFrame) + TINROM_EXC_R(18)($k0)
	sw    $s3, %lo(tinromExceptionFrame) + TINROM_EXC_R(19)($k0)
	sw    $s4, %lo(tinromExceptionFrame) + TINROM_EXC_R(20)($k0)
	sw    $s5, %lo(tinromExceptionFrame) + TINROM_EXC_R(21)($k0)
	sw    $s6, %lo(tinromExceptionFrame) + TINROM_EXC_R(22)($k0)
	sw    $s7, %lo(tinromExceptionFrame) + TINROM_EXC_R(23)($k0)
	sw    $t8, %lo(tinromExceptionFrame) + TINROM_EXC_R(24)($k0)
	sw    $t9, %lo(tinromExceptionFrame) + TINROM_EXC_R(25)($k0)
	sw    $gp, %lo(tinromExceptionFrame) + TINROM_EXC_R(28)($k0)
	sw    $sp, %lo(tinromExceptionFrame) + TINROM_EXC_R(29)($k0)
	sw    $fp, %lo(tinromExceptionFrame) + TINROM_EXC_R(30)($k0)
	sw    $ra, %lo(tinromExceptionFrame) + TINROM_EXC_R(31)($k0)

	mfhi  $t0
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_HI($k0)
	mflo  $t0
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_LO($k0)

	# COP0. BadVaddr is only meaningful for the address errors and TAR only
	# when the exception happened in a branch delay slot, but they are cheap
	# and a crash report wants everything.
	#
	# The nops are load-delay slots and are NOT optional. psx-spx: "When
	# reading from a coprocessor register, the next opcode cannot use the
	# destination register as operand." .set noreorder means the assembler
	# will not fill them in.
	mfc0  $t0, $12
	nop
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_SR($k0)
	mfc0  $t0, $13
	nop
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_CAUSE($k0)
	mfc0  $t0, $14
	nop
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_EPC($k0)
	mfc0  $t0, $8
	nop
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_BADV($k0)
	mfc0  $t0, $6
	nop
	sw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_TAR($k0)

	# Our own stack. The faulting one may be the reason we are here.
	#
	# No gp: the monitor is built with -G0 and never addresses through it,
	# which is what lets it run with whatever gp the stopped program had.
	lui   $sp, %hi(exceptionStackTop - ARG_AREA)
	addiu $sp, $sp, %lo(exceptionStackTop - ARG_AREA)

	jal   monitorDispatch
	nop

restoreFrame:

	# The dispatcher may have changed EPC - to step over a serviced break, or
	# because a debugger moved the program counter - and any register it
	# wants to hand back, which is how PCDRV returns V0 and V1.
	lui   $k0, %hi(tinromExceptionFrame)

	lw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_HI($k0)
	lw    $t1, %lo(tinromExceptionFrame) + TINROM_EXC_LO($k0)
	mthi  $t0
	mtlo  $t1

	lw    $at, %lo(tinromExceptionFrame) + TINROM_EXC_R(1)($k0)
	lw    $v0, %lo(tinromExceptionFrame) + TINROM_EXC_R(2)($k0)
	lw    $v1, %lo(tinromExceptionFrame) + TINROM_EXC_R(3)($k0)
	lw    $a0, %lo(tinromExceptionFrame) + TINROM_EXC_R(4)($k0)
	lw    $a1, %lo(tinromExceptionFrame) + TINROM_EXC_R(5)($k0)
	lw    $a2, %lo(tinromExceptionFrame) + TINROM_EXC_R(6)($k0)
	lw    $a3, %lo(tinromExceptionFrame) + TINROM_EXC_R(7)($k0)
	lw    $t0, %lo(tinromExceptionFrame) + TINROM_EXC_R(8)($k0)
	lw    $t1, %lo(tinromExceptionFrame) + TINROM_EXC_R(9)($k0)
	lw    $t2, %lo(tinromExceptionFrame) + TINROM_EXC_R(10)($k0)
	lw    $t3, %lo(tinromExceptionFrame) + TINROM_EXC_R(11)($k0)
	lw    $t4, %lo(tinromExceptionFrame) + TINROM_EXC_R(12)($k0)
	lw    $t5, %lo(tinromExceptionFrame) + TINROM_EXC_R(13)($k0)
	lw    $t6, %lo(tinromExceptionFrame) + TINROM_EXC_R(14)($k0)
	lw    $t7, %lo(tinromExceptionFrame) + TINROM_EXC_R(15)($k0)
	lw    $s0, %lo(tinromExceptionFrame) + TINROM_EXC_R(16)($k0)
	lw    $s1, %lo(tinromExceptionFrame) + TINROM_EXC_R(17)($k0)
	lw    $s2, %lo(tinromExceptionFrame) + TINROM_EXC_R(18)($k0)
	lw    $s3, %lo(tinromExceptionFrame) + TINROM_EXC_R(19)($k0)
	lw    $s4, %lo(tinromExceptionFrame) + TINROM_EXC_R(20)($k0)
	lw    $s5, %lo(tinromExceptionFrame) + TINROM_EXC_R(21)($k0)
	lw    $s6, %lo(tinromExceptionFrame) + TINROM_EXC_R(22)($k0)
	lw    $s7, %lo(tinromExceptionFrame) + TINROM_EXC_R(23)($k0)
	lw    $t8, %lo(tinromExceptionFrame) + TINROM_EXC_R(24)($k0)
	lw    $t9, %lo(tinromExceptionFrame) + TINROM_EXC_R(25)($k0)
	lw    $gp, %lo(tinromExceptionFrame) + TINROM_EXC_R(28)($k0)
	lw    $fp, %lo(tinromExceptionFrame) + TINROM_EXC_R(30)($k0)
	lw    $ra, %lo(tinromExceptionFrame) + TINROM_EXC_R(31)($k0)

	# Leaving the handler. k1 is ours to clobber.
	lui   $k1, %hi(monitorDepth)
	sw    $zero, %lo(monitorDepth)($k1)

	# sp and the resume address last, because both are read through k0.
	lw    $sp, %lo(tinromExceptionFrame) + TINROM_EXC_R(29)($k0)
	lw    $k0, %lo(tinromExceptionFrame) + TINROM_EXC_EPC($k0)
	nop

	# rfe goes in the delay slot: it pops the interrupt-enable and
	# kernel/user stack in SR that entering the exception pushed.
	jr    $k0
	rfe

# --- an exception inside the handler -------------------------------------
#
# Something faulted while the handler was already running: tinrom's PCDRV
# code on a bad pointer from the program, a breakpoint set inside it, or a
# monitor bug. The frame still holds the context that raised the first
# exception, so that is what the debugger is given: the inner context is
# abandoned, and a resume goes back to the outer one.
#
# Two exceptions pushed SR's mode stack twice and only one rfe is left on
# the way out, so pop once here.

nested:
	lui   $sp, %hi(nestedStackTop - ARG_AREA)
	addiu $sp, $sp, %lo(nestedStackTop - ARG_AREA)

	mfc0  $a0, $13
	nop
	mfc0  $a1, $14
	nop
	jal   monitorNestedFault
	nop

	rfe
	b     restoreFrame
	nop

# --- guarded accessors ---------------------------------------------------
#
# int monitorPeek(uint32_t address, uint32_t size, uint32_t *out)
# int monitorPoke(uint32_t address, uint32_t size, uint32_t value)
#
# Return 1 on success and 0 if the access raised an exception (a bus error,
# an address error, a KSEG2 address that is not there). size is 1, 2 or 4;
# anything else is treated as 1. The guard makes the handler resume at the
# local label 9 instead of reporting a crash, so a debugger that asks for
# memory that does not exist gets an error reply instead of a dead console.
#
# Only a0-a3, v0, v1 and t0 are touched, and the handler's fast path touches
# nothing but k0 and k1, so the recovery path has nothing to restore.

.section .text.monitorPeek, "ax", @progbits
.global monitorPeek
.type monitorPeek, @function
monitorPeek:
	lui   $t0, %hi(monitorGuard)
	lui   $v1, %hi(9f)
	addiu $v1, $v1, %lo(9f)
	sw    $v1, %lo(monitorGuard)($t0)

	li    $v1, 4
	beq   $a1, $v1, 4f
	li    $v1, 2
	beq   $a1, $v1, 2f
	nop

	lbu   $v1, 0($a0)
	b     8f
	nop
2:
	lhu   $v1, 0($a0)
	b     8f
	nop
4:
	lw    $v1, 0($a0)
	nop
8:
	sw    $zero, %lo(monitorGuard)($t0)
	sw    $v1, 0($a2)
	jr    $ra
	li    $v0, 1
9:
	jr    $ra
	li    $v0, 0

.section .text.monitorPoke, "ax", @progbits
.global monitorPoke
.type monitorPoke, @function
monitorPoke:
	lui   $t0, %hi(monitorGuard)
	lui   $v1, %hi(9f)
	addiu $v1, $v1, %lo(9f)
	sw    $v1, %lo(monitorGuard)($t0)

	li    $v1, 4
	beq   $a1, $v1, 4f
	li    $v1, 2
	beq   $a1, $v1, 2f
	nop

	sb    $a2, 0($a0)
	b     8f
	nop
2:
	sh    $a2, 0($a0)
	b     8f
	nop
4:
	sw    $a2, 0($a0)
8:
	# A store can raise its bus error a few cycles late, once it leaves the
	# write buffer. Give it the chance to do so while the guard is still up.
	nop
	nop
	nop
	nop
	sw    $zero, %lo(monitorGuard)($t0)
	jr    $ra
	li    $v0, 1
9:
	jr    $ra
	li    $v0, 0

# --- asking to be halted -------------------------------------------------
#
# void monitorBreakInstruction(void)
#
# One break the dispatcher recognises by its code and steps over, so tinrom
# can halt itself in the debugger with an ordinary function call.

.section .text.monitorBreakInstruction, "ax", @progbits
.global monitorBreakInstruction
.type monitorBreakInstruction, @function
monitorBreakInstruction:
	break 0x3fe
	jr    $ra
	nop
