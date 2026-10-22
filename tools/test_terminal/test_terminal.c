/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Host-side tests of the terminal's pure parts and its modem: the ANSI
 * screen (src/comms/term.c), telnet negotiation (telnet.c), and the modem's
 * dialling, hanging up and carrier watching (modem.c) against a scripted
 * fake modem. Run by scripts/test-terminal.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comms/modem.h"
#include "comms/telnet.h"
#include "comms/term.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* --- a fake SIO1 and clock for modem.c --------------------------------- */

static uint8_t  rx[8192];
static size_t   rxHead, rxTail;
static char     tx[4096];
static size_t   txLength;
static uint32_t frames;
static bool     dtr = true;

static void modemSays(const char *text) {
	size_t n = strlen(text);
	memcpy(&rx[rxHead], text, n);
	rxHead += n;
}

uint32_t tinromFrameCounter(void) { return frames; }
bool     tinromVideoIsPAL(void) { return false; }
void     tinromDelayMicroseconds(int us) { (void) us; }
uint32_t tinromSioBaud(void) { return 115200; }
int      tinromSioSetBaud(uint32_t baud) { (void) baud; return 0; }
void     tinromSioBufferStart(void) { rxHead = rxTail = 0; }
void     tinromSioBufferStop(void) {}
void     tinromSioSetCtsFlow(bool on) { (void) on; }
void     tinromSioSetDtr(bool on) { dtr = on; }
bool     tinromSioDsr(void) { return true; }
void     tinromSioFlushInput(void) { rxTail = rxHead; }
void     tinromLogWrite(const char *f, ...) { (void) f; }
size_t tinromSioReceive(uint8_t *out, size_t max) {
	size_t n = 0;
	while (n < max && rxTail < rxHead)
		out[n++] = rx[rxTail++];
	return n;
}
void tinromSioWrite(const void *data, size_t length) {
	memcpy(&tx[txLength], data, length);
	txLength += length;
	tx[txLength] = 0;
}
void tinromSioWriteString(const char *text) { tinromSioWrite(text, strlen(text)); }

static void run(unsigned count) {
	for (unsigned i = 0; i < count; i++) {
		frames++;
		commsModemPoll();
	}
}

static void testModem(void) {
	CommsModemProfile profile;
	uint8_t           buffer[256];

	puts("modem");
	commsModemDefaults(&profile);
	commsModemOpen(&profile);
	CHECK(commsModemState() == COMMS_MODEM_IDLE);

	/* Dial: the init string, its OK, the dial, the adapter chatting, CONNECT. */
	commsModemDial("somebbs.domain:23");
	CHECK(!strcmp(tx, "ATE0V1\r"));
	txLength = 0;
	modemSays("ATE0V1\r\r\nOK\r\n");
	run(1);
	CHECK(!strcmp(tx, "ATDTsomebbs.domain:23\r"));
	modemSays("\r\nCONNECTING TO somebbs.domain\r\n");
	run(1);
	CHECK(commsModemState() == COMMS_MODEM_DIALLING);
	CHECK(!strcmp(commsModemStatus(), "CONNECTING TO somebbs.domain"));
	modemSays("\r\nCONNECT 9600\r\nWelcome");
	run(1);
	CHECK(commsModemState() == COMMS_MODEM_ONLINE);
	CHECK(!strcmp(commsModemStatus(), "CONNECT 9600"));

	/* What followed CONNECT is the far end's. */
	size_t n = commsModemRead(buffer, sizeof(buffer));
	CHECK(n == 7 && !memcmp(buffer, "Welcome", 7));

	/* Mid-line, it is only text. */
	modemSays("the NO CARRIER blues\r\n");
	commsModemRead(buffer, sizeof(buffer));
	CHECK(commsModemState() == COMMS_MODEM_ONLINE);

	/* At the start of a line it ends the call, split across two reads. */
	modemSays("bye\r\nNO CAR");
	n = commsModemRead(buffer, sizeof(buffer));
	CHECK(commsModemState() == COMMS_MODEM_ONLINE);
	modemSays("RIER\r\n");
	n = commsModemRead(buffer, sizeof(buffer));
	CHECK(n == 4 && commsModemState() == COMMS_MODEM_IDLE);
	CHECK(commsModemResult() == COMMS_RESULT_NO_CARRIER);

	/* BUSY. */
	txLength = 0;
	commsModemDial("x:1");
	modemSays("OK\r\n");
	run(1);
	modemSays("BUSY\r\n");
	run(1);
	CHECK(commsModemState() == COMMS_MODEM_IDLE);
	CHECK(commsModemResult() == COMMS_RESULT_BUSY);

	/* Numeric results, with echo on. */
	txLength = 0;
	commsModemDial("x:2");
	modemSays("ATE0V1\r0\r");
	run(1);
	modemSays("ATDTx:2\r1\r");
	run(1);
	CHECK(commsModemState() == COMMS_MODEM_ONLINE);

	/* Hang up the polite way: a second of quiet, +++, a second, ATH. */
	txLength = 0;
	commsModemHangup();
	run(30);
	CHECK(txLength == 0);
	run(50);
	CHECK(!strcmp(tx, "+++"));
	run(80);
	CHECK(!strcmp(tx, "+++ATH\r"));
	modemSays("OK\r\n");
	run(1);
	CHECK(commsModemState() == COMMS_MODEM_IDLE);

	/* Hang up by DTR. */
	profile.hangup = COMMS_HANGUP_DTR;
	commsModemClose();
	commsModemOpen(&profile);
	commsModemDial("x:3");
	modemSays("OK\r\nCONNECT\r\n");
	run(2);
	CHECK(commsModemState() == COMMS_MODEM_ONLINE);
	commsModemHangup();
	CHECK(!dtr);
	run(40);
	CHECK(dtr && commsModemState() == COMMS_MODEM_IDLE);

	/* Cancelling a dial. */
	txLength = 0;
	commsModemDial("x:4");
	commsModemCancel();
	CHECK(commsModemState() == COMMS_MODEM_IDLE);
	CHECK(commsModemResult() == COMMS_RESULT_CANCELLED);

	/* Direct: everything passes, and a hand-typed dial goes online. */
	commsModemDirect();
	modemSays("OK\r\nCONNECT 2400\r\nhi");
	n = commsModemRead(buffer, sizeof(buffer));
	CHECK(n == 20 && commsModemState() == COMMS_MODEM_ONLINE);
	modemSays("more");
	n = commsModemRead(buffer, sizeof(buffer));
	CHECK(n == 4);
	commsModemClose();

	CHECK(commsModemParseResult("OKAY") == COMMS_RESULT_NONE);
	CHECK(commsModemParseResult("NO DIALTONE") == COMMS_RESULT_NO_DIALTONE);
	CHECK(commsModemParseResult("12") == COMMS_RESULT_CONNECT);
	CHECK(commsModemParseResult("HANGUP") == COMMS_RESULT_OK);
}

/* --- telnet ---------------------------------------------------------------- */

static uint8_t sent[256];
static size_t  sentLength;

static void telnetSend(const uint8_t *data, size_t length) {
	memcpy(&sent[sentLength], data, length);
	sentLength += length;
}

static void testTelnet(void) {
	CommsTelnet t;

	puts("telnet");
	commsTelnetInit(&t, telnetSend, 80, 25);

	uint8_t in1[] = { 'a', 255, 253, 24, 'b', 255, 255, 'c' };
	size_t  n     = commsTelnetReceive(&t, in1, sizeof(in1));
	CHECK(n == 4 && !memcmp(in1, "ab\xff" "c", 4));
	CHECK(sentLength == 3 && sent[0] == 255 && sent[1] == 251 && sent[2] == 24);

	sentLength = 0;
	uint8_t in2[] = { 255, 250, 24, 1, 255, 240 };
	n = commsTelnetReceive(&t, in2, sizeof(in2));
	CHECK(n == 0);
	CHECK(sentLength == 10 && !memcmp(&sent[4], "ANSI", 4) && sent[3] == 0);

	/* WILL ECHO is welcome, once; the second asks for nothing. */
	sentLength = 0;
	uint8_t in3[] = { 255, 251, 1, 255, 251, 1 };
	commsTelnetReceive(&t, in3, sizeof(in3));
	CHECK(sentLength == 3 && sent[1] == 253 && sent[2] == 1);

	/* NAWS: agreed, and the size follows. */
	sentLength = 0;
	uint8_t in4[] = { 255, 253, 31 };
	commsTelnetReceive(&t, in4, sizeof(in4));
	CHECK(sentLength == 12 && sent[1] == 251 && sent[7] == 80 && sent[9] == 25);

	/* Something unknown is refused, and only once. */
	sentLength = 0;
	uint8_t in5[] = { 255, 253, 5, 255, 253, 5, 255, 251, 33 };
	commsTelnetReceive(&t, in5, sizeof(in5));
	CHECK(sentLength == 6 && sent[1] == 252 && sent[4] == 254);

	/* A sequence split across reads. */
	sentLength = 0;
	uint8_t in6[] = { 'x', 255 };
	uint8_t in7[] = { 253, 3, 'y' };
	CHECK(commsTelnetReceive(&t, in6, 2) == 1);
	CHECK(commsTelnetReceive(&t, in7, 3) == 1 && in7[0] == 'y');
	CHECK(sentLength == 3 && sent[1] == 251 && sent[2] == 3);

	/* Sending: IAC doubled, a lone CR followed by NUL. */
	sentLength = 0;
	uint8_t out[] = { 'a', 255, '\r' };
	commsTelnetSendInput(&t, out, 3);
	CHECK(sentLength == 5 && sent[1] == 255 && sent[2] == 255
		&& sent[3] == '\r' && sent[4] == 0);
}

/* --- the screen ------------------------------------------------------------ */

static char replyText[64];

static void termReply(void *user, const uint8_t *data, size_t length) {
	(void) user;
	memcpy(replyText, data, length);
	replyText[length] = 0;
}

static TermScreen term;

static void feed(const char *text) {
	termFeed(&term, (const uint8_t *) text, strlen(text));
}

static bool rowIs(int row, const char *text) {
	for (size_t i = 0; text[i]; i++)
		if (termRow(&term, row)[i].ch != (uint8_t) text[i])
			return false;
	return true;
}

static void testTerm(void) {
	puts("ansi");
	termInit(&term, termReply, NULL);

	feed("Hello\r\nWorld");
	CHECK(rowIs(0, "Hello") && rowIs(1, "World"));
	CHECK(term.cx == 5 && term.cy == 1);

	feed("\033[2J");
	CHECK(term.cx == 0 && term.cy == 0 && termRow(&term, 0)[0].ch == ' ');

	feed("\033[5;10HX");
	CHECK(termRow(&term, 4)[9].ch == 'X' && term.cx == 10 && term.cy == 4);

	/* Colours: bold red on blue, bold making it bright. */
	feed("\033[1;31;44mA\033[0mB");
	CHECK(termRow(&term, 4)[10].attr == (9 | (4 << 4)));
	CHECK(termRow(&term, 4)[11].attr == 7);

	/* Reverse, and the background used by erasing. */
	feed("\033[7mR\033[0;42m\033[K");
	CHECK(termRow(&term, 4)[12].attr == (0 | (7 << 4)));
	CHECK(termRow(&term, 4)[79].attr >> 4 == 2);

	/* Autowrap at column 80, with the wrap deferred until the next glyph. */
	feed("\033[0m\033[2J");
	for (int i = 0; i < 80; i++)
		feed("x");
	CHECK(term.cy == 0 && term.cx == 79 && term.wrapPending);
	feed("y");
	CHECK(term.cy == 1 && termRow(&term, 1)[0].ch == 'y');

	/* Scrolling off the top goes into the history. */
	feed("\033[2J");
	for (int i = 0; i < 30; i++) {
		char line[16];
		snprintf(line, sizeof(line), "line %d\r\n", i);
		feed(line);
	}
	CHECK(term.historyCount == 6);
	CHECK(termLine(&term, TERM_ROWS)->ch == 'l');
	CHECK(rowIs(0, "line 6"));

	/* The cursor position report. */
	feed("\033[3;4H\033[6n");
	CHECK(!strcmp(replyText, "\033[3;4R"));

	/* Save and restore, both spellings. */
	feed("\033[s\033[10;10H\033[u");
	CHECK(term.cy == 2 && term.cx == 3);
	feed("\0337\033[1;1H\0338");
	CHECK(term.cy == 2 && term.cx == 3);

	/* Insert and delete lines, inside a scroll region. */
	feed("\033[2J\033[1;1Ha\r\nb\r\nc\r\nd");
	feed("\033[2;3r\033[2;1H\033[L");
	CHECK(rowIs(0, "a") && termRow(&term, 1)[0].ch == ' ' && rowIs(2, "b")
		&& rowIs(3, "d"));
	feed("\033[r");

	/* Insert and delete characters. */
	feed("\033[2J\033[1;1Habcdef\033[1;3H\033[2P");
	CHECK(rowIs(0, "abef"));
	feed("\033[1;2H\033[1@");
	CHECK(rowIs(0, "a bef"));

	/* CP437 bytes print as themselves. */
	feed("\033[2J\xc9\xcd\xbb");
	CHECK(termRow(&term, 0)[0].ch == 0xc9 && termRow(&term, 0)[2].ch == 0xbb);

	/* Form feed clears; tab stops every eight. */
	feed("x\f\tT");
	CHECK(termRow(&term, 0)[0].ch == ' ' && termRow(&term, 0)[8].ch == 'T');

	/* OSC titles and charset switches vanish. */
	feed("\033]0;title\007\033(BZ");
	CHECK(termRow(&term, 0)[9].ch == 'Z');

	/* 256-colour SGR lands on the nearest of the sixteen. */
	feed("\033[38;5;196mQ");
	CHECK((termRow(&term, 0)[10].attr & 15) == 9);

	/* A parameter past the end, and garbage, do not crash or wrap. */
	feed("\033[999;999H\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18m");
	CHECK(term.cx == 79 && term.cy == 24);

	/* A huge count is the region's height, and is quick. */
	feed("\033[2J\033[1;1Htop\033[99999L");
	CHECK(termRow(&term, 0)[0].ch == ' ');
	unsigned before = term.historyCount;
	feed("\033[1;1H\033[99999M");
	CHECK(term.historyCount == before);
	feed("\033[99999S");
	CHECK(term.historyCount == (before + 25 > TERM_HISTORY ? TERM_HISTORY : before + 25));

	feed("\033[?25l");
	CHECK(!term.cursorVisible);
	feed("\033c");
	CHECK(term.cursorVisible && term.cx == 0);
}

int main(void) {
	testTerm();
	testTelnet();
	testModem();
	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);
	return fails ? 1 : 0;
}
