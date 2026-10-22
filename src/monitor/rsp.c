/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * GDB's remote serial protocol, spoken directly on SIO1, so GDB can be
 * pointed straight at the serial port:
 *
 *     (gdb) set serial baud 115200
 *     (gdb) target remote COM4
 *
 * Written from GDB's own protocol documentation ("Remote Protocol" in the GDB
 * manual). It shares the port with the nops protocol: a GDB packet always
 * starts with '$', and no nops command does.
 */

#include "monitor/mon.h"

/*
 * Advertised to GDB as PacketSize, in hex. A register dump is 576 characters
 * and a memory read comes back as two characters per byte, so this also caps
 * a single read at half of it.
 */
#define PACKET_SIZE  0x1000
#define MAX_READ     (PACKET_SIZE / 2 - 16)

static char    packet[PACKET_SIZE + 16];
static bool    noAck;
static uint8_t txSum;

void monRspReset(void) {
	noAck = false;
}

/* --- sending ------------------------------------------------------------------ */

static void txBegin(void) {
	monSioPut('$');
	txSum = 0;
}

static void txChar(char c) {
	monSioPut((uint8_t) c);
	txSum += (uint8_t) c;
}

static void txString(const char *text) {
	while (*text)
		txChar(*(text++));
}

static void txByte(uint8_t value) {
	txChar(monHexDigit(value >> 4));
	txChar(monHexDigit(value));
}

/* Registers go over the wire in target byte order: little-endian. */
static void txWord(uint32_t value) {
	for (unsigned int i = 0; i < 4; i++)
		txByte((uint8_t) (value >> (i * 8)));
}

static void txEnd(void) {
	monSioPut('#');
	monSioPut((uint8_t) monHexDigit(txSum >> 4));
	monSioPut((uint8_t) monHexDigit(txSum));
}

static void reply(const char *text) {
	txBegin();
	txString(text);
	txEnd();
}

void monRspSendStop(void) {
	txBegin();
	txChar('S');
	txByte((uint8_t) monStopSignal());
	txEnd();
}

/* --- parsing -------------------------------------------------------------------- */

static uint32_t parseWordLE(const char **text) {
	uint32_t value = 0;

	for (unsigned int i = 0; i < 4; i++) {
		int hi = monHexValue((*text)[0]);
		int lo = (hi >= 0) ? monHexValue((*text)[1]) : -1;

		if (lo < 0)
			break;

		value |= (uint32_t) ((hi << 4) | lo) << (i * 8);
		*text += 2;
	}

	return value;
}

/* "addr,length" followed by `stop`; false if malformed. */
static bool parseRange(const char **text, uint32_t *address, uint32_t *length) {
	*address = monParseHex(text);

	if (**text != ',')
		return false;

	(*text)++;
	*length = monParseHex(text);

	return true;
}

/* --- the target description ------------------------------------------------------ */

/*
 * GDB's MIPS support finds its registers by these feature and register
 * names. The numbering is the classic one - 32 general registers, then sr,
 * lo, hi, badvaddr, cause and pc, then the FPU - and is also the order of a
 * 'g' reply. The PS1 has no FPU, but GDB insists on the feature, so the
 * FPU registers exist and read as zero.
 */
static char *xmlPut(char *out, const char *text) {
	while (*text)
		*(out++) = *(text++);

	return out;
}

static char *xmlNumber(char *out, unsigned int value) {
	if (value >= 10)
		*(out++) = (char) ('0' + value / 10);

	*(out++) = (char) ('0' + value % 10);

	return out;
}

static char *xmlReg(char *out, const char *prefix, unsigned int index,
	const char *name, unsigned int regnum) {
	out = xmlPut(out, "<reg name=\"");

	if (prefix) {
		out = xmlPut(out, prefix);
		out = xmlNumber(out, index);
	} else {
		out = xmlPut(out, name);
	}

	out = xmlPut(out, "\" bitsize=\"32\" regnum=\"");
	out = xmlNumber(out, regnum);

	return xmlPut(out, "\"/>");
}

static uint32_t buildTargetXml(char *out) {
	char *start = out;

	out = xmlPut(out,
		"<?xml version=\"1.0\"?>"
		"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
		"<target version=\"1.0\">"
		"<architecture>mips:3000</architecture>"
		"<feature name=\"org.gnu.gdb.mips.cpu\">");

	for (unsigned int n = 0; n < 32; n++)
		out = xmlReg(out, "r", n, 0, n);

	out = xmlReg(out, 0, 0, "lo", 33);
	out = xmlReg(out, 0, 0, "hi", 34);
	out = xmlReg(out, 0, 0, "pc", 37);
	out = xmlPut(out, "</feature><feature name=\"org.gnu.gdb.mips.cp0\">");
	out = xmlReg(out, 0, 0, "status", 32);
	out = xmlReg(out, 0, 0, "badvaddr", 35);
	out = xmlReg(out, 0, 0, "cause", 36);
	out = xmlPut(out, "</feature><feature name=\"org.gnu.gdb.mips.fpu\">");

	for (unsigned int n = 0; n < 32; n++)
		out = xmlReg(out, "f", n, 0, 38 + n);

	out = xmlReg(out, 0, 0, "fcsr", 70);
	out = xmlReg(out, 0, 0, "fir", 71);
	out = xmlPut(out, "</feature></target>");

	return (uint32_t) (out - start);
}

/* qXfer:features:read:target.xml:offset,length */
static void handleXfer(const char *args) {
	static const char annex[] = "qXfer:features:read:target.xml:";

	if (!monStrStarts(args, annex)) {
		reply("");

		return;
	}

	const char *text = args + sizeof(annex) - 1;
	uint32_t    offset, length;

	if (!parseRange(&text, &offset, &length)) {
		reply("E01");

		return;
	}

	/* The packet has been parsed, so its buffer can hold the document. */
	uint32_t size = buildTargetXml(packet);

	if (offset >= size) {
		reply("l");

		return;
	}

	if (length > MAX_READ)
		length = MAX_READ;

	uint32_t end = offset + length;

	if (end > size)
		end = size;

	txBegin();
	txChar((end == size) ? 'l' : 'm');

	/* Nothing in the document needs escaping: no $, #, } or *. */
	for (uint32_t i = offset; i < end; i++)
		txChar(packet[i]);

	txEnd();
}

/* --- queries ------------------------------------------------------------------------ */

static void handleQuery(const char *text) {
	if (monStrStarts(text, "qSupported")) {
		reply("PacketSize=1000;qXfer:features:read+;QStartNoAckMode+");
	} else if (monStrStarts(text, "qXfer:")) {
		handleXfer(text);
	} else if (monStrStarts(text, "qAttached")) {
		/* Detaching resumes the target rather than killing it. */
		reply("1");
	} else if (monStrStarts(text, "qSymbol")) {
		reply("OK");
	} else if (monStrStarts(text, "QStartNoAckMode")) {
		reply("OK");
		noAck = true;
	} else {
		/* qC, qfThreadInfo, qOffsets, qTStatus...: not supported. */
		reply("");
	}
}

/* --- memory --------------------------------------------------------------------------- */

static void handleReadMemory(const char *text) {
	uint32_t address, length;

	if (!parseRange(&text, &address, &length)) {
		reply("E01");

		return;
	}

	if (length > MAX_READ)
		length = MAX_READ;

	/*
	 * A single aligned word or halfword is read as one access, so GDB can look
	 * at an I/O register without it being taken apart into byte reads.
	 */
	uint32_t value;

	if (((length == 4) || (length == 2)) && !(address & (length - 1))) {
		if (!monReadSized(address, length, &value)) {
			reply("E01");

			return;
		}

		txBegin();

		for (uint32_t i = 0; i < length; i++)
			txByte((uint8_t) (value >> (i * 8)));

		txEnd();

		return;
	}

	uint8_t *data = (uint8_t *) packet;
	uint32_t got  = 0;

	/* A partial read is a legal answer; only nothing at all is an error. */
	while ((got < length) && monRead(address + got, &data[got], 1))
		got++;

	if (!got && length) {
		reply("E01");

		return;
	}

	txBegin();

	for (uint32_t i = 0; i < got; i++)
		txByte(data[i]);

	txEnd();
}

static bool writeMemory(uint32_t address, const uint8_t *data, uint32_t length) {
	if (((length == 4) || (length == 2)) && !(address & (length - 1))) {
		uint32_t value = 0;

		for (uint32_t i = 0; i < length; i++)
			value |= (uint32_t) data[i] << (i * 8);

		return monWriteSized(address, length, value);
	}

	return monWrite(address, data, length);
}

/* M addr,length:hex */
static void handleWriteHex(char *text) {
	const char *cursor = text;
	uint32_t    address, length;

	if (!parseRange(&cursor, &address, &length) || (*cursor != ':')) {
		reply("E01");

		return;
	}

	cursor++;

	/* Decoded in place: the output never overtakes the input. */
	uint8_t *data = (uint8_t *) text;

	for (uint32_t i = 0; i < length; i++) {
		int hi = monHexValue(cursor[i * 2]);
		int lo = (hi >= 0) ? monHexValue(cursor[i * 2 + 1]) : -1;

		if (lo < 0) {
			reply("E01");

			return;
		}

		data[i] = (uint8_t) ((hi << 4) | lo);
	}

	bool ok = writeMemory(address, data, length);

	monCodeChanged();
	reply(ok ? "OK" : "E01");
}

/* X addr,length:binary - what "load" uses when it is available. */
static void handleWriteBinary(char *text, uint32_t packetLength) {
	const char *cursor = text;
	uint32_t    address, length;

	if (!parseRange(&cursor, &address, &length) || (*cursor != ':')) {
		reply("E01");

		return;
	}

	cursor++;

	const char *end  = text + packetLength;
	uint8_t    *data = (uint8_t *) text;
	uint32_t    got  = 0;

	/* 7Dh escapes the next byte, XORed with 20h. */
	while ((cursor < end) && (got < length)) {
		char c = *(cursor++);

		if ((c == 0x7d) && (cursor < end))
			c = (char) (*(cursor++) ^ 0x20);

		data[got++] = (uint8_t) c;
	}

	if (got != length) {
		reply("E01");

		return;
	}

	bool ok = !length || writeMemory(address, data, length);

	if (length)
		monCodeChanged();

	reply(ok ? "OK" : "E01");
}

/* --- breakpoints ------------------------------------------------------------------- */

/* Z0/Z1,addr,kind and z0/z1,addr,kind. */
static void handleBreakpoint(const char *text, bool insert) {
	char type = text[1];

	/*
	 * Z1 asks for a hardware breakpoint. A software one does the same job in
	 * RAM, which is all a hardware one would be used for here.
	 */
	if (((type != '0') && (type != '1')) || (text[2] != ',')) {
		reply("");

		return;
	}

	const char *cursor  = text + 3;
	uint32_t    address = monParseHex(&cursor);

	bool ok = insert
		? monBreakpointAdd(address, TINROM_BP_GDB)
		: (monBreakpointRemove(address, TINROM_BP_GDB), true);

	reply(ok ? "OK" : "E01");
}

/* --- the dispatcher --------------------------------------------------------------------- */

static void detach(bool acknowledge) {
	monBreakpointClear(TINROM_BP_GDB);

	if (acknowledge)
		reply("OK");

	noAck = false;
	monitorState.host = TINROM_HOST_NONE;

	/* Nobody is waiting for a stop reply any more. */
	monRequestResume(MON_ACTION_CONTINUE, MON_RESUMED_BY_NONE);
}

static void handlePacket(char *text, uint32_t length) {
	switch (text[0]) {
		case '?':
			monRspSendStop();
			break;

		case 'g':
			txBegin();

			for (unsigned int n = 0; n < MON_GDB_REGS; n++)
				txWord(monRegGet(n));

			txEnd();
			break;

		case 'G': {
			const char *cursor = text + 1;

			for (unsigned int n = 0; (n < MON_GDB_REGS) && *cursor; n++)
				monRegSet(n, parseWordLE(&cursor));

			reply("OK");
			break;
		}

		case 'p': {
			const char *cursor = text + 1;

			txBegin();
			txWord(monRegGet(monParseHex(&cursor)));
			txEnd();
			break;
		}

		case 'P': {
			const char *cursor = text + 1;
			uint32_t    regnum = monParseHex(&cursor);

			if (*cursor != '=') {
				reply("E01");
				break;
			}

			cursor++;
			monRegSet(regnum, parseWordLE(&cursor));
			reply("OK");
			break;
		}

		case 'm':
			handleReadMemory(text + 1);
			break;

		case 'M':
			handleWriteHex(text + 1);
			break;

		case 'X':
			handleWriteBinary(text + 1, length - 1);
			break;

		case 'c':
		case 's': {
			const char *cursor = text + 1;

			if (*cursor)
				monRegSet(37, monParseHex(&cursor));

			/* The stop reply goes out when the target next stops. */
			monRequestResume(
				(text[0] == 's') ? MON_ACTION_STEP : MON_ACTION_CONTINUE,
				MON_RESUMED_BY_GDB
			);
			break;
		}

		case 'Z':
		case 'z':
			handleBreakpoint(text, text[0] == 'Z');
			break;

		case 'D':
			detach(true);
			break;

		case 'k':
			/* No reply is expected to a kill; resuming is the kindest one. */
			detach(false);
			break;

		case 'H':
		case 'T':
			/* One thread, and it is alive. */
			reply("OK");
			break;

		case 'q':
		case 'Q':
			handleQuery(text);
			break;

		default:
			/* v packets, '!', and anything newer: unsupported. */
			reply("");
			break;
	}
}

void monRspPacket(void) {
	uint32_t length = 0;
	uint8_t  sum    = 0;
	bool     overflow = false;

	for (;;) {
		int c = monSioGet(MON_TIMEOUT_BYTE);

		if (c < 0)
			return; /* the host went quiet mid-packet; forget it */
		if (c == '#')
			break;
		if (c == '$') {
			/* A fresh start: GDB gave up on the last one. */
			length = 0;
			sum    = 0;
			overflow = false;
			continue;
		}

		sum += (uint8_t) c;

		if (length < PACKET_SIZE)
			packet[length++] = (char) c;
		else
			overflow = true;
	}

	int hi = monSioGet(MON_TIMEOUT_BYTE);
	int lo = monSioGet(MON_TIMEOUT_BYTE);

	if ((hi < 0) || (lo < 0))
		return;

	int expected = (monHexValue((char) hi) << 4) | monHexValue((char) lo);

	/*
	 * qSupported opens every session. A previous session that ended without
	 * D or k (GDB killed, the cable pulled) may have left no-ack mode on, and
	 * a fresh GDB that gets no '+' for its first packet never connects.
	 */
	packet[(length < PACKET_SIZE) ? length : PACKET_SIZE] = 0;

	if (monStrStarts(packet, "qSupported"))
		noAck = false;

	if (overflow || (monHexValue((char) hi) < 0) || (monHexValue((char) lo) < 0)
		|| ((uint8_t) expected != sum)) {
		if (!noAck)
			monSioPut('-');

		return;
	}

	if (!noAck)
		monSioPut('+');

	packet[length] = 0;

	monitorState.host = TINROM_HOST_GDB;
	monitorState.commands++;
	monitorState.lastCommand = (uint32_t) (uint8_t) packet[0];

	handlePacket(packet, length);
}
