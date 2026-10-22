/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The IRC protocol. See irc.h.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "comms/irc.h"
#include "libc/string.h"

#define CTCP 0x01

/* At most one CTCP answer a second and a half, so a flood of requests
 * cannot get us thrown off for flooding in turn. */
#define CTCP_GAP_FRAMES 90

/* --- small helpers ------------------------------------------------------- */

/* strcspn, which the console's libc does not have. */
static size_t spanUntil(const char *text, const char *stops) {
	size_t length = 0;

	while (text[length] && !strchr(stops, text[length]))
		length++;

	return length;
}

static void copy(char *to, size_t size, const char *from) {
	snprintf(to, size, "%s", from ? from : "");
}

static int foldChar(int c) {
	if ((c >= 'A') && (c <= 'Z'))
		return c + ('a' - 'A');

	/* RFC 1459: []\~ are the upper case of {}|^. */
	switch (c) {
		case '[':  return '{';
		case ']':  return '}';
		case '\\': return '|';
		case '~':  return '^';
		default:   return c;
	}
}

int ircCompare(const char *a, const char *b) {
	for (;; a++, b++) {
		int ca = foldChar((unsigned char) *a), cb = foldChar((unsigned char) *b);

		if (ca != cb)
			return ca - cb;
		if (!ca)
			return 0;
	}
}

static bool prefixMatches(const char *text, const char *start) {
	for (; *start; text++, start++) {
		if (foldChar((unsigned char) *text) != foldChar((unsigned char) *start))
			return false;
	}

	return true;
}

static bool isNickChar(char c) {
	return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))
		|| ((c >= '0') && (c <= '9')) || (c && strchr("-_[]\\`^{}|", c));
}

bool ircMentions(const char *text, const char *nick) {
	size_t length = strlen(nick);

	if (!length)
		return false;

	for (const char *at = text; *at; at++) {
		if ((at != text) && isNickChar(at[-1]))
			continue;
		if (!prefixMatches(at, nick))
			continue;
		if (!isNickChar(at[length]))
			return true;
	}

	return false;
}

static bool isChannel(const IrcClient *irc, const char *name) {
	return name && name[0] && strchr(irc->chanTypes, name[0]);
}

static void sendf(IrcClient *irc, const char *format, ...) {
	char    line[IRC_LINE_MAX];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line) - 2, format, args);
	va_end(args);

	/* A CR or LF in what the user typed would start a second command. */
	for (char *p = line; *p; p++) {
		if ((*p == '\r') || (*p == '\n'))
			*p = ' ';
	}

	irc->send(irc->user, line);

	/* It may have been a PASS or an IDENTIFY. */
	for (volatile char *p = line; *p; p++)
		*p = 0;
}

static void show(IrcClient *irc, IrcStyle style, const char *who,
	const char *where, const char *text, bool highlight) {
	IrcLine line = { style, who, where, text, highlight };

	irc->print(irc->user, &line);
}

static void info(IrcClient *irc, IrcStyle style, const char *format, ...) {
	char    text[IRC_LINE_MAX];
	va_list args;

	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);

	show(irc, style, NULL, NULL, text, false);
}

/* The channel tag to show for `target`: NULL when it is the current one. */
static const char *whereFor(const IrcClient *irc, const char *target) {
	if (irc->inChannel && !ircCompare(target, irc->channel))
		return NULL;

	return target;
}

/* --- parsing ---------------------------------------------------------- */

bool ircParse(char *line, IrcMessage *m) {
	memset(m, 0, sizeof(*m));
	m->source = "";

	char *p = line;

	while (*p == ' ')
		p++;

	/* IRCv3 tags: never asked for, but skipped if a server sends them. */
	if (*p == '@') {
		while (*p && (*p != ' '))
			p++;
		while (*p == ' ')
			p++;
	}

	if (*p == ':') {
		m->source = ++p;

		while (*p && (*p != ' '))
			p++;
		if (*p)
			*p++ = 0;

		size_t length = spanUntil(m->source, "!@");

		if (length >= sizeof(m->nick))
			length = sizeof(m->nick) - 1;

		memcpy(m->nick, m->source, length);
		m->nick[length] = 0;

		while (*p == ' ')
			p++;
	}

	if (!*p)
		return false;

	m->command = p;

	while (*p && (*p != ' '))
		p++;

	while (*p) {
		*p++ = 0;

		while (*p == ' ')
			p++;
		if (!*p)
			break;

		if ((*p == ':') || (m->count == IRC_PARAMS - 1)) {
			m->params[m->count++] = (*p == ':') ? p + 1 : p;
			break;
		}

		m->params[m->count++] = p;

		while (*p && (*p != ' '))
			p++;
	}

	return true;
}

static const char *param(const IrcMessage *m, unsigned index) {
	return (index < m->count) ? m->params[index] : "";
}

/* Parameters from `first` on, joined with spaces, the way a numeric reads. */
static void joinParams(const IrcMessage *m, unsigned first, char *out,
	size_t size) {
	out[0] = 0;

	for (unsigned i = first; i < m->count; i++) {
		size_t used = strlen(out);

		snprintf(out + used, size - used, "%s%s", (i == first) ? "" : " ",
			m->params[i]);
	}
}

/* --- the nick list ------------------------------------------------------------ */

static int findNick(const IrcClient *irc, const char *name) {
	for (unsigned i = 0; i < irc->nickCount; i++) {
		if (!ircCompare(irc->nicks[i].name, name))
			return (int) i;
	}

	return -1;
}

/* A prefix's rank: 0 for the highest, strlen(prefixChars) for none. */
static unsigned rank(const IrcClient *irc, char prefix) {
	const char *at = prefix ? strchr(irc->prefixChars, prefix) : NULL;

	return at ? (unsigned) (at - irc->prefixChars)
		: (unsigned) strlen(irc->prefixChars);
}

static void addNick(IrcClient *irc, const char *name, char prefix) {
	if (!name[0])
		return;

	int at = findNick(irc, name);

	irc->nickSerial++;

	if (at >= 0) {
		if (rank(irc, prefix) < rank(irc, irc->nicks[at].prefix))
			irc->nicks[at].prefix = prefix;
		return;
	}

	if (irc->nickCount >= IRC_NICKS) {
		irc->nicksDropped++;
		return;
	}

	IrcNick *nick = &irc->nicks[irc->nickCount++];

	nick->prefix = prefix;
	copy(nick->name, sizeof(nick->name), name);
}

static bool removeNick(IrcClient *irc, const char *name) {
	int at = findNick(irc, name);

	if (at < 0)
		return false;

	irc->nicks[at] = irc->nicks[--irc->nickCount];
	irc->nickSerial++;

	return true;
}

static void clearNicks(IrcClient *irc) {
	irc->nickCount    = 0;
	irc->nicksDropped = 0;
	irc->namesDone    = true;
	irc->nickSerial++;
}

static bool sortsBefore(const IrcClient *irc, const uint8_t *ranks,
	uint16_t a, uint16_t b) {
	if (ranks[a] != ranks[b])
		return ranks[a] < ranks[b];

	return ircCompare(irc->nicks[a].name, irc->nicks[b].name) < 0;
}

unsigned ircSortedNicks(const IrcClient *irc, uint16_t *order) {
	static uint8_t ranks[IRC_NICKS];
	unsigned       count = irc->nickCount;

	for (unsigned i = 0; i < count; i++) {
		order[i] = (uint16_t) i;
		ranks[i] = (uint8_t) rank(irc, irc->nicks[i].prefix);
	}

	/*
	 * Shell sort, ranks worked out once: it runs while the serial port is
	 * held, and a few hundred names by insertion took a third of a second.
	 */
	static const unsigned gaps[] = { 132, 57, 23, 10, 4, 1 };

	for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
		unsigned gap = gaps[g];

		for (unsigned i = gap; i < count; i++) {
			uint16_t moving = order[i];
			unsigned j      = i;

			for (; (j >= gap) && sortsBefore(irc, ranks, moving, order[j - gap]);
				j -= gap)
				order[j] = order[j - gap];

			order[j] = moving;
		}
	}

	return count;
}

/* --- ISUPPORT ------------------------------------------------------------- */

static void isupport(IrcClient *irc, const IrcMessage *m) {
	/* The last parameter is "are supported by this server". */
	for (unsigned i = 1; i + 1 < m->count; i++) {
		const char *token = m->params[i];

		if (!strncmp(token, "PREFIX=(", 8)) {
			const char *modes = token + 8;
			const char *close = strchr(modes, ')');

			if (!close)
				continue;

			size_t length = (size_t) (close - modes);

			if ((length >= sizeof(irc->prefixModes))
				|| (strlen(close + 1) != length))
				continue;

			memcpy(irc->prefixModes, modes, length);
			irc->prefixModes[length] = 0;
			copy(irc->prefixChars, sizeof(irc->prefixChars), close + 1);
		} else if (!strncmp(token, "CHANTYPES=", 10)) {
			copy(irc->chanTypes, sizeof(irc->chanTypes), token + 10);
		} else if (!strncmp(token, "CHANMODES=", 10)) {
			/* A,B,C,D: A and B always take a parameter, C when set. */
			char        always[40] = "", onSet[24] = "";
			const char *at    = token + 10;
			unsigned    group = 0;

			for (; *at && (group < 3); at++) {
				if (*at == ',') {
					group++;
					continue;
				}

				char  *into = (group < 2) ? always : onSet;
				size_t size = (group < 2) ? sizeof(always) : sizeof(onSet);
				size_t used = strlen(into);

				if (used + 1 < size) {
					into[used]     = *at;
					into[used + 1] = 0;
				}
			}

			copy(irc->modesAlways, sizeof(irc->modesAlways), always);
			copy(irc->modesOnSet, sizeof(irc->modesOnSet), onSet);
		}
	}
}

/* --- what the server says ---------------------------------------------------- */

static void welcome(IrcClient *irc, const IrcMessage *m) {
	irc->registered = true;
	copy(irc->nick, sizeof(irc->nick), param(m, 0));
	show(irc, IRC_STYLE_INFO, NULL, NULL, param(m, m->count - 1), false);

	if (irc->nickServPassword[0]) {
		sendf(irc, "PRIVMSG NickServ :IDENTIFY %s", irc->nickServPassword);
		info(irc, IRC_STYLE_INFO, "Identifying to NickServ");
	}

	memset(irc->nickServPassword, 0, sizeof(irc->nickServPassword));

	if (irc->wantChannel[0])
		sendf(irc, "JOIN %s%s", isChannel(irc, irc->wantChannel) ? "" : "#",
			irc->wantChannel);
}

/* Nick in use before we are in: try another, a few times. */
static void nickTaken(IrcClient *irc, const IrcMessage *m) {
	if (irc->registered) {
		info(irc, IRC_STYLE_ERROR, "%s is taken", param(m, 1));
		return;
	}

	if (++irc->nickTries > 6) {
		info(irc, IRC_STYLE_ERROR,
			"Every nick tried is taken. Pick one with /nick name");
		return;
	}

	/*
	 * First the nick and an underscore; then, in case the server cut that
	 * off again, the first eight characters and a digit - nine being the
	 * shortest NICKLEN still about.
	 */
	if (irc->nickTries == 1) {
		snprintf(irc->nick, sizeof(irc->nick), "%s_", irc->wantNick);
	} else {
		snprintf(irc->nick, sizeof(irc->nick), "%.8s%u", irc->wantNick,
			irc->nickTries - 1);
	}

	info(irc, IRC_STYLE_INFO, "%s is taken; trying %s", param(m, 1), irc->nick);
	sendf(irc, "NICK %s", irc->nick);
}

static void names(IrcClient *irc, const IrcMessage *m) {
	/* 353 me type channel :names */
	if (!irc->inChannel || ircCompare(param(m, 2), irc->channel))
		return;

	if (irc->namesDone) {
		clearNicks(irc);
		irc->namesDone = false;
	}

	char *list = (char *) param(m, 3);

	while (*list) {
		while (*list == ' ')
			list++;
		if (!*list)
			break;

		char *name = list;

		while (*list && (*list != ' '))
			list++;
		if (*list)
			*list++ = 0;

		/* multi-prefix sends them all; the first is the highest. */
		char prefix = 0;

		if (strchr(irc->prefixChars, *name) && *name) {
			prefix = *name;

			while (*name && strchr(irc->prefixChars, *name))
				name++;
		}

		/* userhost-in-names would add !user@host. */
		char *bang = strchr(name, '!');

		if (bang)
			*bang = 0;

		if (*name)
			addNick(irc, name, prefix);
	}
}

static void namesEnd(IrcClient *irc, const IrcMessage *m) {
	if (!irc->inChannel || ircCompare(param(m, 1), irc->channel))
		return;

	irc->namesDone = true;

	if (irc->nickCount > 40) {
		info(irc, IRC_STYLE_INFO, "%u people in %s%s - Square lists them",
			irc->nickCount + irc->nicksDropped, irc->channel,
			irc->nicksDropped ? " (too many to list all)" : "");
		return;
	}

	static uint16_t order[IRC_NICKS];
	char            line[IRC_LINE_MAX];
	unsigned        count = ircSortedNicks(irc, order);

	snprintf(line, sizeof(line), "%u in %s:", count, irc->channel);

	for (unsigned i = 0; i < count; i++) {
		const IrcNick *nick = &irc->nicks[order[i]];
		size_t         used = strlen(line);
		char           prefix[2] = { nick->prefix, 0 };

		snprintf(line + used, sizeof(line) - used, " %s%s", prefix, nick->name);
	}

	show(irc, IRC_STYLE_INFO, NULL, NULL, line, false);
}

static void joined(IrcClient *irc, const IrcMessage *m) {
	const char *channel = param(m, 0);

	if (!ircCompare(m->nick, irc->nick)) {
		irc->inChannel = true;
		copy(irc->channel, sizeof(irc->channel), channel);
		irc->topic[0] = 0;
		clearNicks(irc);
		info(irc, IRC_STYLE_JOIN, "Now talking in %s", channel);
		return;
	}

	if (!irc->inChannel || ircCompare(channel, irc->channel))
		return;

	addNick(irc, m->nick, 0);

	if (irc->showJoins)
		info(irc, IRC_STYLE_JOIN, "%s (%s) joined", m->nick,
			strchr(m->source, '!') ? strchr(m->source, '!') + 1 : m->source);
}

static void leftChannel(IrcClient *irc) {
	irc->inChannel = false;
	irc->topic[0]  = 0;
	clearNicks(irc);
}

static void parted(IrcClient *irc, const IrcMessage *m) {
	const char *channel = param(m, 0);
	const char *reason  = param(m, 1);

	if (!irc->inChannel || ircCompare(channel, irc->channel))
		return;

	if (!ircCompare(m->nick, irc->nick)) {
		info(irc, IRC_STYLE_PART, "You left %s", channel);
		leftChannel(irc);
		return;
	}

	removeNick(irc, m->nick);

	if (irc->showJoins)
		info(irc, IRC_STYLE_PART, "%s left%s%s%s", m->nick, reason[0] ? " (" : "",
			reason, reason[0] ? ")" : "");
}

static void kicked(IrcClient *irc, const IrcMessage *m) {
	const char *channel = param(m, 0), *victim = param(m, 1);
	const char *reason  = param(m, 2);

	if (!irc->inChannel || ircCompare(channel, irc->channel))
		return;

	if (!ircCompare(victim, irc->nick)) {
		info(irc, IRC_STYLE_ERROR, "%s kicked you from %s (%s)", m->nick,
			channel, reason);
		leftChannel(irc);
		return;
	}

	removeNick(irc, victim);
	info(irc, IRC_STYLE_PART, "%s was kicked by %s (%s)", victim, m->nick,
		reason);
}

static void quit(IrcClient *irc, const IrcMessage *m) {
	if (removeNick(irc, m->nick) && irc->showJoins)
		info(irc, IRC_STYLE_PART, "%s quit (%s)", m->nick, param(m, 0));
}

static void nickChanged(IrcClient *irc, const IrcMessage *m) {
	const char *to = param(m, 0);

	if (!to[0])
		return;

	/* A name already listed - a stale entry - goes, rather than doubling. */
	if (ircCompare(m->nick, to))
		removeNick(irc, to);

	if (!ircCompare(m->nick, irc->nick)) {
		copy(irc->nick, sizeof(irc->nick), to);

		int at = findNick(irc, m->nick);

		if (at >= 0) {
			copy(irc->nicks[at].name, sizeof(irc->nicks[at].name), to);
			irc->nickSerial++;
		}

		info(irc, IRC_STYLE_CHANGE, "You are now %s", to);
		return;
	}

	int at = findNick(irc, m->nick);

	if (at < 0)
		return;

	copy(irc->nicks[at].name, sizeof(irc->nicks[at].name), to);
	irc->nickSerial++;

	if (irc->showJoins)
		info(irc, IRC_STYLE_CHANGE, "%s is now %s", m->nick, to);
}

static void topicChanged(IrcClient *irc, const IrcMessage *m) {
	if (irc->inChannel && !ircCompare(param(m, 0), irc->channel))
		copy(irc->topic, sizeof(irc->topic), param(m, 1));

	info(irc, IRC_STYLE_CHANGE, "%s set the topic%s%s: %s", m->nick,
		whereFor(irc, param(m, 0)) ? " of " : "",
		whereFor(irc, param(m, 0)) ? param(m, 0) : "", param(m, 1));
}

static void modeChanged(IrcClient *irc, const IrcMessage *m) {
	char text[IRC_LINE_MAX];

	joinParams(m, 1, text, sizeof(text));

	if (!isChannel(irc, param(m, 0))) {
		info(irc, IRC_STYLE_CHANGE, "Mode %s for %s", text, param(m, 0));
		return;
	}

	info(irc, IRC_STYLE_CHANGE, "%s sets mode %s%s%s", m->nick, text,
		whereFor(irc, param(m, 0)) ? " on " : "",
		whereFor(irc, param(m, 0)) ? param(m, 0) : "");

	if (!irc->inChannel || ircCompare(param(m, 0), irc->channel))
		return;

	/* Walk the mode string, taking parameters as each mode needs them. */
	const char *modes = param(m, 1);
	unsigned    next  = 2;
	bool        set   = true;

	for (; *modes; modes++) {
		char mode = *modes;

		if ((mode == '+') || (mode == '-')) {
			set = (mode == '+');
			continue;
		}

		const char *prefixAt = strchr(irc->prefixModes, mode);

		if (prefixAt) {
			const char *who = param(m, next++);
			int         at  = findNick(irc, who);

			if (at < 0)
				continue;

			char prefix = irc->prefixChars[prefixAt - irc->prefixModes];

			/*
			 * Without the full set of a nick's modes only the highest is
			 * known; taking one away leaves none, which a later NAMES
			 * corrects.
			 */
			if (set) {
				if (rank(irc, prefix) < rank(irc, irc->nicks[at].prefix))
					irc->nicks[at].prefix = prefix;
			} else if (irc->nicks[at].prefix == prefix) {
				irc->nicks[at].prefix = 0;
			}

			irc->nickSerial++;
		} else if (strchr(irc->modesAlways, mode)
			|| (set && strchr(irc->modesOnSet, mode))) {
			next++;
		}
	}
}

/* A CTCP request inside a PRIVMSG: answered, quietly, with a NOTICE. */
static void ctcpRequest(IrcClient *irc, const IrcMessage *m, char *body,
	const char *target) {
	char *space = strchr(body, ' ');
	char *args  = space ? space + 1 : "";

	if (space)
		*space = 0;

	if (!strcmp(body, "ACTION")) {
		bool mine = isChannel(irc, target);

		show(irc, mine ? IRC_STYLE_ACTION : IRC_STYLE_PRIVATE, m->nick,
			mine ? whereFor(irc, target) : NULL, args,
			!mine || ircMentions(args, irc->nick));

		if (!mine)
			copy(irc->lastPrivate, sizeof(irc->lastPrivate), m->nick);
		if (!mine || ircMentions(args, irc->nick))
			irc->highlights++;
		return;
	}

	info(irc, IRC_STYLE_INFO, "CTCP %s from %s", body, m->nick);

	if (irc->now - irc->lastCtcpReply < CTCP_GAP_FRAMES)
		return;

	if (!strcmp(body, "VERSION")) {
		sendf(irc, "NOTICE %s :\001VERSION %s\001", m->nick, irc->version);
	} else if (!strcmp(body, "PING")) {
		sendf(irc, "NOTICE %s :\001PING %s\001", m->nick, args);
	} else if (!strcmp(body, "TIME")) {
		sendf(irc, "NOTICE %s :\001TIME unknown - a PlayStation has no "
			"clock\001", m->nick);
	} else if (!strcmp(body, "CLIENTINFO")) {
		sendf(irc, "NOTICE %s :\001CLIENTINFO ACTION CLIENTINFO PING TIME "
			"VERSION\001", m->nick);
	} else {
		return;
	}

	irc->lastCtcpReply = irc->now;
}

/* Strips a CTCP's \001s in place; NULL if `text` is not one. */
static char *ctcpBody(char *text) {
	size_t length = strlen(text);

	if ((length < 2) || (text[0] != CTCP))
		return NULL;

	if (text[length - 1] == CTCP)
		text[length - 1] = 0;

	return text + 1;
}

static void privmsg(IrcClient *irc, const IrcMessage *m) {
	const char *target = param(m, 0);
	char       *text   = (char *) param(m, 1);
	char       *ctcp   = ctcpBody(text);

	if (ctcp) {
		ctcpRequest(irc, m, ctcp, target);
		return;
	}

	if (isChannel(irc, target)) {
		bool highlight = ircMentions(text, irc->nick);

		if (highlight)
			irc->highlights++;

		show(irc, IRC_STYLE_MESSAGE, m->nick, whereFor(irc, target), text,
			highlight);
		return;
	}

	copy(irc->lastPrivate, sizeof(irc->lastPrivate), m->nick);
	irc->highlights++;
	show(irc, IRC_STYLE_PRIVATE, m->nick, NULL, text, true);
}

static void notice(IrcClient *irc, const IrcMessage *m) {
	char *text = (char *) param(m, 1);
	char *ctcp = ctcpBody(text);

	/* Before welcome, or from a server rather than a person. */
	if (!m->source[0] || !strchr(m->source, '!')) {
		show(irc, IRC_STYLE_INFO, NULL, NULL, text, false);
		return;
	}

	if (ctcp) {
		info(irc, IRC_STYLE_INFO, "CTCP reply from %s: %s", m->nick, ctcp);
		return;
	}

	show(irc, IRC_STYLE_NOTICE, m->nick, isChannel(irc, param(m, 0))
		? whereFor(irc, param(m, 0)) : NULL, text, false);
}

static void numeric(IrcClient *irc, IrcMessage *m, int number) {
	char text[IRC_LINE_MAX];

	switch (number) {
		case 1:
			welcome(irc, m);
			return;

		case 5:
			isupport(irc, m);
			return;

		case 332: /* topic: me channel :text */
			if (irc->inChannel && !ircCompare(param(m, 1), irc->channel))
				copy(irc->topic, sizeof(irc->topic), param(m, 2));

			info(irc, IRC_STYLE_CHANGE, "Topic: %s", param(m, 2));
			return;

		case 333: /* who set it, and when: nothing to show it with */
		case 329: /* the channel's creation time */
			return;

		case 353:
			names(irc, m);
			return;

		case 366:
			namesEnd(irc, m);
			return;

		case 372: case 375: case 376:
			show(irc, IRC_STYLE_MOTD, NULL, NULL, param(m, m->count - 1),
				false);
			return;

		case 432: case 433: case 436: case 437:
			nickTaken(irc, m);
			return;
	}

	joinParams(m, 1, text, sizeof(text));
	show(irc, ((number >= 400) && (number < 600)) ? IRC_STYLE_ERROR
		: IRC_STYLE_INFO, NULL, NULL, text, false);
}

void ircHandleLine(IrcClient *irc, char *line) {
	IrcMessage m;

	if (!ircParse(line, &m))
		return;

	const char *c = m.command;

	if ((c[0] >= '0') && (c[0] <= '9') && (c[1] >= '0') && (c[1] <= '9')
		&& (c[2] >= '0') && (c[2] <= '9') && !c[3]) {
		numeric(irc, &m, (c[0] - '0') * 100 + (c[1] - '0') * 10 + (c[2] - '0'));
	} else if (!strcmp(c, "PING")) {
		sendf(irc, "PONG :%s", param(&m, 0));
	} else if (!strcmp(c, "PRIVMSG")) {
		privmsg(irc, &m);
	} else if (!strcmp(c, "NOTICE")) {
		notice(irc, &m);
	} else if (!strcmp(c, "JOIN")) {
		joined(irc, &m);
	} else if (!strcmp(c, "PART")) {
		parted(irc, &m);
	} else if (!strcmp(c, "KICK")) {
		kicked(irc, &m);
	} else if (!strcmp(c, "QUIT")) {
		quit(irc, &m);
	} else if (!strcmp(c, "NICK")) {
		nickChanged(irc, &m);
	} else if (!strcmp(c, "TOPIC")) {
		topicChanged(irc, &m);
	} else if (!strcmp(c, "MODE")) {
		modeChanged(irc, &m);
	} else if (!strcmp(c, "INVITE")) {
		info(irc, IRC_STYLE_INFO, "%s invites you to %s - /join %s", m.nick,
			param(&m, 1), param(&m, 1));
	} else if (!strcmp(c, "ERROR")) {
		show(irc, IRC_STYLE_ERROR, NULL, NULL, param(&m, 0), false);
	} else if (!strcmp(c, "PONG") || !strcmp(c, "CAP")) {
		/* Nothing to show. */
	} else {
		char text[IRC_LINE_MAX];

		joinParams(&m, 0, text, sizeof(text));
		info(irc, IRC_STYLE_INFO, "%s %s %s", m.nick, c, text);
	}
}

/* --- the session ---------------------------------------------------------- */

void ircInit(IrcClient *irc, void (*send)(void *, const char *),
	void (*print)(void *, const IrcLine *), void *user) {
	memset(irc, 0, sizeof(*irc));
	irc->send    = send;
	irc->print   = print;
	irc->user    = user;
	irc->version = "tinrom";
	ircDisconnected(irc);
}

void ircDisconnected(IrcClient *irc) {
	irc->registered = false;
	leftChannel(irc);
	copy(irc->prefixModes, sizeof(irc->prefixModes), "ov");
	copy(irc->prefixChars, sizeof(irc->prefixChars), "@+");
	copy(irc->chanTypes, sizeof(irc->chanTypes), "#&");
	copy(irc->modesAlways, sizeof(irc->modesAlways), "beIk");
	copy(irc->modesOnSet, sizeof(irc->modesOnSet), "l");
	memset(irc->nickServPassword, 0, sizeof(irc->nickServPassword));
}

void ircLogin(IrcClient *irc, const IrcLogin *login) {
	copy(irc->nick, sizeof(irc->nick), login->nick);
	copy(irc->wantNick, sizeof(irc->wantNick), login->nick);
	copy(irc->wantChannel, sizeof(irc->wantChannel), login->channel);
	copy(irc->nickServPassword, sizeof(irc->nickServPassword),
		login->nickServPassword);
	irc->showJoins = login->showJoins;
	irc->nickTries = 0;

	if (login->serverPassword && login->serverPassword[0])
		sendf(irc, "PASS :%s", login->serverPassword);

	sendf(irc, "NICK %s", irc->nick);
	sendf(irc, "USER %s 0 * :%s", login->user, login->realName);
}

void ircTick(IrcClient *irc, uint32_t frame) {
	irc->now = frame;
}

/* --- what the user types -------------------------------------------------------- */

/* Not connected yet: nothing would be sent, so nothing is shown as said. */
static bool offline(IrcClient *irc) {
	if (irc->registered)
		return false;

	info(irc, IRC_STYLE_ERROR, "Not connected - Start, Reconnect");

	return true;
}

static void say(IrcClient *irc, const char *target, const char *text) {
	if (offline(irc))
		return;

	if (!target[0]) {
		info(irc, IRC_STYLE_ERROR, "Not in a channel - /join #name first");
		return;
	}

	sendf(irc, "PRIVMSG %s :%s", target, text);

	if (isChannel(irc, target))
		show(irc, IRC_STYLE_OWN, irc->nick, whereFor(irc, target), text, false);
	else
		show(irc, IRC_STYLE_PRIVATE_OUT, target, NULL, text, false);
}

static void action(IrcClient *irc, const char *target, const char *text) {
	if (offline(irc))
		return;

	if (!target[0]) {
		info(irc, IRC_STYLE_ERROR, "Not in a channel - /join #name first");
		return;
	}

	sendf(irc, "PRIVMSG %s :\001ACTION %s\001", target, text);
	show(irc, IRC_STYLE_OWN_ACTION, irc->nick, whereFor(irc, target), text,
		false);
}

static const char *current(const IrcClient *irc) {
	return irc->inChannel ? irc->channel : "";
}

/* Splits off the first word of `*rest` into `word`. */
static void firstWord(const char **rest, char *word, size_t size) {
	const char *p = *rest;

	while (*p == ' ')
		p++;

	size_t length = spanUntil(p, " ");

	if (length >= size)
		length = size - 1;

	memcpy(word, p, length);
	word[length] = 0;
	p += spanUntil(p, " ");

	while (*p == ' ')
		p++;

	*rest = p;
}

static const char *const helpText[] = {
	"/join #chan [key]   leave this channel for another",
	"/part [why]         leave it      /quit [why]  disconnect",
	"/msg nick text      a private message;  /r text  answers the last one",
	"/me does something  /nick name    /topic [text]   /whois nick",
	"/names  /notice nick text  /ctcp nick VERSION  /away [why]  /clear",
	"/raw LINE           sent as it is; any other /command goes to the server"
};

IrcInputResult ircInput(IrcClient *irc, const char *text) {
	if (!text[0])
		return IRC_INPUT_DONE;

	/* "//" sends a line that starts with a slash. */
	if ((text[0] != '/') || (text[1] == '/')) {
		say(irc, current(irc), text + ((text[0] == '/') ? 1 : 0));
		return IRC_INPUT_DONE;
	}

	char        verb[16], word[IRC_CHANNEL_MAX];
	const char *rest = text + 1;

	firstWord(&rest, verb, sizeof(verb));

	for (char *v = verb; *v; v++)
		*v = (char) foldChar((unsigned char) *v);

	if (!strcmp(verb, "join") || !strcmp(verb, "j")) {
		firstWord(&rest, word, sizeof(word));

		if (!word[0]) {
			info(irc, IRC_STYLE_ERROR, "/join #channel");
			return IRC_INPUT_DONE;
		}

		char channel[IRC_CHANNEL_MAX + 1];

		/* "/join tinrom" means #tinrom. */
		snprintf(channel, sizeof(channel), "%s%s", isChannel(irc, word) ? ""
			: "#", word);

		if (irc->inChannel && !ircCompare(channel, irc->channel))
			return IRC_INPUT_DONE;
		if (irc->inChannel)
			sendf(irc, "PART %s", irc->channel);

		sendf(irc, rest[0] ? "JOIN %s %s" : "JOIN %s", channel, rest);
	} else if (!strcmp(verb, "part") || !strcmp(verb, "leave")) {
		if (irc->inChannel)
			sendf(irc, rest[0] ? "PART %s :%s" : "PART %s", irc->channel, rest);
	} else if (!strcmp(verb, "msg") || !strcmp(verb, "query")
		|| !strcmp(verb, "m")) {
		firstWord(&rest, word, sizeof(word));

		if (!word[0] || !rest[0])
			info(irc, IRC_STYLE_ERROR, "/msg nick text");
		else
			say(irc, word, rest);
	} else if (!strcmp(verb, "r")) {
		if (!irc->lastPrivate[0])
			info(irc, IRC_STYLE_ERROR, "Nobody has messaged you yet");
		else if (rest[0])
			say(irc, irc->lastPrivate, rest);
	} else if (!strcmp(verb, "me")) {
		action(irc, current(irc), rest);
	} else if (!strcmp(verb, "nick")) {
		firstWord(&rest, word, sizeof(word));

		if (word[0]) {
			sendf(irc, "NICK %s", word);

			/* Before welcome nothing echoes it, so take it now. */
			if (!irc->registered)
				copy(irc->nick, sizeof(irc->nick), word);
		}
	} else if (!strcmp(verb, "topic") || !strcmp(verb, "t")) {
		if (!irc->inChannel)
			info(irc, IRC_STYLE_ERROR, "Not in a channel");
		else if (rest[0])
			sendf(irc, "TOPIC %s :%s", irc->channel, rest);
		else
			info(irc, IRC_STYLE_CHANGE, "Topic: %s",
				irc->topic[0] ? irc->topic : "(none)");
	} else if (!strcmp(verb, "whois") || !strcmp(verb, "w")) {
		sendf(irc, "WHOIS %s", rest);
	} else if (!strcmp(verb, "names")) {
		if (irc->inChannel)
			sendf(irc, "NAMES %s", irc->channel);
	} else if (!strcmp(verb, "notice")) {
		firstWord(&rest, word, sizeof(word));

		if (word[0] && rest[0] && !offline(irc)) {
			sendf(irc, "NOTICE %s :%s", word, rest);
			show(irc, IRC_STYLE_NOTICE, word, NULL, rest, false);
		}
	} else if (!strcmp(verb, "ctcp")) {
		firstWord(&rest, word, sizeof(word));

		char what[IRC_LINE_MAX];

		snprintf(what, sizeof(what), "%s", rest);

		for (char *w = what; *w && (*w != ' '); w++) {
			if ((*w >= 'a') && (*w <= 'z'))
				*w = (char) (*w - ('a' - 'A'));
		}

		if (word[0] && what[0])
			sendf(irc, "PRIVMSG %s :\001%s\001", word, what);
	} else if (!strcmp(verb, "away")) {
		sendf(irc, rest[0] ? "AWAY :%s" : "AWAY", rest);
	} else if (!strcmp(verb, "quit")) {
		sendf(irc, "QUIT :%s", rest[0] ? rest : "tinrom - IRC on a PlayStation");
		return IRC_INPUT_QUIT;
	} else if (!strcmp(verb, "clear")) {
		return IRC_INPUT_CLEAR;
	} else if (!strcmp(verb, "help")) {
		for (unsigned i = 0; i < sizeof(helpText) / sizeof(helpText[0]); i++)
			show(irc, IRC_STYLE_INFO, NULL, NULL, helpText[i], false);
	} else if (!strcmp(verb, "raw") || !strcmp(verb, "quote")) {
		if (rest[0])
			sendf(irc, "%s", rest);
	} else {
		/* Anything else is the server's business: /mode, /kick, /list... */
		for (char *v = verb; *v; v++) {
			if ((*v >= 'a') && (*v <= 'z'))
				*v = (char) (*v - ('a' - 'A'));
		}

		sendf(irc, rest[0] ? "%s %s" : "%s", verb, rest);
	}

	return IRC_INPUT_DONE;
}

/* --- tab completion -------------------------------------------------------- */

static uint32_t hashLine(const char *line, size_t cursor) {
	uint32_t hash = 0x811c9dc5u ^ (uint32_t) cursor;

	for (; *line; line++)
		hash = (hash ^ (uint8_t) *line) * 0x01000193u;

	return hash;
}

bool ircCompleteNick(IrcClient *irc, char *line, size_t size, size_t *cursor) {
	size_t length = strlen(line);

	if (*cursor > length)
		*cursor = length;

	/* The same line as the last completion left: cycle to the next match. */
	bool again = irc->completeHash && (irc->completeHash
		== hashLine(line, *cursor));

	if (!again) {
		size_t start = *cursor;

		while (start && isNickChar(line[start - 1]))
			start--;

		size_t typed = *cursor - start;

		if (!typed || (typed >= sizeof(irc->completeBase)))
			return false;

		memcpy(irc->completeBase, &line[start], typed);
		irc->completeBase[typed] = 0;
		irc->completeStart       = start;
		irc->completeNext        = 0;
	}

	static uint16_t order[IRC_NICKS];
	unsigned        count = ircSortedNicks(irc, order);

	for (unsigned tried = 0; tried < count; tried++) {
		unsigned       index = (irc->completeNext + tried) % count;
		const IrcNick *nick  = &irc->nicks[order[index]];

		if (!prefixMatches(nick->name, irc->completeBase)
			|| !ircCompare(nick->name, irc->nick))
			continue;

		/* At the start of the line it is an address: "nick: ". */
		char   insert[IRC_NICK_MAX + 2];
		size_t start = irc->completeStart;

		/* No second space when one already follows. */
		bool spaced = line[*cursor] == ' ';

		snprintf(insert, sizeof(insert), start ? (spaced ? "%s" : "%s ")
			: (spaced ? "%s:" : "%s: "), nick->name);

		size_t tail     = length - *cursor;
		size_t inserted = strlen(insert);

		if (start + inserted + tail + 1 > size)
			return false;

		memmove(&line[start + inserted], &line[*cursor], tail + 1);
		memcpy(&line[start], insert, inserted);
		*cursor            = start + inserted;
		irc->completeNext  = index + 1;
		irc->completeHash  = hashLine(line, *cursor);

		return true;
	}

	irc->completeHash = 0;

	return false;
}
