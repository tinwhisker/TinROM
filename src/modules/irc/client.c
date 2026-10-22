/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * An IRC session: the conversation on the terminal's 80-column screen, a
 * line to type into below it, and the modem behind both.
 *
 * The pad, with the keyboard put away:
 *
 *   Triangle   the on-screen keyboard  Cross     send the line
 *   Square     who is in the channel   Circle    back to the newest line
 *   Up / Down  lines sent before       Left / Right  move in the line
 *   L1 / R1    scroll back / forward   L2 / R2   a page back / forward
 *   Select     send a phrase           Start     the session menu
 *
 * With it out, keyboard.h has the keys: Enter sends, Tab completes a nick,
 * Esc clears the line, and Circle puts the keyboard away.
 */

#include <stdio.h>
#include "comms/config.h"
#include "comms/irc.h"
#include "comms/irctext.h"
#include "comms/modem.h"
#include "comms/term.h"
#include "comms/termview.h"
#include "comms/vault.h"
#include "hal/uart_sio.h"
#include "libc/string.h"
#include "modules/irc/client.h"
#include "modules/modules.h"
#include "tinrom/version.h"
#include "ui/keyboard.h"

#define READ_CHUNK      512
#define READS_PER_FRAME 8
#define INPUT_MAX       300
#define HISTORY         8

/* How long a /quit waits for the server to close before hanging up. */
#define QUIT_FRAMES     180

#define term (*termViewScreen())

static IrcClient irc;

static struct {
	TinromContext       *ctx;
	const CommsBookmark *bookmark;
	IrcSessionLogin     *login;

	/* A line from the server, as it arrives. */
	char     line[IRC_LINE_MAX + 1];
	size_t   lineLength;

	char     input[INPUT_MAX];
	size_t   inputLength, cursor;
	char     history[HISTORY][INPUT_MAX];
	unsigned historyCount, historyAt;

	bool     keyboardOpen;
	UIKeyboard keyboard;
	unsigned back;

	CommsModemState lastState;
	unsigned seenHighlights;
	uint32_t flashUntil;
	bool     quitting;
	uint32_t quitAt;
} s;

/* --- the line to the server ------------------------------------------------ */

static void sendLine(void *user, const char *line) {
	(void) user;

	if (commsModemState() != COMMS_MODEM_ONLINE)
		return;

	commsModemWrite(line, strlen(line));
	commsModemWrite("\r\n", 2);
}

static void printLine(void *user, const IrcLine *line) {
	(void) user;
	ircTextWrite(&term, line);
}

/* Something of tinrom's own, in the server's style. */
static void note(IrcStyle style, const char *text) {
	IrcLine line = { style, NULL, NULL, text, false };

	ircTextWrite(&term, &line);
}

static void login(void) {
	IrcSessionLogin *l = s.login;
	IrcLogin         in = {
		l->nick, l->user, l->realName, l->serverPassword,
		l->nickServPassword, l->channel, l->showJoins
	};

	/* The passwords stay until the session ends: a reconnect sends them
	 * again. ircSession() wipes them on the way out. */
	ircLogin(&irc, &in);
}

static void handleChunk(const uint8_t *data, size_t count) {
	for (size_t i = 0; i < count; i++) {
		char c = (char) data[i];

		if (c == '\n') {
			s.line[s.lineLength] = 0;
			ircHandleLine(&irc, s.line);
			s.lineLength = 0;
		} else if (c == '\r') {
			/* The LF that follows ends it. */
		} else if (s.lineLength < IRC_LINE_MAX) {
			/* A longer line than the protocol allows is cut, not split. */
			s.line[s.lineLength++] = c;
		}
	}
}

static void receive(void) {
	static uint8_t buffer[READ_CHUNK];

	for (unsigned i = 0; i < READS_PER_FRAME; i++) {
		size_t count = commsModemRead(buffer, sizeof(buffer));

		if (!count)
			break;

		/* Only the server's own words: what the modem says is shown by
		 * noteStateChange(). */
		if (commsModemState() == COMMS_MODEM_ONLINE)
			handleChunk(buffer, count);
	}
}

static void noteStateChange(void) {
	CommsModemState state = commsModemState();

	if (state == s.lastState)
		return;

	char text[96];

	if (state == COMMS_MODEM_ONLINE) {
		snprintf(text, sizeof(text), "Connected (%s); logging in as %s",
			commsModemStatus(), s.login->nick);
		note(IRC_STYLE_JOIN, text);
		s.lineLength = 0;
		login();
	} else if (state == COMMS_MODEM_IDLE) {
		snprintf(text, sizeof(text), "%s - Start for the menu, to reconnect",
			commsModemStatus());
		note(((s.lastState == COMMS_MODEM_ONLINE)
			|| (s.lastState == COMMS_MODEM_HANGING_UP)) ? IRC_STYLE_PART
			: IRC_STYLE_ERROR, text);
		ircDisconnected(&irc);
	}

	s.lastState = state;
}

/* Everything that must go on while anything is on screen: answering PINGs
 * above all, or the server gives up on us. */
static void service(void) {
	commsModemPoll();
	noteStateChange();
	receive();
	noteStateChange();
	ircTick(&irc, tinromFrameCounter());

	if (irc.highlights != s.seenHighlights) {
		s.seenHighlights = irc.highlights;
		s.flashUntil     = tinromFrameCounter() + 20;
	}

	if (s.quitting && (commsModemState() == COMMS_MODEM_ONLINE)
		&& (tinromFrameCounter() - s.quitAt > QUIT_FRAMES))
		commsModemHangup();
}

/* --- drawing -------------------------------------------------------------------- */

#define PANEL_ROW 12

static int inputTop(const UIContext *ui) {
	return s.keyboardOpen ? (int) ui->height - UI_KEYBOARD_HEIGHT - 14
		: TERM_VIEW_HEIGHT + 2 + PANEL_ROW;
}

static int chatTop(const UIContext *ui) {
	if (!s.keyboardOpen)
		return 0;

	/* The newest line just above the input box. */
	int bottom = inputTop(ui) - 2;
	int top    = bottom - (term.cy + 1) * TERM_VIEW_CELL_H;

	return (top < 0) ? top : 0;
}

static void drawInput(UIContext *ui, int y) {
	char prompt[IRC_CHANNEL_MAX + 4];
	int  width = (int) ui->width;

	uiFillRect(ui, 0, y - 1, width, 12, TINROM_COLOR_PANEL);

	snprintf(prompt, sizeof(prompt), "%s>", irc.inChannel ? irc.channel
		: (irc.registered ? "(no channel)" : ""));

	int promptWidth = uiTextWidth(prompt);

	if (promptWidth > 90)
		promptWidth = 90;

	uiTextClipped(ui, 4, y, 90, TINROM_COLOR_DIM, prompt);

	/* The end of a long line rather than its start, as uiEditText does. */
	int         x    = 8 + promptWidth;
	int         room = width - x - 6;
	char        before[INPUT_MAX];
	const char *shown = s.input;

	memcpy(before, s.input, s.cursor);
	before[s.cursor] = 0;

	while ((uiTextWidth(before + (shown - s.input)) > room)
		&& (shown < s.input + s.cursor))
		shown++;

	uiTextClipped(ui, x, y, room, TINROM_COLOR_TEXT, shown);

	if ((tinromFrameCounter() / 16) & 1)
		uiFillRect(ui, x + uiTextWidth(before + (shown - s.input)), y - 1, 1,
			10, TINROM_COLOR_ACCENT);
}

static const char *stateText(void) {
	switch (commsModemState()) {
		case COMMS_MODEM_DIALLING:   return commsModemStatus();
		case COMMS_MODEM_HANGING_UP: return "Hanging up...";
		case COMMS_MODEM_ONLINE:
			return irc.registered ? "Online" : "Logging in...";
		default:                     return "Offline";
	}
}

static void drawPanel(UIContext *ui) {
	int  y = TERM_VIEW_HEIGHT + 2;
	char text[IRC_TOPIC_MAX + IRC_CHANNEL_MAX];

	uiFillRect(ui, 0, TERM_VIEW_HEIGHT, (int) ui->width,
		(int) ui->height - TERM_VIEW_HEIGHT,
		(tinromFrameCounter() < s.flashUntil) ? TINROM_COLOR_HEADER
			: TINROM_COLOR_PANEL);

	/* The topic, or who we are when there is none. */
	if (irc.inChannel && irc.topic[0]) {
		ircTextPlain(irc.topic, text, sizeof(text));
	} else {
		snprintf(text, sizeof(text), "%s on %s", irc.nick[0] ? irc.nick
			: s.login->nick, s.bookmark->name);
	}

	uiTextClipped(ui, 4, y, (int) ui->width - 8, TINROM_COLOR_TEXT, text);

	drawInput(ui, y + PANEL_ROW);

	int y3 = y + 2 * PANEL_ROW;

	if (y3 + 10 > (int) ui->height)
		return;

	if (s.back) {
		snprintf(text, sizeof(text), "Back %u lines - R1 forward, O newest",
			s.back);
		uiTextColored(ui, 4, y3, TINROM_COLOR_ACCENT, text);
	} else {
		uiTextColored(ui, 4, y3, TINROM_COLOR_DIM,
			"/\\ type  X send  [] people  Start menu");
	}

	const char *state = stateText();
	uint32_t    lost  = tinromSioOverruns();

	if (lost && (commsModemState() == COMMS_MODEM_ONLINE)) {
		snprintf(text, sizeof(text), "lost %u", (unsigned int) lost);
		state = text;
	}

	uiTextColored(ui, (int) ui->width - 4 - uiTextWidth(state), y3,
		(commsModemState() == COMMS_MODEM_ONLINE) ? TINROM_COLOR_OK
			: TINROM_COLOR_DIM, state);
}

static void drawChat(void) {
	UIContext *ui = &s.ctx->ui;

	termViewDraw(ui, &term, 0, chatTop(ui), s.back, false);
}

static void drawScreen(void) {
	UIContext *ui = &s.ctx->ui;

	drawChat();

	if (s.keyboardOpen) {
		drawInput(ui, inputTop(ui));
		uiKeyboardDraw(&s.keyboard, ui, (int) ui->height - UI_KEYBOARD_HEIGHT);
	} else {
		drawPanel(ui);
	}
}

/* A popup's background: the conversation carries on behind it. */
static void drawBehind(void *unused) {
	(void) unused;

	service();
	termViewUpdate(&term, s.back);
	uiSetChromeVisible(&s.ctx->ui, true);
	drawChat();
}

/* --- the line being typed --------------------------------------------------- */

static void setInput(const char *text) {
	snprintf(s.input, sizeof(s.input), "%s", text);
	s.inputLength = s.cursor = strlen(s.input);
	s.historyAt   = s.historyCount;
}

static void remember(const char *text) {
	if (s.historyCount && !strcmp(s.history[(s.historyCount - 1) % HISTORY],
		text))
		return;

	snprintf(s.history[s.historyCount % HISTORY], INPUT_MAX, "%s", text);
	s.historyCount++;
}

static void submit(const char *text) {
	switch (ircInput(&irc, text)) {
		case IRC_INPUT_CLEAR:
			ircTextReset(&term);
			termViewInvalidate();
			break;

		case IRC_INPUT_QUIT:
			s.quitting = true;
			s.quitAt   = tinromFrameCounter();
			break;

		default:
			break;
	}
}

static void submitInput(void) {
	if (!s.inputLength)
		return;

	remember(s.input);
	submit(s.input);
	setInput("");
	s.back = 0;
}

/* Up and down through what was sent, the newest first. */
static void browseHistory(int step) {
	unsigned oldest = (s.historyCount > HISTORY) ? s.historyCount - HISTORY : 0;
	int      at     = (int) s.historyAt + step;

	if ((at < (int) oldest) || (at > (int) s.historyCount))
		return;

	s.historyAt = (unsigned) at;

	const char *text = (s.historyAt == s.historyCount) ? ""
		: s.history[s.historyAt % HISTORY];

	snprintf(s.input, sizeof(s.input), "%s", text);
	s.inputLength = s.cursor = strlen(s.input);
}

static void editKey(int key) {
	switch (key) {
		case UI_KEY_NONE:
			return;

		case '\r':
			submitInput();
			return;

		case '\t':
			if (ircCompleteNick(&irc, s.input, sizeof(s.input), &s.cursor))
				s.inputLength = strlen(s.input);
			return;

		case 0x1b:
			setInput("");
			return;

		case '\b':
			if (s.cursor) {
				memmove(&s.input[s.cursor - 1], &s.input[s.cursor],
					s.inputLength - s.cursor + 1);
				s.cursor--;
				s.inputLength--;
			}
			return;

		case UI_KEY_LEFT:
			if (s.cursor)
				s.cursor--;
			return;

		case UI_KEY_RIGHT:
			if (s.cursor < s.inputLength)
				s.cursor++;
			return;

		case UI_KEY_UP:
			browseHistory(-1);
			return;

		case UI_KEY_DOWN:
			browseHistory(1);
			return;

		default:
			if ((key >= 0x20) && (key < 0x7f)
				&& (s.inputLength + 1 < sizeof(s.input))) {
				memmove(&s.input[s.cursor + 1], &s.input[s.cursor],
					s.inputLength - s.cursor + 1);
				s.input[s.cursor++] = (char) key;
				s.inputLength++;
			}
			return;
	}
}

/* Starts a command for the user to finish on the keyboard. */
static void prefill(const char *text) {
	setInput(text);
	s.keyboardOpen = true;
	s.back         = 0;
}

/* --- who is here ------------------------------------------------------------------ */

#define LIST_COLUMNS 3
#define LIST_TOP     (TINROM_HEADER_H + 6)

static struct {
	uint16_t order[IRC_NICKS];
	unsigned count, selected, scroll;
	uint32_t serial;
	char     title[IRC_CHANNEL_MAX + 24];
} list;

static int listColumnWidth(const UIContext *ui) {
	return (int) (ui->width - 2 * TINROM_MARGIN_X) / LIST_COLUMNS;
}

static unsigned listRows(const UIContext *ui) {
	int rows = ((int) ui->height - LIST_TOP - TINROM_FOOTER_H - 4)
		/ TINROM_ROW_HEIGHT;

	return (rows < 1) ? 1 : (unsigned) rows;
}

/* Keeps the conversation going, and the list in step with it. */
static void listService(void) {
	service();

	/* Sorted again only when it changed: a few hundred names is a
	 * noticeable part of a frame. */
	if (list.serial != irc.nickSerial) {
		list.serial = irc.nickSerial;
		list.count  = ircSortedNicks(&irc, list.order);

		if (list.selected >= list.count)
			list.selected = list.count ? list.count - 1 : 0;
	}
}

static void listDraw(UIContext *ui) {
	int      columnWidth = listColumnWidth(ui);
	unsigned rows        = listRows(ui);
	unsigned row         = list.selected / LIST_COLUMNS;

	if (row < list.scroll)
		list.scroll = row;
	if (row >= list.scroll + rows)
		list.scroll = row - rows + 1;

	snprintf(list.title, sizeof(list.title), "%s - %u", irc.inChannel
		? irc.channel : "No channel", list.count + irc.nicksDropped);

	uiSetChromeVisible(ui, true);
	uiSetChrome(ui, list.title, "X: choose  L1/R1: page  O: back");
	termViewUpdate(&term, s.back);

	if (!list.count)
		uiText(ui, TINROM_MARGIN_X + 4, LIST_TOP + 4,
			irc.inChannel ? "Nobody listed yet." : "Not in a channel.");

	for (unsigned i = 0; i < rows * LIST_COLUMNS; i++) {
		unsigned index = list.scroll * LIST_COLUMNS + i;

		if (index >= list.count)
			break;

		const IrcNick *nick = &irc.nicks[list.order[index]];
		int  x = TINROM_MARGIN_X + (int) (i % LIST_COLUMNS) * columnWidth;
		int  y = LIST_TOP + (int) (i / LIST_COLUMNS) * TINROM_ROW_HEIGHT;
		char name[IRC_NICK_MAX + 1];

		snprintf(name, sizeof(name), "%c%s", nick->prefix ? nick->prefix
			: ' ', nick->name);

		if (index == list.selected)
			uiFillRect(ui, x, y - 2, columnWidth - 2, TINROM_ROW_HEIGHT,
				TINROM_COLOR_SELECTION);

		uiTextClipped(ui, x + 2, y, columnWidth - 6, nick->prefix
			? TINROM_COLOR_ACCENT : TINROM_COLOR_TEXT, name);
	}
}

static void listBehind(void *unused) {
	(void) unused;

	listService();
	listDraw(&s.ctx->ui);
}

/* What can be done to one nick. True once something was, to go back. */
static bool nickActions(const char *nick, int x, int y) {
	char       command[IRC_NICK_MAX + 16];
	UIMenuItem items[] = {
		{ "Message...", "Type a private message", true, 0 },
		{ "Address", "Put \"nick: \" in the line", true, 1 },
		{ "Who is it", "Ask the server (WHOIS)", true, 2 },
		{ "Client version", "Ask their client (CTCP VERSION)", true, 3 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(s.ctx, nick, items,
		sizeof(items) / sizeof(items[0]), x, y, listBehind, NULL, &cursor);

	switch (choice) {
		case 0:
			snprintf(command, sizeof(command), "/msg %s ", nick);
			prefill(command);
			return true;

		case 1:
			snprintf(command, sizeof(command), "%s: ", nick);
			prefill(command);
			return true;

		case 2:
			snprintf(command, sizeof(command), "/whois %s", nick);
			submit(command);
			return true;

		case 3:
			snprintf(command, sizeof(command), "/ctcp %s VERSION", nick);
			submit(command);
			return true;

		default:
			return false;
	}
}

static void nickList(void) {
	UIContext *ui    = &s.ctx->ui;
	bool       leave = false;

	list.serial   = irc.nickSerial - 1;
	list.selected = list.scroll = 0;

	while (!leave) {
		/* As in the session: the far end held off while this frame is
		 * worked out, and let go for the vsync wait, which drains it. */
		uiBeginFrame(ui);
		tinromSioHold(true);
		listService();
		listDraw(ui);
		tinromSioHold(false);
		uiEndFrame(ui);

		uint16_t pressed  = tinromPadPressed(0) | tinromPadPressed(1);
		uint16_t repeated = tinromPadRepeated(0) | tinromPadRepeated(1);
		unsigned count    = list.count;
		unsigned page     = listRows(ui) * LIST_COLUMNS;
		unsigned *sel     = &list.selected;

		if (pressed & TINROM_PAD_CIRCLE)
			break;
		if (!count)
			continue;

		if ((repeated & TINROM_PAD_RIGHT) && (*sel + 1 < count))
			(*sel)++;
		if ((repeated & TINROM_PAD_LEFT) && *sel)
			(*sel)--;
		if ((repeated & TINROM_PAD_DOWN) && (*sel + LIST_COLUMNS < count))
			*sel += LIST_COLUMNS;
		if ((repeated & TINROM_PAD_UP) && (*sel >= LIST_COLUMNS))
			*sel -= LIST_COLUMNS;
		if (repeated & TINROM_PAD_R1)
			*sel = (*sel + page < count) ? *sel + page : count - 1;
		if (repeated & TINROM_PAD_L1)
			*sel = (*sel > page) ? *sel - page : 0;

		if (pressed & TINROM_PAD_CROSS) {
			char nick[IRC_NICK_MAX];
			int  x = TINROM_MARGIN_X + (int) (*sel % LIST_COLUMNS)
				* listColumnWidth(ui) + 20;
			int  y = LIST_TOP + ((int) (*sel / LIST_COLUMNS)
				- (int) list.scroll + 1) * TINROM_ROW_HEIGHT;

			/* A copy: the list can change under the popup. */
			snprintf(nick, sizeof(nick), "%s", irc.nicks[list.order[*sel]].name);
			leave = nickActions(nick, x, y);
		}
	}

	uiSetChrome(ui, s.bookmark->name, NULL);
	termViewInvalidate();
}

/* --- menus ------------------------------------------------------------------- */

static void sendPhrase(void) {
	CommsConfig  *config = commsConfig();
	UIMenuItem    items[COMMS_PHRASES];
	size_t        count = 0;
	static size_t cursor;

	for (unsigned i = 0; i < COMMS_PHRASES; i++) {
		if (!config->phrases[i][0])
			continue;

		items[count++] = (UIMenuItem) { config->phrases[i],
			"Sent as if typed; /commands work", true, i };
	}

	if (!count)
		return;

	size_t choice = uiPopupRunAt(s.ctx, "Send", items, count, 160, 40,
		drawBehind, NULL, &cursor);

	if (choice != UI_MENU_CANCELLED) {
		remember(config->phrases[items[choice].tag]);
		submit(config->phrases[items[choice].tag]);
	}
}

static void waitForHangup(void) {
	UIContext *ui = &s.ctx->ui;

	commsModemHangup();

	for (unsigned frame = 0; (frame < 300)
		&& (commsModemState() == COMMS_MODEM_HANGING_UP); frame++) {
		service();
		uiBeginFrame(ui);
		uiSetChromeVisible(ui, false);
		termViewUpdate(&term, s.back);
		drawScreen();
		uiEndFrame(ui);
	}
}

/* Leaving politely: QUIT, and a moment for the server to see it. */
static void quitAndLeave(void) {
	UIContext *ui = &s.ctx->ui;

	if (commsModemState() == COMMS_MODEM_ONLINE) {
		if (!s.quitting)
			submit("/quit");

		for (unsigned frame = 0; (frame < QUIT_FRAMES)
			&& (commsModemState() == COMMS_MODEM_ONLINE); frame++) {
			service();
			uiBeginFrame(ui);
			uiSetChromeVisible(ui, false);
			termViewUpdate(&term, s.back);
			drawScreen();
			uiEndFrame(ui);
		}
	}

	if (commsModemState() == COMMS_MODEM_ONLINE)
		waitForHangup();
	else if (commsModemState() == COMMS_MODEM_DIALLING)
		commsModemCancel();
}

static void redial(void) {
	char text[COMMS_TARGET_MAX + 16];

	snprintf(text, sizeof(text), "Dialling %s", s.bookmark->target);
	note(IRC_STYLE_INFO, text);
	s.quitting = false;
	commsModemDial(s.bookmark->target);
}

/* Returns true to leave the session. */
static bool sessionMenu(void) {
	CommsModemState state  = commsModemState();
	bool            online = (state == COMMS_MODEM_ONLINE);
	bool            in     = online && irc.registered;

	UIMenuItem items[] = {
		{ "Join a channel...", "Leaves this one first", in, 0 },
		{ "Leave the channel", "PART", in && irc.inChannel, 1 },
		{ "People here", "The nick list (Square)", in && irc.inChannel, 2 },
		{ "Change nick...", "NICK", in, 3 },
		{ "Send a phrase", "One of the saved lines (Select)", in, 4 },
		{ online ? "Hang up" : "Reconnect",
			online ? "Drop the line; the screen stays" : "Dial the server again",
			online || (state == COMMS_MODEM_IDLE), 5 },
		{ "Clear screen", "Locally; nothing is sent", true, 6 },
		{ "Leave", online ? "Says goodbye, then hangs up" : "Back to the list",
			true, 7 }
	};
	static size_t cursor;

	size_t choice = uiPopupRunAt(s.ctx, "IRC", items,
		sizeof(items) / sizeof(items[0]), 160, 30, drawBehind, NULL, &cursor);

	switch (choice) {
		case 0: prefill("/join #"); break;
		case 1: submit("/part"); break;
		case 2: nickList(); break;
		case 3: prefill("/nick "); break;
		case 4: sendPhrase(); break;

		case 5:
			if (online)
				waitForHangup();
			else
				redial();
			break;

		case 6:
			ircTextReset(&term);
			termViewInvalidate();
			break;

		case 7:
			quitAndLeave();
			return true;
	}

	return false;
}

/* --- the session -------------------------------------------------------------- */

static void scroll(int lines) {
	int back = (int) s.back + lines;

	if (back < 0)
		back = 0;
	if (back > (int) term.historyCount)
		back = (int) term.historyCount;

	s.back = (unsigned) back;
}

enum { WANT_NOTHING = 0, WANT_PHRASE, WANT_MENU, WANT_NICKS };

static int handlePad(uint16_t pressed, uint16_t repeated) {
	if (pressed | repeated)
		commsEntropyStir(tinromTimerNoise() ^ pressed);

	if (s.keyboardOpen) {
		if (pressed & TINROM_PAD_CIRCLE) {
			s.keyboardOpen = false;
			return WANT_NOTHING;
		}

		editKey(uiKeyboardUpdate(&s.keyboard, pressed, repeated));

		return WANT_NOTHING;
	}

	if (repeated & TINROM_PAD_UP)    editKey(UI_KEY_UP);
	if (repeated & TINROM_PAD_DOWN)  editKey(UI_KEY_DOWN);
	if (repeated & TINROM_PAD_LEFT)  editKey(UI_KEY_LEFT);
	if (repeated & TINROM_PAD_RIGHT) editKey(UI_KEY_RIGHT);

	if (pressed & TINROM_PAD_CROSS) {
		if (s.inputLength)
			submitInput();
		else
			s.keyboardOpen = true;
	}

	if (pressed & TINROM_PAD_TRIANGLE) {
		s.keyboardOpen = true;
		s.back         = 0;
	}

	if (pressed & TINROM_PAD_CIRCLE)
		s.back = 0;

	if (repeated & TINROM_PAD_L1) scroll(1);
	if (repeated & TINROM_PAD_R1) scroll(-1);
	if (repeated & TINROM_PAD_L2) scroll(TERM_ROWS - 1);
	if (repeated & TINROM_PAD_R2) scroll(-(TERM_ROWS - 1));

	if (pressed & TINROM_PAD_SQUARE) return WANT_NICKS;
	if (pressed & TINROM_PAD_SELECT) return WANT_PHRASE;
	if (pressed & TINROM_PAD_START)  return WANT_MENU;

	return WANT_NOTHING;
}

void ircSession(TinromContext *ctx, const CommsBookmark *bookmark,
	IrcSessionLogin *loginData) {
	UIContext *ui       = &ctx->ui;
	bool       hadModel = uiModelVisible(ui);

	memset(&s, 0, sizeof(s));
	s.ctx      = ctx;
	s.bookmark = bookmark;
	s.login    = loginData;

	ircInit(&irc, sendLine, printLine, NULL);
	irc.version = TINROM_NAME " " TINROM_VERSION_STRING
		" - IRC on a PlayStation";

	uiKeyboardInit(&s.keyboard);
	uiSetModelVisible(ui, false);
	uiSetChrome(ui, bookmark->name, NULL);

	termInit(&term, NULL, NULL);
	term.idle = tinromSioPump;
	ircTextReset(&term);
	termViewInit();
	commsModemOpen(&commsConfig()->modem);

	note(IRC_STYLE_INFO, "tinrom IRC. Triangle types, Cross sends, Square "
		"lists who is here, Start has the rest. /help lists the commands.");
	redial();
	s.lastState = commsModemState();

	bool leave = false;

	while (!leave) {
		uiBeginFrame(ui);
		uiSetChromeVisible(ui, false);
		tinromSioHold(true);

		service();

		int want = handlePad(tinromPadPressed(0) | tinromPadPressed(1),
			tinromPadRepeated(0) | tinromPadRepeated(1));

		termViewUpdate(&term, s.back);
		drawScreen();
		tinromSioHold(false);
		uiEndFrame(ui);

		/* A /quit that the server has answered by closing: done. */
		if (s.quitting && (commsModemState() == COMMS_MODEM_IDLE))
			break;

		if (want == WANT_PHRASE)
			sendPhrase();
		else if (want == WANT_NICKS)
			nickList();
		else if (want == WANT_MENU)
			leave = sessionMenu();
	}

	commsModemClose();
	ircDisconnected(&irc);

	/* Passwords, and whatever was typed - "/msg NickServ IDENTIFY ..." -
	 * do not outlive the session. */
	memset(loginData, 0, sizeof(*loginData));
	memset(&s, 0, sizeof(s));
	uiSetChromeVisible(ui, true);
	uiSetModelVisible(ui, hadModel);
}
