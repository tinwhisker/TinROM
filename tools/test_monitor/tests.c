/* tinrom - (C) 2026 TinWhisker - MIT licensed */
/* Host-side tests of the monitor's protocol and breakpoint logic. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "monitor/mon.h"
#include "ps1/registers.h"

extern uint8_t ram[];
extern uint8_t in[]; extern size_t inHead, inTail;
extern uint8_t out[]; extern size_t outLen;
extern jmp_buf starved; extern int starvedArmed;
extern uint32_t breakInReturn, lastBaud;
extern int flushes;
void monitorDispatch(void);

#define BREAKIN_ADDR 0x801e9000u
#define TCB_ADDR     0x8000e000u
#define F (tinromExceptionFrame)

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* --- helpers --- */
static void push(const void *d, size_t n) { memcpy(&in[inTail], d, n); inTail += n; }
static void pushStr(const char *s) { push(s, strlen(s)); }
static void pushWord(uint32_t w) { push(&w, 4); }
static void pushTok(const char *t) { push(t, 4); }
static void pushPacketBody(const char *body, size_t n, int withDollar) {
	uint8_t sum = 0; char tail[4];
	if (withDollar) pushStr("$");
	for (size_t i = 0; i < n; i++) sum += (uint8_t) body[i];
	push(body, n);
	snprintf(tail, sizeof(tail), "#%02x", sum); pushStr(tail);
}
static void pushPacket(const char *body) { pushPacketBody(body, strlen(body), 1); }

static size_t outPos;
static void outReset(void) { outLen = 0; outPos = 0; }
/* Next packet in the output, skipping acks. NULL if none. Verifies checksum. */
static char pktBuf[65536];
static const char *nextPacket(void) {
	while (outPos < outLen && (out[outPos] == '+' || out[outPos] == '-')) outPos++;
	if (outPos >= outLen || out[outPos] != '$') return NULL;
	outPos++;
	size_t n = 0; uint8_t sum = 0;
	while (outPos < outLen && out[outPos] != '#') { sum += out[outPos]; pktBuf[n++] = out[outPos++]; }
	pktBuf[n] = 0;
	if (outPos + 3 > outLen) return NULL;
	unsigned int cs; sscanf((char *) &out[outPos + 1], "%2x", &cs);
	outPos += 3;
	if ((uint8_t) cs != sum) { fprintf(stderr, "  bad checksum on %s\n", pktBuf); failures++; }
	return pktBuf;
}
static int nextWordIs(const char *tok) {
	if (outPos + 4 > outLen) return 0;
	int ok = !memcmp(&out[outPos], tok, 4); outPos += 4; return ok;
}
static uint32_t nextWord(void) { uint32_t w = 0; if (outPos + 4 <= outLen) memcpy(&w, &out[outPos], 4); outPos += 4; return w; }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, &ram[a & 0x1fffff], 4); return v; }
static void wr32(uint32_t a, uint32_t v) { memcpy(&ram[a & 0x1fffff], &v, 4); }
static uint32_t hexLE(const char *h) { uint32_t v = 0; for (int i = 3; i >= 0; i--) { unsigned b; sscanf(h + i * 2, "%2x", &b); v = (v << 8) | b; } return v; }

static int pcdrvCalls;
static int pcdrvHook(TinromExceptionFrame *f, uint32_t fn) { pcdrvCalls++; f->r[2] = 0; f->r[3] = 42; return 1; }

static void setup(void) {
	memset(ram, 0, 0x200000);
	inHead = inTail = 0; outReset();
	wr32(0x80000110u, TCB_ADDR);
	wr32(TCB_ADDR, 0x4000); wr32(TCB_ADDR + 4, 0x1234);
	monitorState.tinromStart = 0x00010000; monitorState.tinromEnd = 0x00040000;
	monitorState.tinromIntact = 1; monitorState.pcdrv = pcdrvHook;
	monInstallVector();
	for (int i = 1; i < 32; i++) F.r[i] = 0x1000 + i;
	F.hi = 0xaaaa; F.lo = 0xbbbb; F.sr = 0x40000401;
}

/* Run tinrom's break-in with whatever is queued; returns 1 if the stop loop resumed. */
static int breakIn(uint32_t reason) {
	starvedArmed = 1;
	if (setjmp(starved)) { starvedArmed = 0; return 0; }
	monBreakInNow(reason, '$');
	starvedArmed = 0;
	return 1;
}
/* The target executes the instruction at addr and it raises `code`. */
static int hit(uint32_t addr, uint32_t code, uint32_t bd) {
	F.epc = addr; F.cause = (code << 2) | (bd ? TINROM_EXC_CAUSE_BD : 0);
	starvedArmed = 1;
	if (setjmp(starved)) { starvedArmed = 0; return 0; }
	monitorDispatch();
	starvedArmed = 0;
	return 1;
}

/* ---------------------------------------------------------------------- */

static void testGdbSession(void) {
	setup();
	/* The '$' was read by tinrom; the monitor reads the rest. */
	const char *first = "qSupported:multiprocess+;swbreak+;xmlRegisters=i386";
	pushPacketBody(first, strlen(first), 0);
	pushStr("+");
	pushPacket("QStartNoAckMode");
	pushStr("+");
	pushPacket("?");
	pushPacket("qXfer:features:read:target.xml:0,7f0");
	pushPacket("qXfer:features:read:target.xml:7f0,7f0");
	pushPacket("qXfer:features:read:target.xml:fe0,7f0");
	pushPacket("g");
	pushPacket("p25");
	pushPacket("p1d");
	pushPacket("m80000110,4");
	pushPacket("mfffe0000,4");       /* KSEG2 nowhere: E01 */
	pushPacket("m1f801050,4");       /* SIO1: refused */
	pushPacket("M801e8000,4:00000000"); /* the monitor: refused */
	pushPacket("M80100000,8:0d0005000c000000");
	pushPacket("m80100000,8");
	/* X with an escaped '#' (0x23 -> 7d 03) and '}' (0x7d -> 7d 5d) */
	{ const char x[] = "X80100010,4:\x7d\x03\x7d\x5d\x01\x02"; pushPacketBody(x, sizeof(x) - 1, 1); }
	pushPacket("m80100010,4");
	pushPacket("X80100000,0:");
	pushPacket("P25=00002080");      /* pc = 0x80200000? no: LE 00002080 = 0x80200000 */
	pushPacket("P25=00001080");      /* pc = 0x80100000 */
	pushPacket("P4=78563412");
	pushPacket("Z0,80100020,4");
	pushPacket("vMustReplyEmpty");
	pushPacket("qfThreadInfo");
	pushPacket("c");

	int resumed = breakIn(TINROM_BREAKIN_RSP);
	CHECK(resumed);
	CHECK(out[0] == '+');
	const char *p;
	p = nextPacket(); CHECK(p && strstr(p, "PacketSize=1000") && strstr(p, "qXfer:features:read+"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	/* no acks after no-ack mode */
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	static char xml[8192]; size_t xl = 0; int last = 0;
	for (int i = 0; i < 3; i++) {
		p = nextPacket(); CHECK(p && (p[0] == 'm' || p[0] == 'l'));
		if (!p) break;
		strcpy(xml + xl, p + 1); xl += strlen(p + 1);
		if (p[0] == 'l') { last = 1; }
	}
	CHECK(last);
	FILE *fx = fopen("target.xml", "w"); fwrite(xml, 1, xl, fx); fclose(fx);
	p = nextPacket(); CHECK(p && strlen(p) == 72 * 8);
	if (p) {
		CHECK(hexLE(p + 37 * 8) == BREAKIN_ADDR + 4);
		CHECK(hexLE(p + 0) == 0);
		CHECK(hexLE(p + 4 * 8) == 0x1004);
		CHECK(hexLE(p + 33 * 8) == 0xbbbb); /* lo */
		CHECK(hexLE(p + 34 * 8) == 0xaaaa); /* hi */
		CHECK(hexLE(p + 32 * 8) == 0x40000401); /* sr */
	}
	p = nextPacket(); CHECK(p && hexLE(p) == BREAKIN_ADDR + 4);
	p = nextPacket(); CHECK(p && hexLE(p) == 0x101d);
	p = nextPacket(); CHECK(p && hexLE(p) == TCB_ADDR);
	p = nextPacket(); CHECK(p && !strcmp(p, "E01"));
	p = nextPacket(); CHECK(p && !strcmp(p, "E01"));
	p = nextPacket(); CHECK(p && !strcmp(p, "E01"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "0d0005000c000000"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "237d0102"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	p = nextPacket(); CHECK(p && !strcmp(p, ""));
	p = nextPacket(); CHECK(p && !strcmp(p, ""));
	CHECK(nextPacket() == NULL);
	CHECK(breakInReturn == 0x80100000u);
	CHECK(F.r[4] == 0x12345678u);
	/* The kernel's own TCB words are back. */
	CHECK(rd32(TCB_ADDR) == 0x4000 && rd32(TCB_ADDR + 4) == 0x1234);
	/* The breakpoint is in memory while running. */
	CHECK(rd32(0x80100020u) == (0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16)));
	CHECK(monitorState.tinromIntact == 1); /* 0x80100000 is past tinrom */

	/* The program runs and hits it. GDB is owed a stop reply. */
	wr32(0x80100024u, 0x10400003u); /* beq v0, zero, +3 */
	outReset();
	pushPacket("g");
	pushPacket("m80100020,4");
	pushPacket("s");
	CHECK(hit(0x80100020u, TINROM_EXC_BP, 0));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	p = nextPacket(); CHECK(p && hexLE(p + 37 * 8) == 0x80100020u);
	p = nextPacket(); CHECK(p && !strcmp(p, "00000000")); /* original, not the patch */
	CHECK(nextPacket() == NULL);
	/* Stepping off a breakpoint: the step target is in, the breakpoint is not. */
	CHECK(F.epc == 0x80100020u);
	CHECK(rd32(0x80100020u) == 0);
	CHECK(rd32(0x80100024u) == (0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16)));

	/* The step lands. Next: step the branch, which gets two targets. */
	outReset();
	pushPacket("s");
	CHECK(hit(0x80100024u, TINROM_EXC_BP, 0));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	CHECK(rd32(0x80100024u) == 0x10400003u);             /* restored */
	uint32_t patch = 0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16);
	CHECK(rd32(0x80100024u + 4 + 12) == patch);           /* taken */
	CHECK(rd32(0x80100024u + 8) == patch);                /* not taken */
	CHECK(rd32(0x80100020u) == patch);                    /* bp back in */

	/* Taken branch arrives. Now continue from the Z0 address after moving pc there. */
	outReset();
	pushPacket("P25=20001080");
	pushPacket("c");
	CHECK(hit(0x80100034u, TINROM_EXC_BP, 0));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	CHECK(rd32(0x80100020u) == 0);          /* not inserted: we step off it */
	CHECK(rd32(0x80100024u) == patch);      /* the step target */
	/* The step completes silently and the breakpoint goes back in. */
	outReset();
	CHECK(hit(0x80100024u, TINROM_EXC_BP, 0));
	CHECK(outLen == 0);
	CHECK(F.epc == 0x80100024u);
	CHECK(rd32(0x80100024u) == 0x10400003u);
	CHECK(rd32(0x80100020u) == patch);

	/* Detach: breakpoints gone, running, nobody owed anything. */
	outReset();
	pushPacket("D");
	CHECK(hit(0x80100020u, TINROM_EXC_BP, 0));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	CHECK(rd32(0x80100020u) == 0);
	CHECK(monitorState.host == TINROM_HOST_NONE);
}

static void testBadChecksumAndInterrupt(void) {
	setup();
	pushStr("?#00");            /* bad checksum: '-' */
	pushPacket("?");
	pushStr("\x03");            /* Ctrl-C while stopped: stop reply again */
	pushPacket("k");
	CHECK(breakIn(TINROM_BREAKIN_RSP));
	CHECK(out[0] == '-');
	outPos = 1;
	const char *p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	CHECK(nextPacket() == NULL);

	/* Ctrl-C while running tinrom: SIGINT. */
	outReset();
	pushPacket("c");
	CHECK(breakIn(TINROM_BREAKIN_INTERRUPT));
	p = nextPacket(); CHECK(p && !strcmp(p, "S02"));
}

static void testNopsGdbFlow(void) {
	setup();
	monitorState.attachOnContact = 1;
	/* nops: DEBG while tinrom runs. Queue what nops does next: GDB's 'g'
	 * (nops thinks it is halted, so no HALT), then load: the X probe HALT,
	 * SBIN of the program, SBIN of the frame with a new pc, then CONT. */
	pushTok("DUMP"); pushWord(0x80000110u); pushWord(4);
	pushTok("DUMP"); pushWord(TCB_ADDR); pushWord(0xc0);
	pushTok("HALT");
	uint8_t prog[16] = { 0x0d, 0x00, 0x05, 0x00 }; /* break 5 at 0x80010000 */
	uint32_t sum = 0; for (int i = 0; i < 16; i++) sum += prog[i];
	pushTok("SBIN"); pushWord(0x80010000u); pushWord(16); pushWord(sum); push(prog, 16);
	pushTok("PING");
	pushTok("CONT");

	/* tinrom's serial server has read "DEBG" and hands it over. */
	int handled = 0;
	starvedArmed = 1;
	if (!setjmp(starved)) handled = monNopsCommand(0x47424544u);
	starvedArmed = 0;
	CHECK(handled);
	CHECK(nextWordIs("OKAY"));
	CHECK(nextWordIs("OKAY") && nextWord() == TCB_ADDR && nextWord() == 0x160);
	CHECK(nextWordIs("OKAY"));
	uint32_t tcb[48]; memcpy(tcb, &out[outPos], 0xc0); outPos += 0xc0;
	uint32_t s2 = 0; for (int i = 0; i < 0xc0; i++) s2 += ((uint8_t *) tcb)[i];
	CHECK(nextWord() == s2);
	CHECK(tcb[34] == BREAKIN_ADDR + 4);     /* resume address */
	CHECK(tcb[2 + 4] == 0x1004);           /* a0 */
	CHECK(tcb[0] == 0x40000401);           /* sr, shown as status */
	CHECK(tcb[35] == 0xaaaa && tcb[36] == 0xbbbb);
	CHECK(nextWordIs("HLTD"));
	CHECK(nextWordIs("OKAY"));
	CHECK(nextWordIs("PONG"));
	CHECK(nextWordIs("OKAY"));
	CHECK(outPos == outLen);
	CHECK(rd32(0x80010000u) == 0x0005000du);
	CHECK(monitorState.tinromIntact == 0);  /* loaded over tinrom */
	CHECK(monitorState.attachOnContact == 0);
	CHECK(breakInReturn == BREAKIN_ADDR + 4);

	/* Now pretend the program was started at 0x80010000 and hits its own break 5.
	 * nops is owed an unsolicited HLTD; it then does HALT, DUMP x2, CONT, which
	 * must not resume. A second CONT (GDB 'continue') does. */
	outReset();
	pushTok("HALT");
	pushTok("DUMP"); pushWord(0x80000110u); pushWord(4);
	pushTok("CONT");
	pushTok("DUMP"); pushWord(TCB_ADDR + 0x88); pushWord(4);
	/* GDB removes its break and continues. */
	uint8_t nop4[4] = { 0 };
	pushTok("SBIN"); pushWord(0x80010000u); pushWord(4); pushWord(0); push(nop4, 4);
	pushTok("CONT");
	CHECK(hit(0x80010000u, TINROM_EXC_BP, 0));
	CHECK(nextWordIs("HLTD"));             /* unsolicited */
	CHECK(nextWordIs("HLTD"));             /* the answer to HALT */
	CHECK(nextWordIs("OKAY") && nextWord() == TCB_ADDR && nextWord() == 0x160);
	CHECK(nextWordIs("OKAY"));             /* absorbed CONT */
	CHECK(nextWordIs("OKAY") && nextWord() == 0x80010000u);
	nextWord();
	CHECK(nextWordIs("OKAY"));             /* SBIN */
	CHECK(nextWordIs("OKAY"));             /* the real CONT */
	CHECK(outPos == outLen);
	CHECK(F.epc == 0x80010000u);
	CHECK(rd32(0x80010000u) == 0);
	CHECK(monitorState.stopReason == TINROM_STOP_BREAK);

}

static void testHookWhileRunning(void) {
	setup();
	/* HKEX from nops while tinrom's menu runs: the patch goes in straight away. */
	pushWord(0x80020000u);
	CHECK(monNopsCommand(0x58454b48u)); /* "HKEX" */
	CHECK(nextWordIs("OKAY"));
	uint32_t patch = 0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16);
	CHECK(rd32(0x80020000u) == patch);
	/* It fires. nops set it, so nops hears HLTD even though it did not resume
	 * us: that is how `nops /hookex` learns the hook was hit. */
	outReset();
	pushTok("UNHK");
	pushTok("CONT");
	CHECK(hit(0x80020000u, TINROM_EXC_BP, 0));
	CHECK(nextWordIs("HLTD"));
	CHECK(nextWordIs("OKAY") && nextWordIs("OKAY"));
	CHECK(rd32(0x80020000u) == 0);
	CHECK(monitorState.stopReason == TINROM_STOP_HOOK);
	for (int i = 0; i < TINROM_MONITOR_BREAKPOINTS; i++) CHECK(monitorState.breakpoints[i].kind == TINROM_BP_FREE);
}

static void testDumpBlocks(void) {
	setup();
	for (int i = 0; i < 4096; i++) ram[0x100000 + i] = (uint8_t) (i * 7);
	pushWord(0x80100000u); pushWord(4096);
	pushTok("MORE"); pushTok("MORE");
	pushTok("PING");
	CHECK(monNopsCommand(0x504d5544u)); /* DUMP */
	CHECK(nextWordIs("OKAY"));
	uint32_t sum = 0; int same = 1;
	for (int i = 0; i < 4096; i++) { if (out[outPos + i] != (uint8_t) (i * 7)) same = 0; sum += out[outPos + i]; }
	outPos += 4096;
	CHECK(same);
	CHECK(nextWord() == sum);
	CHECK(inHead == inTail - 4); /* both MOREs consumed, PING left */
}

static void testFaultsAndServices(void) {
	setup();
	/* PCDRV while intact goes to tinrom's hook and steps over. */
	wr32(0x80030000u, 0x0000000du | (0x105u << 6)); /* full-field encoding */
	CHECK(hit(0x80030000u, TINROM_EXC_BP, 0));
	CHECK(pcdrvCalls == 1 && F.epc == 0x80030004u && F.r[3] == 42);
	/* upper-half encoding too */
	wr32(0x80030008u, 0x0000000du | (0x103u << 16));
	CHECK(hit(0x80030008u, TINROM_EXC_BP, 0));
	CHECK(pcdrvCalls == 2 && F.epc == 0x8003000cu);
	/* In a delay slot of a taken branch, resume at TAR. */
	wr32(0x80030014u, 0x0000000du | (0x101u << 16));
	F.tar = 0x80031000u; F.cause = 0;
	F.epc = 0x80030010u; F.cause = (TINROM_EXC_BP << 2) | TINROM_EXC_CAUSE_BD | TINROM_EXC_CAUSE_BT;
	monitorDispatch();
	CHECK(F.epc == 0x80031000u);
	/* Syscall 1 (EnterCriticalSection): v0 = 1, next instruction. */
	F.r[4] = 1;
	CHECK(hit(0x80030020u, TINROM_EXC_SYS, 0));
	CHECK(F.r[2] == 1 && F.epc == 0x80030024u);
	/* A bus error: halts, recorded, crash text reported. */
	outReset();
	pushTok("CONT");
	CHECK(hit(0x80030030u, TINROM_EXC_DBE, 0));
	CHECK(monitorState.haveFault && monitorState.lastFault.epc == 0x80030030u);
	CHECK(memmem(out, outLen, "bus error, data", 15) != NULL);
	CHECK(monitorState.stopSignal == MON_SIGBUS);
	/* An interrupt: masked and resumed, not a stop. */
	fakeIrqMask = 0xffff; outReset();
	CHECK(hit(0x80030040u, TINROM_EXC_INT, 0));
	CHECK(fakeIrqMask == 0 && outLen == 0 && F.epc == 0x80030040u);
	/* After tinrom has been loaded over, PCDRV is a stop, not a call. */
	monitorState.tinromIntact = 0; monitorState.armed = 1; outReset();
	pushTok("CONT");
	CHECK(hit(0x80030000u, TINROM_EXC_BP, 0));
	CHECK(pcdrvCalls == 3 && monitorState.stopReason == TINROM_STOP_BREAK);
	/* Rate tokens switch without replying. */
	outReset(); CHECK(monNopsCommand(0x54534146u)); CHECK(lastBaud == 518400 && outLen == 0);
	/* Removing gives the vector back and restores nothing it did not take. */

}

static void testStepDecode(void) {
	setup();
	uint32_t patch = 0x0000000du | (TINROM_MONITOR_BREAK_PATCH << 16);
	struct { uint32_t insn; uint32_t t0, t1; } cases[] = {
		{ 0x08040010u, 0x80100040u, 0 },            /* j 0x80100040 */
		{ 0x0c040010u, 0x80100040u, 0 },            /* jal */
		{ 0x03e00008u, 0x1000u + 31, 0 },           /* jr ra -> F.r[31] */
		{ 0x1000ffffu, 0x80100000u, 0x80100008u },  /* b -1 (beq zero,zero) */
		{ 0x04110002u, 0x8010000cu, 0x80100008u },  /* bgezal zero, +2 */
		{ 0x00000000u, 0x80100004u, 0 },            /* nop */
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		setup();
		F.r[31] = 0x80101000u; if (cases[c].t0 == 0x1000u + 31) cases[c].t0 = 0x80101000u;
		memset(&ram[0x100000], 0, 0x2000);
		wr32(0x80100000u, cases[c].insn);
		outReset();
		pushPacketBody("P25=00001080", 12, 0);
		pushPacket("s");
		CHECK(breakIn(TINROM_BREAKIN_RSP));
		CHECK(rd32(cases[c].t0) == patch);
		if (cases[c].t1) CHECK(rd32(cases[c].t1) == patch);
		int n = 0; for (uint32_t a = 0x80100000u; a < 0x80102000u; a += 4) if (rd32(a) == patch) n++;
		CHECK(n == (cases[c].t1 ? 2 : 1));
	}
}


static void testReviewFixes(void) {
	setup();
	/* A breakpoint in ROM cannot be written: Z0 must say so. */
	pushPacketBody("Z0,bfc00100,4", 13, 0);
	pushPacket("Z0,80020000,4");   /* inside tinrom's image */
	pushPacket("c");
	CHECK(breakIn(TINROM_BREAKIN_RSP));
	const char *p = nextPacket(); CHECK(p && !strcmp(p, "E01"));
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	/* The monitor's own patch in tinrom's code is not "a load over tinrom". */
	CHECK(monitorState.tinromIntact == 1);
	/* A Ctrl-C GDB sent before it heard about the stop gets no second reply. */
	outReset();
	pushStr("\x03");
	pushPacket("g");
	pushPacket("D");
	CHECK(hit(0x80020000u, TINROM_EXC_BP, 0));
	p = nextPacket(); CHECK(p && !strcmp(p, "S05"));
	p = nextPacket(); CHECK(p && strlen(p) == 576);
	p = nextPacket(); CHECK(p && !strcmp(p, "OK"));
	CHECK(nextPacket() == NULL);
	/* A fresh session after one that left no-ack mode on gets its '+'. */
	outReset();
	pushPacket("QStartNoAckMode");
	pushPacket("c");
	CHECK(breakIn(TINROM_BREAKIN_RSP) || 1);
	outReset();
	pushPacketBody("qSupported", 10, 0);
	pushPacket("c");
	CHECK(breakIn(TINROM_BREAKIN_RSP));
	CHECK(outLen && out[0] == '+');
	/* nops /poke32 at an aligned address is one word store. */
	outReset();
	pushWord(0x80100100u); pushWord(4); pushWord(0); pushWord(0xdeadbeefu);
	CHECK(monNopsCommand(0x4e494253u)); /* SBIN */
	CHECK(rd32(0x80100100u) == 0xdeadbeefu);
}

static void run(const char *name, void (*fn)(void)) {
	fflush(stdout);
	pid_t pid = fork();
	if (!pid) { failures = 0; fn(); exit(failures ? 1 : 0); }
	int status; waitpid(pid, &status, 0);
	int ok = WIFEXITED(status) && !WEXITSTATUS(status);
	printf("%-28s %s\n", name, ok ? "ok" : "FAILED");
	if (!ok) failures++;
}

int main(void) {
	run("gdb session", testGdbSession);
	run("bad checksum, interrupt", testBadChecksumAndInterrupt);
	run("nops gdb flow", testNopsGdbFlow);
	run("hook while running", testHookWhileRunning);
	run("dump blocks", testDumpBlocks);
	run("faults and services", testFaultsAndServices);
	run("step decode", testStepDecode);
	run("review fixes", testReviewFixes);
	return failures ? 1 : 0;
}
