/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The resident monitor: what the exception handler decides, the debugger's
 * stop loop, breakpoints, stepping and the register view hosts read.
 *
 * See docs/debugger.md for the design and mon.h for the rules code in this
 * directory has to follow.
 */

#include "monitor/mon.h"
#include "ps1/registers.h"

/* --- the header tinrom finds at the base address -------------------------- */

extern uint8_t _monitorImageEnd[];
extern uint8_t _monitorBssEnd[];

TinromMonitorState monitorState;

static int  monRemove(void);
static void monClearAll(void);

__attribute__((section(".monitor.header"), used))
const TinromMonitorHeader monitorHeader = {
	.magic            = TINROM_MONITOR_MAGIC,
	.version          = TINROM_MONITOR_VERSION,
	.imageEnd         = _monitorImageEnd,
	.bssEnd           = _monitorBssEnd,
	.state            = &monitorState,
	.frame            = &tinromExceptionFrame,
	.install          = monInstallVector,
	.remove           = monRemove,
	.breakIn          = monBreakInNow,
	.nopsCommand      = monNopsCommand,
	.clearBreakpoints = monClearAll
};

/* --- installing ------------------------------------------------------------ */

/* psx-spx, "Exception Vectors". The PSX only ever uses the BEV=0 set. */
#define VECTOR_GENERAL 0x80000080u
#define VECTOR_WORDS   4

static uint32_t savedVector[VECTOR_WORDS];

/*
 * lui k0, hi / ori k0, lo / jr k0 / nop. ori rather than addiu so there is no
 * sign-extension adjustment to get wrong.
 */
int monInstallVector(void) {
	if (monitorState.installed)
		return 0;

	volatile uint32_t *vector = MON_RAM32(VECTOR_GENERAL);
	uint32_t           target = (uint32_t) (uintptr_t) tinromExceptionHandler;

	for (unsigned int i = 0; i < VECTOR_WORDS; i++)
		savedVector[i] = vector[i];

	vector[0] = 0x3c1a0000u | (target >> 16);    /* lui  $k0, %hi      */
	vector[1] = 0x375a0000u | (target & 0xffff); /* ori  $k0, $k0, %lo */
	vector[2] = 0x03400008u;                     /* jr   $k0           */
	vector[3] = 0x00000000u;                     /* nop                */

	/*
	 * psx-spx says changes at 800000xxh "seem" to reach the instruction cache
	 * without a flush and recommends flushing anyway; a vector is not the
	 * place to rely on a "seems".
	 */
	flushCache();

	monitorState.installed = 1;

	return 0;
}

static void removePatches(void);
static void restoreKernelTcbPointer(void);

static int monRemove(void) {
	if (!monitorState.installed)
		return 0;

	/*
	 * Breakpoints written into memory would reach the kernel's handler once
	 * the vector is gone, and it has no idea what they are.
	 */
	monClearAll();

	volatile uint32_t *vector = MON_RAM32(VECTOR_GENERAL);

	for (unsigned int i = 0; i < VECTOR_WORDS; i++)
		vector[i] = savedVector[i];

	restoreKernelTcbPointer();
	flushCache();

	monitorState.installed = 0;
	monitorState.armed     = 0;

	return 0;
}

/* --- memory as a debugger sees it ------------------------------------------ */

/* Main RAM is 2 MB, mirrored through the first 8 MB of the physical map. */
static bool ramOffset(uint32_t address, uint32_t *offset) {
	if (address >= 0xc0000000u)
		return false;

	uint32_t physical = address & 0x1fffffffu;

	if (physical >= 0x00800000u)
		return false;

	*offset = physical & 0x001fffffu;

	return true;
}

static bool overlaps(uint32_t start, uint32_t length, uint32_t lo, uint32_t hi) {
	return (start < hi) && ((start + length) > lo);
}

#define MONITOR_RAM_START (TINROM_MONITOR_BASE & 0x001fffffu)
#define MONITOR_RAM_END   (MONITOR_RAM_START + TINROM_MONITOR_SIZE)

/* SIO1's registers: reading its data register would eat the host's bytes. */
#define SIO1_PHYSICAL_START 0x1f801050u
#define SIO1_PHYSICAL_END   0x1f801060u
/* The one KSEG2 register there is. */
#define CACHE_CONTROL       0xfffe0130u

/*
 * The monitor's own stand-in for the kernel's thread block (see "the register
 * view" below). It is inside the monitor's window, but nops writes registers
 * by writing to it, so it is the one part of the window a host may write.
 */
#define TCB_WORDS 48

static uint32_t ownTcb[TCB_WORDS];
static bool     tcbPointerReplaced;

/* Pure: whether a host may touch this range. */
static bool allowed(uint32_t address, uint32_t length, bool write) {
	if (!length)
		return true;
	if ((address + length) < address)
		return false;

	if (address >= 0xc0000000u)
		return (address == CACHE_CONTROL) && (length == 4);

	uint32_t physical = address & 0x1fffffffu;

	if (overlaps(physical, length, SIO1_PHYSICAL_START, SIO1_PHYSICAL_END))
		return false;

	uint32_t offset;

	if (!write || !ramOffset(address, &offset))
		return true;

	/* A write that runs off the end of a mirror is not worth the edge case. */
	if ((offset + length) > 0x00200000u)
		return false;

	uint32_t tcbStart = (uint32_t) (uintptr_t) ownTcb & 0x001fffffu;

	if (
		tcbPointerReplaced && (offset >= tcbStart)
		&& ((offset + length) <= (tcbStart + sizeof(ownTcb)))
	)
		return true;

	/* The monitor itself, and the four words that lead to it. */
	if (overlaps(offset, length, MONITOR_RAM_START, MONITOR_RAM_END))
		return false;
	if (overlaps(offset, length, VECTOR_GENERAL & 0xffu, (VECTOR_GENERAL & 0xffu) + 16))
		return false;

	return true;
}

/*
 * A host writing over tinrom's image means tinrom can no longer be called:
 * PCDRV stops being forwarded to it. The monitor's own breakpoint writes do
 * not count, which is why they do not come through here.
 */
static void noteHostWrite(uint32_t address, uint32_t length) {
	uint32_t offset;

	if (!length || !ramOffset(address, &offset))
		return;

	if (overlaps(
		offset, length,
		monitorState.tinromStart & 0x001fffffu,
		monitorState.tinromEnd   & 0x001fffffu
	))
		monitorState.tinromIntact = 0;
}

bool monWritable(uint32_t address, uint32_t length) {
	if (allowed(address, length, true)) {
		noteHostWrite(address, length);

		return true;
	}

	monitorState.refused++;

	return false;
}

bool monReadSized(uint32_t address, uint32_t size, uint32_t *out) {
	if (address & (size - 1))
		return false;
	if (!allowed(address, size, false))
		return false;

	return monitorPeek(address, size, out) != 0;
}

bool monWriteSized(uint32_t address, uint32_t size, uint32_t value) {
	if (address & (size - 1))
		return false;
	if (!monWritable(address, size))
		return false;

	return monitorPoke(address, size, value) != 0;
}

bool monRead(uint32_t address, uint8_t *out, uint32_t length) {
	if (!allowed(address, length, false))
		return false;

	for (uint32_t i = 0; i < length; i++) {
		uint32_t value;

		if (!monitorPeek(address + i, 1, &value))
			return false;

		out[i] = (uint8_t) value;
	}

	return true;
}

bool monWrite(uint32_t address, const uint8_t *data, uint32_t length) {
	if (!monWritable(address, length))
		return false;

	for (uint32_t i = 0; i < length; i++) {
		if (!monitorPoke(address + i, 1, data[i]))
			return false;
	}

	return true;
}

void monCodeChanged(void) {
	flushCache();
}

/* --- the register view: the kernel's thread control block ------------------ */

/*
 * nops reads a stopped console's registers by following the kernel's pointer
 * to its thread control blocks, at 80000110h, and reading 0C0h bytes of the
 * first one - the same shape the BIOS saves a thread into. So that is where a
 * stopped frame is published, and where edits come back from on resume.
 * While tinrom owns the vector the kernel never saves a thread there itself.
 *
 * Offsets, in words:
 *   0      SR (the kernel keeps a status flag here; saved and put back)
 *   1      BadVaddr (the kernel's "mode" field; saved and put back)
 *   2+n    r(n), r0 at 2 always zero
 *   34     resume address (EPC)
 *   35/36  hi / lo
 *   37     SR, 38 cause
 *   39     non-zero if stopped in a branch delay slot (then 34 is the branch)
 */
#define KERNEL_TCB_POINTER MON_RAM32(0x80000110u)

#define TCB_R(n)  (2 + (n))
#define TCB_EPC   34
#define TCB_HI    35
#define TCB_LO    36
#define TCB_SR    37
#define TCB_CAUSE 38
#define TCB_BD    39

static volatile uint32_t *tcb;
static uint32_t          tcbKept[3];
static uint32_t          replacedTcbPointer;

static volatile uint32_t *findTcb(void) {
	uint32_t pointer = *KERNEL_TCB_POINTER;
	uint32_t offset;

	/*
	 * The kernel allocates them in its own 64 KB. Anything else - before the
	 * kernel is up, say, on the cart's pre-boot path - is not one, and the
	 * monitor substitutes its own block and points the table at it.
	 */
	if (
		!(pointer & 3) && ramOffset(pointer, &offset)
		&& (offset < 0x10000u) && ((offset + TCB_WORDS * 4) <= 0x10000u)
		&& (pointer >= 0x80000000u)
	)
		return MON_RAM32(pointer);

	if (!tcbPointerReplaced) {
		replacedTcbPointer = pointer;
		tcbPointerReplaced = true;
	}

	*KERNEL_TCB_POINTER = (uint32_t) (uintptr_t) ownTcb;

	return ownTcb;
}

static void restoreKernelTcbPointer(void) {
	if (!tcbPointerReplaced)
		return;

	*KERNEL_TCB_POINTER = replacedTcbPointer;
	tcbPointerReplaced  = false;
}

static void publishFrame(const TinromExceptionFrame *frame) {
	tcb = findTcb();

	tcbKept[0] = tcb[0];
	tcbKept[1] = tcb[1];
	tcbKept[2] = tcb[TCB_BD];

	tcb[0]      = frame->sr;
	tcb[1]      = frame->badVaddr;
	tcb[TCB_R(0)] = 0;

	for (unsigned int n = 1; n < 32; n++)
		tcb[TCB_R(n)] = frame->r[n];

	tcb[TCB_EPC]   = frame->epc;
	tcb[TCB_HI]    = frame->hi;
	tcb[TCB_LO]    = frame->lo;
	tcb[TCB_SR]    = frame->sr;
	tcb[TCB_CAUSE] = frame->cause;
	tcb[TCB_BD]    = (frame->cause & TINROM_EXC_CAUSE_BD) ? 1 : 0;
}

static void collectFrame(TinromExceptionFrame *frame) {
	for (unsigned int n = 1; n < 32; n++)
		frame->r[n] = tcb[TCB_R(n)];

	frame->epc = tcb[TCB_EPC];
	frame->hi  = tcb[TCB_HI];
	frame->lo  = tcb[TCB_LO];

	/* SR, cause and BadVaddr are not writable in any useful sense. */
	tcb[0]      = tcbKept[0];
	tcb[1]      = tcbKept[1];
	tcb[TCB_BD] = tcbKept[2];
}

uint32_t monRegGet(unsigned int regnum) {
	const TinromExceptionFrame *frame = &tinromExceptionFrame;

	if (regnum == 0)
		return 0;
	if (regnum < 32)
		return tcb[TCB_R(regnum)];

	switch (regnum) {
		case 32: return frame->sr;
		case 33: return tcb[TCB_LO];
		case 34: return tcb[TCB_HI];
		case 35: return frame->badVaddr;
		case 36: return frame->cause;
		case 37: return tcb[TCB_EPC];
		default: return 0; /* no FPU: f0-f31, fcsr and fir read as zero */
	}
}

void monRegSet(unsigned int regnum, uint32_t value) {
	if ((regnum > 0) && (regnum < 32)) {
		tcb[TCB_R(regnum)] = value;

		return;
	}

	switch (regnum) {
		case 33: tcb[TCB_LO]  = value; break;
		case 34: tcb[TCB_HI]  = value; break;
		case 37: tcb[TCB_EPC] = value; break;
		default: break;
	}
}

/* --- breakpoints ------------------------------------------------------------ */

/*
 * Breakpoints are only in memory while the target runs. They come out the
 * moment it stops, so a debugger reading code sees the real instructions,
 * and go back in on resume - all but one at the resume address, which is
 * stepped over first so it does not fire again straight away.
 */
#define PATCH_WORD (0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16))
#define PATCHES    (TINROM_MONITOR_BREAKPOINTS + 2)

typedef struct {
	uint32_t address;
	uint32_t original;
} Patch;

static Patch        patches[PATCHES];
static unsigned int patchCount;

static bool monitorStopped;

bool monStopped(void) {
	return monitorStopped;
}

/* The monitor's own code writes: refused ranges still apply, tracking does not. */
static bool patchWrite(uint32_t address, uint32_t value) {
	return !(address & 3) && allowed(address, 4, true)
		&& monitorPoke(address, 4, value);
}

/* Whether a breakpoint can go here at all: ROM and refused ranges cannot. */
static bool patchable(uint32_t address) {
	uint32_t original, check;

	if (!monReadSized(address, 4, &original))
		return false;
	if (original == PATCH_WORD)
		return true; /* one of ours already */
	if (!patchWrite(address, PATCH_WORD))
		return false;

	bool ok = monReadSized(address, 4, &check) && (check == PATCH_WORD);

	patchWrite(address, original);
	flushCache();

	return ok;
}

static bool insertPatch(uint32_t address) {
	for (unsigned int i = 0; i < patchCount; i++) {
		if (patches[i].address == address)
			return true;
	}

	if (patchCount >= PATCHES)
		return false;

	uint32_t original, check;

	if (!monReadSized(address, 4, &original))
		return false;
	if (!patchWrite(address, PATCH_WORD))
		return false;

	/* ROM takes the write without complaint and keeps its old contents. */
	if (!monReadSized(address, 4, &check) || (check != PATCH_WORD))
		return false;

	patches[patchCount].address  = address;
	patches[patchCount].original = original;
	patchCount++;

	return true;
}

static void removePatches(void) {
	for (unsigned int i = 0; i < patchCount; i++) {
		uint32_t current;

		/* If the program has since overwritten it, its version wins. */
		if (
			monReadSized(patches[i].address, 4, &current)
			&& (current == PATCH_WORD)
		)
			patchWrite(patches[i].address, patches[i].original);
	}

	patchCount = 0;
}

static void insertBreakpoints(uint32_t skip) {
	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		TinromBreakpoint *bp = &monitorState.breakpoints[i];

		if (bp->kind == TINROM_BP_FREE)
			continue;
		if ((bp->address == skip) && (bp->kind != TINROM_BP_STEP))
			continue;

		insertPatch(bp->address);
	}
}

/* Changing the set while the target runs means changing memory now. */
static void rearmIfRunning(void) {
	if (monitorStopped)
		return;

	removePatches();
	insertBreakpoints(1); /* never an instruction address */
	flushCache();
}

bool monBreakpointAdd(uint32_t address, TinromBreakpointKind kind) {
	if (address & 3)
		return false;

	TinromBreakpoint *slot = 0;

	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		TinromBreakpoint *bp = &monitorState.breakpoints[i];

		if ((bp->kind == (uint32_t) kind) && (bp->address == address))
			return true;
		if (!slot && (bp->kind == TINROM_BP_FREE))
			slot = bp;
	}

	if (!slot)
		return false;

	/*
	 * Refuse now rather than accept and silently never stop: GDB on MIPS
	 * steps by setting a breakpoint on the next instruction, and one that
	 * cannot be written (the BIOS ROM) would let the program run away.
	 */
	if (!patchable(address))
		return false;

	slot->address = address;
	slot->kind    = kind;

	/* A breakpoint means nothing unless the vector is ours. */
	monInstallVector();
	rearmIfRunning();

	return true;
}

bool monBreakpointRemove(uint32_t address, TinromBreakpointKind kind) {
	bool found = false;

	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		TinromBreakpoint *bp = &monitorState.breakpoints[i];

		if ((bp->kind == (uint32_t) kind) && (bp->address == address)) {
			bp->kind = TINROM_BP_FREE;
			found    = true;
		}
	}

	rearmIfRunning();

	return found;
}

void monBreakpointClear(TinromBreakpointKind kind) {
	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		TinromBreakpoint *bp = &monitorState.breakpoints[i];

		if ((kind == TINROM_BP_FREE) || (bp->kind == (uint32_t) kind))
			bp->kind = TINROM_BP_FREE;
	}

	rearmIfRunning();
}

static void monClearAll(void) {
	monBreakpointClear(TINROM_BP_FREE);
}

static bool breakpointAt(uint32_t address, TinromBreakpointKind kind) {
	for (unsigned int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) {
		const TinromBreakpoint *bp = &monitorState.breakpoints[i];

		if ((bp->kind == (uint32_t) kind) && (bp->address == address))
			return true;
	}

	return false;
}

/* --- stepping --------------------------------------------------------------- */

/*
 * One step is one instruction - or, for a branch or jump, the branch and its
 * delay slot together, which the CPU cannot split. Rather than evaluate a
 * condition, a conditional branch gets a breakpoint on both places it can
 * go. Code that cannot be written to (the BIOS ROM) cannot be stepped into
 * this way; the step then runs until something else stops it.
 */
static unsigned int stepTargets(uint32_t pc, uint32_t targets[2]) {
	uint32_t instruction;

	targets[0] = pc + 4;

	if (!monReadSized(pc, 4, &instruction))
		return 1;

	uint32_t opcode = instruction >> 26;
	uint32_t rs     = (instruction >> 21) & 31;
	uint32_t rt     = (instruction >> 16) & 31;
	uint32_t branch = pc + 4 + ((uint32_t) (int32_t) (int16_t) instruction << 2);
	bool     conditional = false;

	switch (opcode) {
		case 0x00: /* SPECIAL: jr, jalr */
			if (((instruction & 63) == 8) || ((instruction & 63) == 9)) {
				targets[0] = monRegGet(rs);

				return 1;
			}
			break;

		case 0x01: /* REGIMM: bltz, bgez, bltzal, bgezal */
			conditional = (rt == 0) || (rt == 1) || (rt == 16) || (rt == 17);
			break;

		case 0x02: /* j   */
		case 0x03: /* jal */
			targets[0] = ((pc + 4) & 0xf0000000u)
				| ((instruction & 0x03ffffffu) << 2);

			return 1;

		case 0x04: /* beq  */
		case 0x05: /* bne  */
		case 0x06: /* blez */
		case 0x07: /* bgtz */
			conditional = true;
			break;

		case 0x10: /* COP0-3: bcNf, bcNt */
		case 0x11:
		case 0x12:
		case 0x13:
			conditional = (rs == 8);
			break;

		default:
			break;
	}

	if (!conditional)
		return 1;

	targets[0] = branch;
	targets[1] = pc + 8;

	return (targets[0] == targets[1]) ? 1 : 2;
}

static void addStepBreakpoints(uint32_t pc) {
	uint32_t     targets[2];
	unsigned int count = stepTargets(pc, targets);

	for (unsigned int i = 0; i < count; i++)
		monBreakpointAdd(targets[i], TINROM_BP_STEP);
}

/* --- the stop loop ----------------------------------------------------------- */

static MonAction  resumeAction;
static MonResumer resumedBy;
static bool       continueAfterStep;

static volatile bool     breakInPending;
static volatile uint32_t breakInReason;
static volatile uint32_t breakInByte;

void monRequestResume(MonAction action, MonResumer by) {
	resumeAction = action;
	resumedBy    = by;
}

uint32_t monStopSignal(void) {
	return monitorState.stopSignal;
}

/* Reads the rest of a nops token whose first byte is already in hand. */
static void serviceToken(int first) {
	uint32_t token = (uint32_t) first;

	for (unsigned int i = 1; i < 4; i++) {
		int c = monSioGet(MON_TIMEOUT_SHORT);

		if (c < 0)
			return;

		token |= (uint32_t) c << (i * 8);
	}

	if (!monNopsCommand(token)) {
		monSioFlushInput();
		monSioPutWord(0x50534e55u); /* "UNSP" */
	}
}

static void stopLoop(TinromExceptionFrame *frame, uint32_t reason,
	uint32_t signal, bool requested) {
	monitorStopped    = true;
	/* Whatever the step-off was for, this stop ends it. */
	continueAfterStep = false;
	bool gdbTold      = false;

	removePatches();
	monBreakpointClear(TINROM_BP_STEP);
	flushCache();

	monitorState.halted     = 1;
	monitorState.stopReason = reason;
	monitorState.stopSignal = signal;
	monitorState.stopPc     = frame->epc;
	monitorState.stopCount++;

	publishFrame(frame);

	/*
	 * Whoever resumed the target is owed word that it stopped - unless they
	 * asked for the stop, in which case the reply to that request says so.
	 */
	if (!requested) {
		/*
		 * nops also wants the HLTD when it did not resume us itself - a hook
		 * it set while tinrom's menu ran, say - because that is how
		 * `nops /hookex` finds out. GDB only wants a stop reply it asked for.
		 */
		if (
			(resumedBy == MON_RESUMED_BY_NOPS)
			|| ((resumedBy == MON_RESUMED_BY_NONE)
				&& (monitorState.host == TINROM_HOST_NOPS))
		) {
			monNopsAnnounceStop();
		} else if (resumedBy == MON_RESUMED_BY_GDB) {
			monRspSendStop();
			gdbTold = true;
		}
	}

	resumedBy    = MON_RESUMED_BY_NONE;
	resumeAction = MON_ACTION_STAY;

	if (breakInPending) {
		breakInPending = false;

		switch (breakInReason) {
			case TINROM_BREAKIN_HALT:
				monitorState.host = TINROM_HOST_NOPS;
				monSioPutWord(0x44544c48u); /* "HLTD" */
				break;

			case TINROM_BREAKIN_RSP:
				monRspPacket();
				break;

			case TINROM_BREAKIN_INTERRUPT:
				monitorState.host = TINROM_HOST_GDB;
				monRspSendStop();
				gdbTold = true;
				break;

			default:
				break;
		}
	}

	while (resumeAction == MON_ACTION_STAY) {
		int c = monSioGetBlocking();

		if (c == '$')
			monRspPacket();
		else if ((c == '+') || (c == '-'))
			continue; /* acknowledgements; nothing is ever resent */
		else if (c == 0x03) {
			/*
			 * GDB sends Ctrl-C when it believes the target is running. If
			 * it has already been told about this stop, the Ctrl-C was sent
			 * before it heard and has been waiting in the FIFO; answering it
			 * would give GDB a second stop reply to mistake for the answer to
			 * its next packet.
			 */
			monitorState.host = TINROM_HOST_GDB;

			if (!gdbTold) {
				monRspSendStop();
				gdbTold = true;
			}
		} else
			serviceToken(c);
	}

	uint32_t pc = tcb[TCB_EPC];

	if (resumeAction == MON_ACTION_STEP) {
		addStepBreakpoints(pc);
		insertBreakpoints(pc);
	} else if (
		breakpointAt(pc, TINROM_BP_GDB) || breakpointAt(pc, TINROM_BP_HOOK)
	) {
		/* Step off it first, then put it back and carry on. */
		continueAfterStep = true;
		addStepBreakpoints(pc);
		insertBreakpoints(pc);
	} else {
		insertBreakpoints(1);
	}

	collectFrame(frame);
	flushCache();

	monitorState.halted = 0;
	monitorStopped      = false;
}

void monBreakInNow(uint32_t reason, uint32_t byte) {
	if (!monitorState.installed || monitorStopped)
		return;

	breakInReason  = reason;
	breakInByte    = byte;
	breakInPending = true;

	monitorBreakInstruction();

	breakInPending = false;
}

/* --- reporting a crash ------------------------------------------------------- */

static const char *const excNames[] = {
	"interrupt", "tlb modified", "tlb load", "tlb store",
	"address error on load", "address error on store",
	"bus error, instruction fetch", "bus error, data",
	"syscall", "breakpoint", "reserved instruction",
	"coprocessor unusable", "arithmetic overflow"
};

static uint32_t signalFor(uint32_t excCode) {
	switch (excCode) {
		case TINROM_EXC_ADEL:
		case TINROM_EXC_ADES: return MON_SIGSEGV;
		case TINROM_EXC_IBE:
		case TINROM_EXC_DBE:  return MON_SIGBUS;
		case TINROM_EXC_SYS:  return MON_SIGSYS;
		case TINROM_EXC_BP:   return MON_SIGTRAP;
		case TINROM_EXC_RI:
		case TINROM_EXC_CPU:  return MON_SIGILL;
		case TINROM_EXC_OVF:  return MON_SIGFPE;
		default:              return MON_SIGSEGV;
	}
}

/*
 * entry.S calls this for an exception taken while the handler was already
 * running. The frame still holds the outer context - the program that raised
 * the first exception - so that is what the debugger stops on; the inner one
 * is abandoned. Returns when a host resumes, and entry.S then resumes the
 * outer context.
 */
void monitorNestedFault(uint32_t cause, uint32_t epc) {
	monitorState.faultCount++;

	if (monitorState.host != TINROM_HOST_GDB) {
		monReport("exc: fault inside the handler (cause ");
		monReportHex(cause);
		monReport(" at ");
		monReportHex(epc);
		monReport("); stopping on the outer context\r\n");
	}

	stopLoop(&tinromExceptionFrame, TINROM_STOP_FAULT,
		signalFor(TINROM_EXC_CODE(cause)), false);
}

static void reportFault(const TinromExceptionFrame *frame) {
	/*
	 * A GDB session would read the text as line noise between packets; the
	 * stop reply tells it everything this does. nops shows it as TTY.
	 */
	if (monitorState.host == TINROM_HOST_GDB)
		return;

	uint32_t code = TINROM_EXC_CODE(frame->cause);

	monReport("exc: ");
	monReport((code < (sizeof(excNames) / sizeof(excNames[0])))
		? excNames[code] : "unknown");
	monReport(" at ");
	monReportHex(frame->epc);
	monReport("\r\nexc: cause ");
	monReportHex(frame->cause);
	monReport(" badv ");
	monReportHex(frame->badVaddr);
	monReport(" ra ");
	monReportHex(frame->r[31]);
	monReport("\r\nexc: halted in the debugger\r\n");
}

/*
 * tinrom's code addresses its small data through gp, and the stopped context's
 * gp is whatever that context had. The monitor itself never uses gp (-G0), so
 * swapping it around the call is safe.
 */
static int callPcdrv(TinromExceptionFrame *frame, uint32_t call) {
#ifndef MON_HOST_TEST
	uint32_t saved;

	__asm__ volatile(
		"move %0, $gp\n"
		"move $gp, %1\n"
		: "=&r"(saved) : "r"(monitorState.tinromGp)
	);

	int result = monitorState.pcdrv(frame, call);

	__asm__ volatile("move $gp, %0\n" :: "r"(saved));

	return result;
#else
	return monitorState.pcdrv(frame, call);
#endif
}

/* --- the dispatcher ------------------------------------------------------------ */

#define BREAK_MASK  0xfc00003fu
#define BREAK_MATCH 0x0000000du

/*
 * The 20-bit code field is conventionally two 10-bit halves, and which half a
 * "break 101h" lands in depends on the assembler, so both are accepted.
 */
static uint32_t breakCode(uint32_t instruction) {
	uint32_t full  = (instruction >> 6) & 0xfffffu;
	uint32_t upper = (instruction >> 16) & 0x3ffu;

	if ((full >= 0x101) && (full <= 0x107))
		return full;

	return upper;
}

/** In a branch delay slot EPC points at the branch, not at us. */
static uint32_t faultingAddress(const TinromExceptionFrame *frame) {
	return (frame->cause & TINROM_EXC_CAUSE_BD) ? (frame->epc + 4) : frame->epc;
}

/*
 * Step over the instruction just serviced. In a delay slot the branch has
 * already been decided, so resuming means going where it was going: TAR if
 * CAUSE.BT says it was taken, otherwise past the slot.
 */
static void stepOver(TinromExceptionFrame *frame) {
	if (!(frame->cause & TINROM_EXC_CAUSE_BD)) {
		frame->epc += 4;

		return;
	}

	frame->epc = (frame->cause & TINROM_EXC_CAUSE_BT)
		? frame->tar : (frame->epc + 8);
}

static void stopForFault(TinromExceptionFrame *frame) {
	monitorState.lastFault = *frame;
	monitorState.haveFault = 1;
	monitorState.faultCount++;

	uint32_t code = TINROM_EXC_CODE(frame->cause);

	if (monitorState.dbgcon) {
		volatile uint8_t *putc = MON_RAM8(0x1f802080u);

		for (const char *text = "exc: halted in the debugger\n"; *text; text++)
			*putc = (uint8_t) *text;
	}

	reportFault(frame);
	stopLoop(frame, TINROM_STOP_FAULT, signalFor(code), false);
}

void monitorDispatch(void) {
	TinromExceptionFrame *frame = &tinromExceptionFrame;
	uint32_t              code  = TINROM_EXC_CODE(frame->cause);

	/*
	 * An interrupt is not a fault. tinrom masks every source and polls, so one
	 * arriving here means something unmasked it; masking everything and
	 * resuming is right for a program that does not use interrupts.
	 */
	if (code == TINROM_EXC_INT) {
		IRQ_MASK = 0;
		monitorState.interruptCount++;

		return;
	}

	/*
	 * EnterCriticalSection and ExitCriticalSection, which most SDKs call on
	 * the way in. With every interrupt masked there is nothing to enter or
	 * leave, but refusing them would stop half of all homebrew at its first
	 * instruction.
	 */
	if (code == TINROM_EXC_SYS) {
		uint32_t function = frame->r[4];

		if ((function == 1) || (function == 2)) {
			frame->r[2] = (function == 1) ? 1 : 0;
			stepOver(frame);

			return;
		}

		stopForFault(frame);

		return;
	}

	if (code != TINROM_EXC_BP) {
		stopForFault(frame);

		return;
	}

	uint32_t at = faultingAddress(frame);

	/* One of ours: a breakpoint, a hook or the end of a step. */
	bool gdb  = breakpointAt(at, TINROM_BP_GDB);
	bool hook = breakpointAt(at, TINROM_BP_HOOK);
	bool step = breakpointAt(at, TINROM_BP_STEP);

	if (gdb || hook || step) {
		if (step && continueAfterStep && !gdb && !hook) {
			/*
			 * Stepped off a breakpoint on the way to continuing: drop the
			 * step's own breakpoints, which puts the rest back in, and go.
			 */
			continueAfterStep = false;
			monBreakpointClear(TINROM_BP_STEP);

			return;
		}

		continueAfterStep = false;

		if (hook)
			monBreakpointRemove(at, TINROM_BP_HOOK);

		stopLoop(
			frame,
			gdb ? TINROM_STOP_BREAKPOINT
				: (hook ? TINROM_STOP_HOOK : TINROM_STOP_STEP),
			MON_SIGTRAP, false
		);

		return;
	}

	uint32_t instruction;

	if (!monReadSized(at, 4, &instruction)
		|| ((instruction & BREAK_MASK) != BREAK_MATCH)) {
		stopForFault(frame);

		return;
	}

	uint32_t call = breakCode(instruction);

	if (call == TINROM_MONITOR_BREAK_ENTER) {
		stepOver(frame);

		uint32_t signal = (breakInPending
			&& (breakInReason == TINROM_BREAKIN_INTERRUPT))
			? MON_SIGINT : MON_SIGTRAP;

		stopLoop(frame, TINROM_STOP_REQUEST, signal, true);

		return;
	}

	/*
	 * PCDRV, handled by tinrom - but only while tinrom is still there. Once
	 * something has been loaded over it the call is reported to the debugger
	 * instead of jumping into whatever now occupies tinrom's old address.
	 */
	if (
		(call >= 0x101) && (call <= 0x107)
		&& monitorState.pcdrv && monitorState.tinromIntact
	) {
		if (callPcdrv(frame, call)) {
			monitorState.breakCount++;
			stepOver(frame);

			return;
		}
	}

	/*
	 * Any other break: GDB's own breakpoint written with M, a compiler trap,
	 * a program's assert. The debugger gets it with the PC on the break, which
	 * is what GDB expects of a software breakpoint.
	 */
	if (monitorState.armed) {
		stopLoop(frame, TINROM_STOP_BREAK, MON_SIGTRAP, false);

		return;
	}

	stopForFault(frame);
}
