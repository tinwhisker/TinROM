/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The debug half of the nops wire protocol: what `nops /gdb`, `/debug`,
 * `/halt`, `/cont`, `/regs` and `/hookex` send, plus the memory commands
 * they are built from.
 *
 * Clean-room: written from docs/nops-protocol.md, a description of the bytes
 * on the wire. No NOTPSXSerial source is used. See NOTICE.md.
 *
 * Called in two situations, and has to be right in both:
 *   - while tinrom runs, by tinrom's serial server, for these tokens only;
 *   - while stopped, by the stop loop, for every token.
 */

#include "monitor/mon.h"

#define TOKEN(a, b, c, d) \
	((uint32_t) (a) | ((uint32_t) (b) << 8) | ((uint32_t) (c) << 16) \
	| ((uint32_t) (d) << 24))

#define T_DEBG TOKEN('D','E','B','G')
#define T_HALT TOKEN('H','A','L','T')
#define T_CONT TOKEN('C','O','N','T')
#define T_HKEX TOKEN('H','K','E','X')
#define T_HKRD TOKEN('H','K','R','D')
#define T_HKWR TOKEN('H','K','W','R')
#define T_UNHK TOKEN('U','N','H','K')
#define T_DUMP TOKEN('D','U','M','P')
#define T_SBIN TOKEN('S','B','I','N')
#define T_PING TOKEN('P','I','N','G')
#define T_SLOW TOKEN('S','L','O','W')
#define T_MEDI TOKEN('M','E','D','I')
#define T_FAST TOKEN('F','A','S','T')
#define T_WHOA TOKEN('W','H','O','A')

#define R_OKAY TOKEN('O','K','A','Y')
#define R_HLTD TOKEN('H','L','T','D')
#define R_PONG TOKEN('P','O','N','G')
#define R_MORE TOKEN('M','O','R','E')
#define R_UNSP TOKEN('U','N','S','P')

#define DUMP_BLOCK 2048

/*
 * When a resumed target stops, nops hears an unsolicited HLTD and reads the
 * registers - but it brackets the read with HALT and CONT, because it still
 * thinks the target is running. Taken literally, that CONT would set the
 * target going again straight after reporting the stop. So the HALT that
 * answers our HLTD is remembered, and the CONT that closes the bracket is
 * acknowledged and ignored.
 */
static bool expectHaltAck;
static bool absorbCont;

void monNopsAnnounceStop(void) {
	monSioPutWord(R_HLTD);

	expectHaltAck = true;
	absorbCont    = false;
}

static void handleHalt(void) {
	if (!monStopped()) {
		/* The stop loop answers HLTD once the target is actually stopped. */
		monitorState.armed = 1;
		monInstallVector();
		monBreakInNow(TINROM_BREAKIN_HALT, 0);

		return;
	}

	/* Already stopped: nops must hear HLTD regardless, or it waits forever. */
	monSioPutWord(R_HLTD);

	if (expectHaltAck) {
		expectHaltAck = false;
		absorbCont    = true;
	}
}

static void handleCont(void) {
	monSioPutWord(R_OKAY);

	if (!monStopped())
		return;

	expectHaltAck = false;

	if (absorbCont) {
		absorbCont = false;

		return;
	}

	monRequestResume(MON_ACTION_CONTINUE, MON_RESUMED_BY_NOPS);
}

static void handleDebug(void) {
	monitorState.armed = 1;
	monInstallVector();
	monSioPutWord(R_OKAY);

	/* "Wait for a debugger": the first contact is the moment to halt. */
	if (!monStopped() && monitorState.attachOnContact) {
		monitorState.attachOnContact = 0;
		monBreakInNow(TINROM_BREAKIN_ATTACH, 0);
	}
}

static void handleHookExec(void) {
	uint32_t address;

	monSioPutWord(R_OKAY);

	if (!monSioGetWord(&address, MON_TIMEOUT_BYTE))
		return;

	monitorState.armed = 1;
	monBreakpointAdd(address & ~3u, TINROM_BP_HOOK);
}

/*
 * Read and write watchpoints need the COP0 data breakpoint, which vectors to
 * 80000040h - a vector the cart's mid-boot takeover already uses. Not done.
 * nops sends the address whatever the answer, so it is read and dropped.
 */
static void handleHookData(void) {
	uint32_t address;

	monSioPutWord(R_UNSP);
	monSioGetWord(&address, MON_TIMEOUT_SHORT);
}

static void handleUnhook(void) {
	monSioPutWord(R_OKAY);
	monBreakpointClear(TINROM_BP_HOOK);
}

/*
 * DUMP: OKAY, then the host sends address and length. The data follows, and
 * after every full 2048-byte block the host sends MORE - also after the last
 * one, if the length is an exact multiple - which has to be read before
 * anything else is sent. Then a 32-bit sum of the bytes.
 *
 * A host that does not send MORE costs one short timeout per block.
 */
static void handleDump(void) {
	uint32_t address, length;

	monSioPutWord(R_OKAY);

	if (
		!monSioGetWord(&address, MON_TIMEOUT_BYTE)
		|| !monSioGetWord(&length, MON_TIMEOUT_BYTE)
	)
		return;

	uint32_t sum = 0;

	for (uint32_t i = 0; i < length; i++) {
		uint8_t value = 0;

		/* A hole reads as zero; the framing matters more than the byte. */
		monRead(address + i, &value, 1);
		monSioPut(value);
		sum += value;

		if (!((i + 1) % DUMP_BLOCK)) {
			uint32_t more;

			monSioGetWord(&more, MON_TIMEOUT_SHORT);
		}
	}

	monSioPutWord(sum);
}

/*
 * SBIN: OKAY, then address, length and checksum, then the data, unframed.
 * That is protocol revision 1, which is what a host falls back to when the
 * console does not offer a newer one - and tinrom does not.
 *
 * A write that would land on the monitor or the vector is received and
 * dropped, so the host is not left mid-transfer.
 */
static void handleSendBinary(void) {
	uint32_t address, length, checksum;

	monSioPutWord(R_OKAY);

	if (
		!monSioGetWord(&address, MON_TIMEOUT_BYTE)
		|| !monSioGetWord(&length, MON_TIMEOUT_BYTE)
		|| !monSioGetWord(&checksum, MON_TIMEOUT_BYTE)
	)
		return;

	bool writable = monWritable(address, length);

	/*
	 * nops' /poke16 and /poke32 are SBIN with a two- or four-byte payload.
	 * Aimed at an I/O register they have to be one access of that width, not
	 * a run of byte stores.
	 */
	if (((length == 2) || (length == 4)) && !(address & (length - 1))) {
		uint32_t value = 0;

		for (uint32_t i = 0; i < length; i++) {
			int c = monSioGet(MON_TIMEOUT_BYTE);

			if (c < 0)
				return;

			value |= (uint32_t) c << (i * 8);
		}

		if (writable)
			monWriteSized(address, length, value);

		monCodeChanged();

		return;
	}

	for (uint32_t i = 0; i < length; i++) {
		int c = monSioGet(MON_TIMEOUT_BYTE);

		if (c < 0)
			break;

		if (writable) {
			uint8_t value = (uint8_t) c;

			if (!monWrite(address + i, &value, 1))
				writable = false;
		}
	}

	(void) checksum;
	monCodeChanged();
}

static void setBaud(uint32_t baud) {
	/* nops sends the rate token bare, then reopens the port at the new rate. */
	monSioSetBaud(baud);
}

int monNopsCommand(uint32_t token) {
	switch (token) {
		case T_DEBG: handleDebug();      break;
		case T_HALT: handleHalt();       break;
		case T_CONT: handleCont();       break;
		case T_HKEX: handleHookExec();   break;
		case T_HKRD:
		case T_HKWR: handleHookData();   break;
		case T_UNHK: handleUnhook();     break;
		case T_DUMP: handleDump();       break;
		case T_SBIN: handleSendBinary(); break;
		case T_PING: monSioPutWord(R_PONG); break;
		case T_SLOW: setBaud(115200);    break;
		case T_MEDI: setBaud(414720);    break;
		case T_FAST: setBaud(518400);    break;
		case T_WHOA: setBaud(921600);    break;
		default:
			return 0;
	}

	monitorState.host = TINROM_HOST_NOPS;
	monitorState.commands++;
	monitorState.lastCommand = token;

	return 1;
}
