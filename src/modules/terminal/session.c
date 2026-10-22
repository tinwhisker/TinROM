/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * A terminal session: the 80x25 screen, the modem behind it, and the pad
 * (or the on-screen keyboard) in front of it.
 *
 * The pad, with the keyboard put away:
 *
 *   D-pad      the arrow keys          Cross     Enter
 *   Square     backspace               Circle    Esc
 *   Triangle   the on-screen keyboard  Select    send a phrase
 *   L1 / R1    scroll back / forward   L2 / R2   a page back / forward
 *   Start      the session menu: hang up, dial again, leave
 *
 * With it out, keyboard.h has the keys, and Circle puts it away.
 */

#include <stdio.h>
#include "comms/config.h"
#include "comms/modem.h"
#include "comms/telnet.h"
#include "comms/term.h"
#include "comms/termview.h"
#include "hal/uart_sio.h"
#include "libc/string.h"
#include "modules/modules.h"
#include "modules/terminal/terminal.h"
#include "ui/keyboard.h"

/* Big enough that a screenful of ANSI arrives in a frame or two. */
#define READ_CHUNK      512
#define READS_PER_FRAME 8

/* The screen is termview's one, which the IRC client shares. */
#define term (*termViewScreen())
static CommsTelnet telnet;

static struct {
	TinromContext *ctx;
	const char    *name;
	char           target[COMMS_TARGET_MAX];
	bool           direct;
	bool           useTelnet;
	bool           keyboardOpen;
	UIKeyboard     keyboard;
	unsigned       back;
	CommsModemState lastState;
	uint32_t       bellUntil;
} s;

/* --- sending ----------------------------------------------------------- */

static void sendRaw(const uint8_t *data, size_t length) {
	commsModemWrite(data, length);
}

/* The terminal's answers to the far end's queries go straight back. */
static void termReply(void *user, const uint8_t *data, size_t length) {
	(void) user;
	sendRaw(data, length);
}

static void sendBytes(const uint8_t *data, size_t length) {
	if (s.useTelnet && (commsModemState() == COMMS_MODEM_ONLINE))
		commsTelnetSendInput(&telnet, data, length);
	else
		sendRaw(data, length);
}

static void sendText(const char *text) {
	sendBytes((const uint8_t *) text, strlen(text));
}

static void sendKey(int key) {
	switch (key) {
		case UI_KEY_NONE:  return;
		case UI_KEY_UP:    sendText("\033[A"); return;
		case UI_KEY_DOWN:  sendText("\033[B"); return;
		case UI_KEY_RIGHT: sendText("\033[C"); return;
		case UI_KEY_LEFT:  sendText("\033[D"); return;

		case UI_KEY_NUL: {
			static const uint8_t nul = 0;

			sendBytes(&nul, 1);
			return;
		}

		default: {
			uint8_t byte = (uint8_t) key;

			sendBytes(&byte, 1);

			/* Back to the live screen when typing, as a terminal does. */
			s.back = 0;
		}
	}
}

/* --- receiving --------------------------------------------------------- */

static void receive(void) {
	static uint8_t buffer[READ_CHUNK];

	for (unsigned i = 0; i < READS_PER_FRAME; i++) {
		/* Asked first: the read that ends in NO CARRIER was still online. */
		bool   online = (commsModemState() == COMMS_MODEM_ONLINE);
		size_t count  = commsModemRead(buffer, sizeof(buffer));

		if (!count)
			break;

		if (s.useTelnet && online)
			count = commsTelnetReceive(&telnet, buffer, count);

		termFeed(&term, buffer, count);
	}

	if (term.bell) {
		term.bell   = false;
		s.bellUntil = tinromFrameCounter() + 8;
	}
}

/* Tell the screen what the modem did, since it is not on the line. */
static void noteStateChange(void) {
	CommsModemState state = commsModemState();

	if (state == s.lastState)
		return;

	char line[80];

	switch (state) {
		case COMMS_MODEM_ONLINE:
			snprintf(line, sizeof(line), "\r\n\033[0;1;32m[%s]\033[0m\r\n",
				commsModemStatus());
			termPrint(&term, line);

			if (s.useTelnet)
				commsTelnetInit(&telnet, sendRaw, TERM_COLS, TERM_ROWS);
			break;

		case COMMS_MODEM_IDLE:
			snprintf(line, sizeof(line), "\r\n\033[0;1;33m[%s]\033[0m\r\n",
				commsModemStatus());
			termPrint(&term, line);

			/* Typing at the modem goes on after a hand-dialled call ends. */
			if (s.direct)
				commsModemDirect();
			break;

		default:
			break;
	}

	s.lastState = commsModemState();
}

/* --- drawing ------------------------------------------------------------ */

static int termTop(const UIContext *ui) {
	if (!s.keyboardOpen)
		return 0;

	/* Keep the cursor's row in sight above the keyboard. */
	int keyboardTop = (int) ui->height - UI_KEYBOARD_HEIGHT;
	int cursorEnd   = (term.cy + 1) * TERM_VIEW_CELL_H;
	int top         = keyboardTop - 2 - cursorEnd;

	return (top < 0) ? top : 0;
}

static const char *stateText(void) {
	switch (commsModemState()) {
		case COMMS_MODEM_DIALLING:   return commsModemStatus();
		case COMMS_MODEM_ONLINE:     return "Online";
		case COMMS_MODEM_HANGING_UP: return "Hanging up...";
		case COMMS_MODEM_DIRECT:     return "Modem";
		default:                     return "Offline";
	}
}

static void drawStatus(UIContext *ui) {
	int  y = TERM_VIEW_HEIGHT + 3;
	char line[96];

	if (s.keyboardOpen || (y + 12 > (int) ui->height))
		return;

	uiFillRect(ui, 0, TERM_VIEW_HEIGHT, (int) ui->width,
		(int) ui->height - TERM_VIEW_HEIGHT,
		(tinromFrameCounter() < s.bellUntil) ? TINROM_COLOR_HEADER
			: TINROM_COLOR_PANEL);

	snprintf(line, sizeof(line), "%s  %u baud", s.name,
		(unsigned int) tinromSioBaud());
	uiTextClipped(ui, 6, y, 200, TINROM_COLOR_TEXT, line);

	uint32_t lost = tinromSioOverruns();

	if (lost) {
		snprintf(line, sizeof(line), "lost %u", (unsigned int) lost);
		uiTextColored(ui, 210, y, TINROM_COLOR_WARNING, line);
	}

	const char *state = stateText();

	uiTextColored(ui, (int) ui->width - 6 - uiTextWidth(state), y,
		(commsModemState() == COMMS_MODEM_ONLINE) ? TINROM_COLOR_OK
			: TINROM_COLOR_DIM, state);

	if (s.back) {
		snprintf(line, sizeof(line), "Back %u lines - R1 forward", s.back);
		uiTextColored(ui, 6, y + 12, TINROM_COLOR_ACCENT, line);
	} else {
		uiTextColored(ui, 6, y + 12, TINROM_COLOR_DIM,
			"/\\ keys  Sel phrase  O Esc  Start menu");
	}
}

static void drawTerminal(void) {
	UIContext *ui = &s.ctx->ui;

	termViewDraw(ui, &term, 0, termTop(ui), s.back,
		(tinromFrameCounter() / 16) & 1);
}

static void drawScreen(void *unused) {
	(void) unused;

	UIContext *ui = &s.ctx->ui;

	drawTerminal();
	drawStatus(ui);

	if (s.keyboardOpen)
		uiKeyboardDraw(&s.keyboard, ui, (int) ui->height - UI_KEYBOARD_HEIGHT);
}

/*
 * The frames a popup draws: the terminal behind it, and the popup's own
 * chrome and summary line where the status line would be.
 */
static void drawBehind(void *unused) {
	(void) unused;

	uiSetChromeVisible(&s.ctx->ui, true);
	drawTerminal();
}

/* --- menus --------------------------------------------------------------- */

static void sendPhrase(void) {
	CommsConfig *config = commsConfig();
	UIMenuItem   items[COMMS_PHRASES];
	size_t       count = 0;
	static size_t cursor;

	for (unsigned i = 0; i < COMMS_PHRASES; i++) {
		if (!config->phrases[i][0])
			continue;

		items[count++] = (UIMenuItem) { config->phrases[i],
			"Sent with Enter after it", true, i };
	}

	if (!count)
		return;

	size_t choice = uiPopupRunAt(s.ctx, "Send", items, count, 160, 40,
		drawBehind, NULL, &cursor);

	if (choice == UI_MENU_CANCELLED)
		return;

	sendText(config->phrases[items[choice].tag]);
	sendText("\r");
}

static void waitForHangup(void) {
	UIContext *ui = &s.ctx->ui;

	commsModemHangup();

	/* At most five seconds of "Hanging up...". */
	for (unsigned frame = 0; (frame < 300)
		&& (commsModemState() == COMMS_MODEM_HANGING_UP); frame++) {
		commsModemPoll();
		uiBeginFrame(ui);
		uiSetChromeVisible(ui, false);
		termViewUpdate(&term, s.back);
		drawScreen(NULL);
		uiEndFrame(ui);
	}
}

/* Returns true to leave the session. */
static bool sessionMenu(void) {
	CommsModemState state = commsModemState();
	bool online  = (state == COMMS_MODEM_ONLINE);
	bool canDial = !s.direct && (state == COMMS_MODEM_IDLE);

	UIMenuItem items[] = {
		{ "Hang up",      "End the call; the screen stays", online, 0 },
		{ "Dial again",   "Call the same address", canDial, 1 },
		{ "Send a phrase", "One of the saved lines", true, 2 },
		{ "Send Ctrl-C",  "Interrupt whatever is running", true, 3 },
		{ "Clear screen", "Locally; nothing is sent", true, 4 },
		{ "Leave",        online ? "Hangs up first" : "Back to the list",
			true, 5 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(s.ctx, "Session", items,
		sizeof(items) / sizeof(items[0]), 160, 40, drawBehind, NULL, &cursor);

	switch (choice) {
		case 0: waitForHangup(); break;

		case 1:
			termPrint(&term, "\r\n\033[0;1;36m[Dialling]\033[0m\r\n");
			commsModemDial(s.target);
			break;

		case 2: sendPhrase(); break;
		case 3: sendText("\003"); break;

		case 4:
			termReset(&term);
			termViewInvalidate();
			break;

		case 5:
			if (online)
				waitForHangup();
			else if (state == COMMS_MODEM_DIALLING)
				commsModemCancel();

			return true;
	}

	return false;
}

/* --- the session ---------------------------------------------------------- */

static void scroll(int lines) {
	int back = (int) s.back + lines;

	if (back < 0)
		back = 0;
	if (back > (int) term.historyCount)
		back = (int) term.historyCount;

	s.back = (unsigned) back;
}

/* What the pad asked for that needs a popup: done once the frame is out. */
enum { WANT_NOTHING = 0, WANT_PHRASE, WANT_MENU };

static int handlePad(uint16_t pressed, uint16_t repeated) {
	if (s.keyboardOpen) {
		if (pressed & TINROM_PAD_CIRCLE) {
			s.keyboardOpen = false;

			return WANT_NOTHING;
		}

		sendKey(uiKeyboardUpdate(&s.keyboard, pressed, repeated));

		return WANT_NOTHING;
	}

	if (repeated & TINROM_PAD_UP)    sendKey(UI_KEY_UP);
	if (repeated & TINROM_PAD_DOWN)  sendKey(UI_KEY_DOWN);
	if (repeated & TINROM_PAD_LEFT)  sendKey(UI_KEY_LEFT);
	if (repeated & TINROM_PAD_RIGHT) sendKey(UI_KEY_RIGHT);
	if (repeated & TINROM_PAD_CROSS) sendKey('\r');
	if (repeated & TINROM_PAD_SQUARE) sendKey('\b');
	if (pressed & TINROM_PAD_CIRCLE) sendKey(0x1b);

	if (repeated & TINROM_PAD_L1) scroll(1);
	if (repeated & TINROM_PAD_R1) scroll(-1);
	if (repeated & TINROM_PAD_L2) scroll(TERM_ROWS - 1);
	if (repeated & TINROM_PAD_R2) scroll(-(TERM_ROWS - 1));

	if (pressed & TINROM_PAD_TRIANGLE) {
		s.keyboardOpen = true;
		s.back         = 0;
	}

	if (pressed & TINROM_PAD_SELECT)
		return WANT_PHRASE;
	if (pressed & TINROM_PAD_START)
		return WANT_MENU;

	return WANT_NOTHING;
}

void terminalSession(
	TinromContext *ctx, const char *name, const CommsBookmark *bookmark
) {
	UIContext *ui       = &ctx->ui;
	bool       hadModel = uiModelVisible(ui);

	memset(&s, 0, sizeof(s));
	s.ctx    = ctx;
	s.name   = name;
	s.direct = !bookmark;

	if (bookmark) {
		snprintf(s.target, sizeof(s.target), "%s", bookmark->target);
		s.useTelnet = commsBookmarkTelnet(bookmark);
	}

	uiKeyboardInit(&s.keyboard);
	uiSetModelVisible(ui, false);
	/* Only seen over a popup. */
	uiSetChrome(ui, name, NULL);

	termInit(&term, termReply, NULL);
	term.idle = tinromSioPump;
	termViewInit();
	commsModemOpen(&commsConfig()->modem);

	if (s.direct) {
		commsModemDirect();
		termPrint(&term, "\033[1;36mTalking to the modem.\033[0m Type AT "
			"commands; ATD dials, and a call made that way works as\r\n"
			"usual. Triangle brings up the keyboard.\r\n\r\n");
	} else {
		char line[COMMS_TARGET_MAX + 32];

		snprintf(line, sizeof(line), "\033[1;36m[Dialling %s]\033[0m\r\n",
			s.target);
		termPrint(&term, line);
		commsModemDial(s.target);
	}

	s.lastState = commsModemState();

	bool leave = false;

	while (!leave) {
		uiBeginFrame(ui);
		uiSetChromeVisible(ui, false);

		/*
		 * Parsing and drawing can outlast the port's FIFO, so the far end
		 * is held off with RTS until the vsync wait, which drains it.
		 */
		tinromSioHold(true);

		/* Before and after: a CONNECT belongs above what follows it, and
		 * a NO CARRIER below what came before. */
		commsModemPoll();
		noteStateChange();
		receive();
		noteStateChange();

		int want = handlePad(tinromPadPressed(0) | tinromPadPressed(1),
			tinromPadRepeated(0) | tinromPadRepeated(1));

		termViewUpdate(&term, s.back);
		drawScreen(NULL);
		tinromSioHold(false);
		uiEndFrame(ui);

		/* Popups run frames of their own, so they wait for this one. */
		if (want == WANT_PHRASE)
			sendPhrase();
		else if (want == WANT_MENU)
			leave = sessionMenu();
	}

	commsModemClose();
	uiSetChromeVisible(ui, true);
	uiSetModelVisible(ui, hadModel);
}
