/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Host-side tests of the IRC client's pure parts: the protocol
 * (src/comms/irc.c), turning its lines into screen text (irctext.c), the
 * cryptography (crypto.c) against the published test vectors, and the
 * password lock (vault.c). Run by scripts/test-irc.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comms/config.h"
#include "comms/crypto.h"
#include "comms/irc.h"
#include "comms/irctext.h"
#include "comms/term.h"
#include "comms/vault.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* --- fakes ------------------------------------------------------------------ */

static uint32_t noise = 12345;
uint32_t tinromTimerNoise(void) { return noise = noise * 1103515245u + 12345u; }
uint32_t tinromFrameCounter(void) { return 1000; }

/* --- helpers ---------------------------------------------------------------------- */

static bool hexIs(const uint8_t *bytes, size_t length, const char *hex) {
	char text[300];

	for (size_t i = 0; i < length; i++)
		sprintf(&text[i * 2], "%02x", bytes[i]);

	return !strcmp(text, hex);
}

static TermScreen term;
static char       sent[64][IRC_LINE_MAX];
static unsigned   sentCount;
static char       printed[64][IRC_LINE_MAX];
static IrcLine    lastLine;
static unsigned   printedCount;

static void fakeSend(void *user, const char *line) {
	(void) user;
	if (sentCount < 64)
		snprintf(sent[sentCount++], IRC_LINE_MAX, "%s", line);
}

static void fakePrint(void *user, const IrcLine *line) {
	(void) user;
	if (printedCount < 64)
		snprintf(printed[printedCount++], IRC_LINE_MAX, "%d|%s|%s|%s|%d",
			line->style, line->who ? line->who : "",
			line->where ? line->where : "", line->text, line->highlight);
	lastLine = *line;
	ircTextWrite(&term, line);
}

static void reset(void) {
	sentCount = printedCount = 0;
}

static void serverSays(IrcClient *irc, const char *text) {
	char line[IRC_LINE_MAX];

	snprintf(line, sizeof(line), "%s", text);
	ircHandleLine(irc, line);
}

static bool sentHas(const char *line) {
	for (unsigned i = 0; i < sentCount; i++)
		if (!strcmp(sent[i], line))
			return true;
	return false;
}

static bool printedHas(const char *fragment) {
	for (unsigned i = 0; i < printedCount; i++)
		if (strstr(printed[i], fragment))
			return true;
	return false;
}

/* Screen row `row` as text, trailing spaces trimmed. */
static const char *rowText(int row) {
	static char text[TERM_COLS + 1];
	TermCell   *cells = termRow(&term, row);

	for (int c = 0; c < TERM_COLS; c++)
		text[c] = (char) cells[c].ch;

	int end = TERM_COLS;
	while (end && text[end - 1] == ' ')
		end--;
	text[end] = 0;

	return text;
}

/* --- crypto ---------------------------------------------------------------- */

static void testCrypto(void) {
	uint8_t d[64];

	puts("crypto");

	/* FIPS 180-4 examples. */
	cryptoSha256("abc", 3, d);
	CHECK(hexIs(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	cryptoSha256("", 0, d);
	CHECK(hexIs(d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	cryptoSha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
	CHECK(hexIs(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

	/* RFC 4231 test cases 2 and 6 (a key longer than the block). */
	cryptoHmac("Jefe", 4, "what do ya want for nothing?", 28, d);
	CHECK(hexIs(d, 32, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
	{
		uint8_t key[131];
		memset(key, 0xaa, sizeof(key));
		const char *data = "Test Using Larger Than Block-Size Key - Hash Key First";
		cryptoHmac(key, sizeof(key), data, strlen(data), d);
		CHECK(hexIs(d, 32, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
	}

	/* PBKDF2-HMAC-SHA-256, checked against Python's hashlib. */
	cryptoPbkdf2("password", 8, "salt", 4, 1, d, 32, NULL);
	CHECK(hexIs(d, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
	cryptoPbkdf2("password", 8, "salt", 4, 4096, d, 40, NULL);
	CHECK(hexIs(d, 40, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"
		"f7ad98c1b458ce3f"));

	/* RFC 8439 2.3.2: the block function. */
	uint8_t key[32], nonce[12] = { 0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
	for (int i = 0; i < 32; i++)
		key[i] = (uint8_t) i;
	cryptoChaCha20Block(key, nonce, 1, d);
	CHECK(hexIs(d, 64, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
		"d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"));

	/* RFC 8439 2.4.2: the cipher, across a block boundary. */
	static const char plain[] = "Ladies and Gentlemen of the class of '99: "
		"If I could offer you only one tip for the future, sunscreen would be it.";
	uint8_t text[sizeof(plain) - 1];
	uint8_t nonce2[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
	memcpy(text, plain, sizeof(text));
	cryptoChaCha20(key, nonce2, 1, text, sizeof(text));
	CHECK(hexIs(text, 16, "6e2e359a2568f98041ba0728dd0d6981"));
	CHECK(hexIs(text + 96, sizeof(text) - 96, "5af90bbf74a35be6b40b8eedf2785e42874d"));
	cryptoChaCha20(key, nonce2, 1, text, sizeof(text));
	CHECK(!memcmp(text, plain, sizeof(text)));
}

/* --- the password lock ------------------------------------------------------ */

static void testVault(void) {
	static CommsConfig c;
	char out[64];

	puts("vault");
	memset(&c, 0, sizeof(c));
	CommsSecret *server = &c.bookmarks[1].irc.serverPassword;
	CommsSecret *ns     = &c.bookmarks[1].irc.nickServPassword;

	/* Off: as typed. */
	CHECK(commsSecretSet(&c, server, "hunter2"));
	CHECK(server->state == COMMS_SECRET_PLAIN);
	CHECK(commsSecretGet(&c, server, out, sizeof(out)) && !strcmp(out, "hunter2"));
	CHECK(!commsSecretSet(&c, ns, "0123456789012345678901234567890123"));
	CHECK(!commsSecretPresent(ns));

	/* On: sealed, and nothing of it readable. */
	CHECK(commsVaultEnable(&c, "correct horse", NULL) == TINROM_OK);
	CHECK(server->state == COMMS_SECRET_SEALED);
	CHECK(!memmem(server->data, sizeof(server->data), "hunter2", 7));
	CHECK(commsVaultUnlocked(&c));
	CHECK(commsSecretGet(&c, server, out, sizeof(out)) && !strcmp(out, "hunter2"));
	CHECK(commsSecretSet(&c, ns, "nickpass"));
	CHECK(ns->state == COMMS_SECRET_SEALED);

	/* Two seals of the same text differ. */
	CommsSecret again = *ns;
	CHECK(commsSecretSet(&c, ns, "nickpass"));
	CHECK(memcmp(again.data, ns->data, sizeof(ns->data)));

	/* Locked: unreadable, unwritable; a wrong password stays locked. */
	commsVaultLock();
	CHECK(!commsVaultUnlocked(&c));
	CHECK(!commsSecretGet(&c, server, out, sizeof(out)) && !out[0]);
	CHECK(!commsSecretSet(&c, server, "x"));
	CHECK(commsSecretPresent(server));
	CHECK(commsVaultUnlock(&c, "wrong horse", NULL) == TINROM_ERR_BAD_ARG);
	CHECK(!commsVaultUnlocked(&c));
	CHECK(commsVaultUnlock(&c, "correct horse", NULL) == TINROM_OK);
	CHECK(commsSecretGet(&c, ns, out, sizeof(out)) && !strcmp(out, "nickpass"));

	/* A key unlocked for one lock does not open another. */
	static CommsConfig other;
	memset(&other, 0, sizeof(other));
	commsSecretSet(&other, &other.bookmarks[1].irc.serverPassword, "abc");
	CommsConfig copyOfC = c;
	CHECK(commsVaultEnable(&other, "correct horse", NULL) == TINROM_OK);
	CHECK(!commsVaultUnlocked(&copyOfC));
	CHECK(memcmp(other.vault.salt, copyOfC.vault.salt, 16));

	/* Change, then off: back as typed. */
	CHECK(commsVaultUnlock(&c, "correct horse", NULL) == TINROM_OK);
	CHECK(commsVaultChange(&c, "battery staple", NULL) == TINROM_OK);
	commsVaultLock();
	CHECK(commsVaultUnlock(&c, "correct horse", NULL) == TINROM_ERR_BAD_ARG);
	CHECK(commsVaultUnlock(&c, "battery staple", NULL) == TINROM_OK);
	CHECK(commsVaultDisable(&c) == TINROM_OK);
	CHECK(!commsVaultEnabled(&c) && server->state == COMMS_SECRET_PLAIN);
	CHECK(commsSecretGet(&c, server, out, sizeof(out)) && !strcmp(out, "hunter2"));

	/* Forgotten: the sealed ones go, the lock goes. */
	CHECK(commsVaultEnable(&c, "x", NULL) == TINROM_OK);
	commsVaultLock();
	commsVaultForget(&c);
	CHECK(!commsVaultEnabled(&c) && !commsSecretPresent(server));
	CHECK(commsSecretSet(&c, server, "new"));
}

/* --- the protocol --------------------------------------------------------------- */

static void testParse(void) {
	IrcMessage m;
	char line[IRC_LINE_MAX];

	puts("parse");
	strcpy(line, "@time=x :nick!user@host PRIVMSG #chan :hello there");
	CHECK(ircParse(line, &m));
	CHECK(!strcmp(m.nick, "nick") && !strcmp(m.source, "nick!user@host"));
	CHECK(!strcmp(m.command, "PRIVMSG") && m.count == 2);
	CHECK(!strcmp(m.params[0], "#chan") && !strcmp(m.params[1], "hello there"));

	strcpy(line, "PING :irc.example");
	CHECK(ircParse(line, &m) && !strcmp(m.params[0], "irc.example") && !m.source[0]);

	strcpy(line, ":server 005 me A B C D E F G H I J K L M N O P Q :are supported");
	CHECK(ircParse(line, &m) && m.count == IRC_PARAMS);
	CHECK(!strcmp(m.params[IRC_PARAMS - 1], "N O P Q :are supported"));

	strcpy(line, "   ");
	CHECK(!ircParse(line, &m));

	CHECK(!ircCompare("Nick[a]", "nick{A}"));
	CHECK(ircMentions("hey snep, look", "Snep"));
	CHECK(!ircMentions("a snep is here", "snep"));
	CHECK(ircMentions("snep", "snep"));
	CHECK(!ircMentions("ssnep", "snep"));
}

static IrcClient irc;

static void connect(void) {
	ircInit(&irc, fakeSend, fakePrint, NULL);
	irc.version = "tinrom test";
	ircTextReset(&term);
	reset();

	IrcLogin login = { "snep", "snep", "Snep", "serverpw", "nspw", "#tinrom", true };
	ircLogin(&irc, &login);
}

static void testSession(void) {
	puts("session");
	connect();
	CHECK(sentCount == 3 && !strcmp(sent[0], "PASS :serverpw"));
	CHECK(!strcmp(sent[1], "NICK snep") && !strcmp(sent[2], "USER snep 0 * :Snep"));

	/* Nick taken before welcome: another is tried. */
	reset();
	serverSays(&irc, ":srv 433 * snep :Nickname is already in use");
	CHECK(sentHas("NICK snep_"));

	reset();
	serverSays(&irc, ":srv 001 snep_ :Welcome to the network");
	CHECK(irc.registered && !strcmp(irc.nick, "snep_"));
	CHECK(sentHas("PRIVMSG NickServ :IDENTIFY nspw") && sentHas("JOIN #tinrom"));
	CHECK(!irc.nickServPassword[0]);

	serverSays(&irc, ":srv 005 snep_ PREFIX=(qaohv)~&@%+ CHANMODES=beI,k,l,imnst CHANTYPES=# :are supported");
	CHECK(!strcmp(irc.prefixModes, "qaohv") && !strcmp(irc.prefixChars, "~&@%+"));
	CHECK(!strcmp(irc.modesAlways, "beIk") && !strcmp(irc.modesOnSet, "l"));

	reset();
	serverSays(&irc, "PING :12345");
	CHECK(sentHas("PONG :12345"));

	serverSays(&irc, ":snep_!u@h JOIN #tinrom");
	CHECK(irc.inChannel && !strcmp(irc.channel, "#tinrom"));
	serverSays(&irc, ":srv 332 snep_ #tinrom :All about \00304tinrom\003");
	CHECK(strstr(irc.topic, "tinrom") != NULL);
	serverSays(&irc, ":srv 353 snep_ = #tinrom :snep_ @op +voice ~@owner plain");
	serverSays(&irc, ":srv 366 snep_ #tinrom :End of /NAMES list.");
	CHECK(irc.nickCount == 5 && irc.namesDone);

	uint16_t order[IRC_NICKS];
	CHECK(ircSortedNicks(&irc, order) == 5);
	CHECK(!strcmp(irc.nicks[order[0]].name, "owner") && irc.nicks[order[0]].prefix == '~');
	CHECK(!strcmp(irc.nicks[order[1]].name, "op"));
	CHECK(!strcmp(irc.nicks[order[2]].name, "voice"));
	CHECK(!strcmp(irc.nicks[order[4]].name, "snep_"));
	CHECK(printedHas("5 in #tinrom: ~owner @op +voice plain snep_"));

	/* Joins, parts, modes, nick changes keep the list right. */
	serverSays(&irc, ":newbie!n@h JOIN #tinrom");
	CHECK(irc.nickCount == 6);
	serverSays(&irc, ":op!o@h MODE #tinrom +ov-v+l newbie voice voice 50");
	{
		unsigned n = ircSortedNicks(&irc, order);
		bool newbieOp = false, voiceNone = false;
		for (unsigned i = 0; i < n; i++) {
			IrcNick *k = &irc.nicks[order[i]];
			if (!strcmp(k->name, "newbie") && k->prefix == '@') newbieOp = true;
			if (!strcmp(k->name, "voice") && k->prefix == 0) voiceNone = true;
		}
		CHECK(newbieOp && voiceNone);
	}
	serverSays(&irc, ":newbie!n@h NICK :oldie");
	CHECK(printedHas("newbie is now oldie"));
	serverSays(&irc, ":oldie!n@h PART #tinrom :bye");
	serverSays(&irc, ":plain!p@h QUIT :gone");
	CHECK(irc.nickCount == 4);
	serverSays(&irc, ":stranger!s@h QUIT :never here");
	CHECK(!printedHas("stranger"));

	/* Messages: plain, highlighted, elsewhere, private, action. */
	reset();
	serverSays(&irc, ":op!o@h PRIVMSG #tinrom :hello all");
	CHECK(printedHas("3|op||hello all|0"));
	irc.highlights = 0;
	serverSays(&irc, ":op!o@h PRIVMSG #tinrom :snep_: you there?");
	CHECK(lastLine.highlight && irc.highlights == 1);
	serverSays(&irc, ":op!o@h PRIVMSG #other :elsewhere");
	CHECK(printedHas("|#other|elsewhere"));
	serverSays(&irc, ":friend!f@h PRIVMSG snep_ :psst");
	CHECK(lastLine.style == IRC_STYLE_PRIVATE && !strcmp(irc.lastPrivate, "friend"));
	serverSays(&irc, ":op!o@h PRIVMSG #tinrom :\001ACTION waves\001");
	CHECK(lastLine.style == IRC_STYLE_ACTION && !strcmp(lastLine.text, "waves"));

	/* CTCP: answered, but not more than once in a while. */
	reset();
	ircTick(&irc, 1000);
	serverSays(&irc, ":friend!f@h PRIVMSG snep_ :\001VERSION\001");
	CHECK(sentHas("NOTICE friend :\001VERSION tinrom test\001"));
	reset();
	serverSays(&irc, ":friend!f@h PRIVMSG snep_ :\001PING 42\001");
	CHECK(sentCount == 0);
	ircTick(&irc, 1200);
	serverSays(&irc, ":friend!f@h PRIVMSG snep_ :\001PING 42\001");
	CHECK(sentHas("NOTICE friend :\001PING 42\001"));
	reset();
	serverSays(&irc, ":friend!f@h NOTICE snep_ :\001VERSION mIRC\001");
	CHECK(printedHas("CTCP reply from friend: VERSION mIRC"));

	/* Typing. */
	reset();
	CHECK(ircInput(&irc, "hello") == IRC_INPUT_DONE);
	CHECK(sentHas("PRIVMSG #tinrom :hello") && lastLine.style == IRC_STYLE_OWN);
	ircInput(&irc, "//slash");
	CHECK(sentHas("PRIVMSG #tinrom :/slash"));
	ircInput(&irc, "/me dances");
	CHECK(sentHas("PRIVMSG #tinrom :\001ACTION dances\001"));
	ircInput(&irc, "/msg friend hi there");
	CHECK(sentHas("PRIVMSG friend :hi there") && lastLine.style == IRC_STYLE_PRIVATE_OUT);
	ircInput(&irc, "/r again");
	CHECK(sentHas("PRIVMSG friend :again"));
	ircInput(&irc, "/mode #tinrom +m");
	CHECK(sentHas("MODE #tinrom +m"));
	ircInput(&irc, "/ctcp friend version");
	CHECK(sentHas("PRIVMSG friend :\001VERSION\001"));
	ircInput(&irc, "evil\r\nQUIT");
	CHECK(sentHas("PRIVMSG #tinrom :evil  QUIT"));
	CHECK(ircInput(&irc, "/clear") == IRC_INPUT_CLEAR);

	/* One channel at a time. */
	reset();
	ircInput(&irc, "/join retro");
	CHECK(sentCount == 2 && !strcmp(sent[0], "PART #tinrom") && !strcmp(sent[1], "JOIN #retro"));
	serverSays(&irc, ":snep_!u@h PART #tinrom");
	CHECK(!irc.inChannel && irc.nickCount == 0);
	reset();
	ircInput(&irc, "hello?");
	CHECK(sentCount == 0 && lastLine.style == IRC_STYLE_ERROR);
	serverSays(&irc, ":snep_!u@h JOIN #retro");
	serverSays(&irc, ":srv 353 snep_ = #retro :snep_ Snepper snowy alice");
	serverSays(&irc, ":srv 366 snep_ #retro :End");

	/* Tab completion cycles, and addresses at the start of a line. */
	char   line[64] = "sn";
	size_t cursor   = 2;
	CHECK(ircCompleteNick(&irc, line, sizeof(line), &cursor));
	CHECK(!strcmp(line, "Snepper: ") && cursor == 9);
	CHECK(ircCompleteNick(&irc, line, sizeof(line), &cursor));
	CHECK(!strcmp(line, "snowy: "));
	strcpy(line, "hi al and");
	cursor = 5;
	CHECK(ircCompleteNick(&irc, line, sizeof(line), &cursor));
	CHECK(!strcmp(line, "hi alice and") && cursor == 8);

	/* Rename onto a stale entry: one, not two. */
	serverSays(&irc, ":alice!a@h NICK :snowy");
	{
		unsigned n = 0;
		for (unsigned i = 0; i < irc.nickCount; i++)
			if (!ircCompare(irc.nicks[i].name, "snowy")) n++;
		CHECK(n == 1);
	}

	/* Kicked: out of the channel. */
	serverSays(&irc, ":op!o@h KICK #retro snep_ :behave");
	CHECK(!irc.inChannel);

	CHECK(ircInput(&irc, "/quit") == IRC_INPUT_QUIT);
	ircDisconnected(&irc);
	CHECK(!irc.registered);

	/* Offline: nothing is shown as said. */
	reset();
	ircInput(&irc, "anyone?");
	CHECK(sentCount == 0 && lastLine.style == IRC_STYLE_ERROR);

	/* Retries that survive a server cutting nicks to nine. */
	connect();
	reset();
	serverSays(&irc, ":srv 433 * snep :in use");
	serverSays(&irc, ":srv 433 * snep_ :in use");
	CHECK(sentHas("NICK snep_") && sentHas("NICK snep1"));
	connect();
	IrcLogin longer = { "averylongnickname", "x", "x", "", "", "tinrom", true };
	ircLogin(&irc, &longer);
	reset();
	serverSays(&irc, ":srv 433 * averylong :in use");
	serverSays(&irc, ":srv 433 * averylong :in use");
	CHECK(sentHas("NICK averylon1"));
	reset();
	serverSays(&irc, ":srv 001 averylon1 :hi");
	CHECK(sentHas("JOIN #tinrom"));

	/* A nick with escapes in it must not reach the screen as escapes. */
	serverSays(&irc, ":averylon1!u@h JOIN #tinrom");
	ircTextReset(&term);
	serverSays(&irc, ":\033]0;x!u@h PRIVMSG #tinrom :evil");
	serverSays(&irc, ":bob!u@h PRIVMSG #tinrom :still here");
	CHECK(!strncmp(rowText(1), "<bob> still here", 16));
}

/* --- screen text --------------------------------------------------------------- */

static void testText(void) {
	puts("text");

	/* UTF-8 into CP437; Latin-1 when it is not UTF-8. */
	IrcCell cells[64];
	size_t  n = ircTextCells("caf\xc3\xa9 \xe2\x94\x80 \xe9t\xe9 \xf0\x9f\x98\x80", 7, false, cells, 64);
	CHECK(n == 12);
	CHECK(cells[3].ch == 0x82 && cells[5].ch == 0xc4 && cells[7].ch == 0x82);
	CHECK(cells[9].ch == 0x82 && cells[11].ch == '?');

	/* mIRC codes. */
	n = ircTextCells("\00304,02red\017 \002b\002\026r", 7, false, cells, 64);
	CHECK(n == 6 && cells[0].fg == 9 && cells[0].bg == 4);
	CHECK(cells[3].fg == 7 && cells[4].fg == 15 && cells[5].fg == 0 && cells[5].bg == 7);
	n = ircTextCells("\00399x\0031,y\003z\004ff00ffw\035i", 7, false, cells, 64);
	CHECK(n == 6 && cells[0].fg == 7 && cells[1].fg == 0 && cells[1].ch == ','
		&& cells[4].ch == 'w' && cells[5].ch == 'i');

	char plain[32];
	ircTextPlain("\0034T\xc3\xa9st\017!", plain, sizeof(plain));
	CHECK(!strcmp(plain, "T?st!"));

	/* Wrapping, with the rest indented under the message. */
	ircTextReset(&term);
	IrcLine line = { IRC_STYLE_MESSAGE, "bob", NULL,
		"the quick brown fox jumps over the lazy dog and then keeps on running "
		"for quite a while longer", false };
	ircTextWrite(&term, &line);
	CHECK(!strncmp(rowText(0), "<bob> the quick brown fox", 25));
	CHECK(!strcmp(rowText(1), "      quite a while longer"));
	CHECK(strlen(rowText(0)) <= TERM_COLS);

	IrcLine two = { IRC_STYLE_INFO, NULL, NULL,
		"averyveryveryveryveryveryveryveryveryveryveryveryveryveryveryveryveryveryverylongword"
		"thatkeepsgoing", false };
	ircTextWrite(&term, &two);
	CHECK(!strncmp(rowText(2), "-!- averyvery", 13) && strlen(rowText(2)) == TERM_COLS);
	CHECK(!strncmp(rowText(3), "    ", 4));

	/* Colours: the nick's, and the text's. */
	TermCell *row = termRow(&term, 0);
	CHECK((row[1].attr & 0x0f) != (row[6].attr & 0x0f));
	CHECK((row[6].attr & 0x0f) == 7);
}

int main(void) {
	termInit(&term, NULL, NULL);
	testCrypto();
	testVault();
	testParse();
	testSession();
	testText();

	printf(fails ? "%d failure(s)\n" : "all passed (0 failures)\n", fails);

	return fails ? 1 : 0;
}
