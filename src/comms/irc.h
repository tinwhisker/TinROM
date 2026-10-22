/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * An IRC client's protocol side: what the server's lines mean, what the
 * user's typing sends, and the one channel being talked in.
 *
 * Written from RFC 1459 and RFC 2812, with the ISUPPORT tokens (PREFIX,
 * CHANMODES, CHANTYPES) and CTCP as the community documents them
 * (modern.ircdocs.horse; see NOTICE.md). One channel at a time: joining
 * another leaves the current one. Private messages show in the same window.
 *
 * Pure C with no hardware. The session hands it lines without their CR LF
 * and gives it `send` to answer with and `print` to show things; printing
 * turns an IrcLine into screen text (comms/irctext.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IRC_LINE_MAX    512
#define IRC_PARAMS      15
#define IRC_NICK_MAX    32
#define IRC_CHANNEL_MAX 64
#define IRC_TOPIC_MAX   320
#define IRC_NICKS       400

typedef enum {
	IRC_STYLE_INFO = 0,    /* -!- the server, or tinrom, saying something */
	IRC_STYLE_MOTD,
	IRC_STYLE_ERROR,
	IRC_STYLE_MESSAGE,     /* <who> text */
	IRC_STYLE_OWN,         /* <me> text */
	IRC_STYLE_ACTION,      /* * who text */
	IRC_STYLE_OWN_ACTION,
	IRC_STYLE_NOTICE,      /* -who- text */
	IRC_STYLE_PRIVATE,     /* *who* text, to me alone */
	IRC_STYLE_PRIVATE_OUT, /* -> *who* text, from me alone */
	IRC_STYLE_JOIN,
	IRC_STYLE_PART,        /* parts, quits and kicks */
	IRC_STYLE_CHANGE       /* nick, topic and mode changes */
} IrcStyle;

typedef struct {
	IrcStyle    style;
	const char *who;       /* may be NULL */
	const char *where;     /* a channel other than the current one, or NULL */
	const char *text;      /* may hold mIRC formatting codes */
	bool        highlight; /* mentions me */
} IrcLine;

typedef struct {
	char prefix;           /* '@', '+' and so on, or 0 */
	char name[IRC_NICK_MAX];
} IrcNick;

typedef struct {
	const char *nick;
	const char *user;
	const char *realName;
	const char *serverPassword;   /* "" for none */
	const char *nickServPassword; /* "" for none */
	const char *channel;          /* "#chan" or "#chan key"; "" for none */
	bool        showJoins;
} IrcLogin;

/* What the session should do about what was typed. */
typedef enum {
	IRC_INPUT_DONE = 0,
	IRC_INPUT_CLEAR, /* /clear: wipe the screen */
	IRC_INPUT_QUIT   /* /quit was sent: hang up once the server lets go */
} IrcInputResult;

typedef struct {
	void (*send)(void *user, const char *line);
	void (*print)(void *user, const IrcLine *line);
	void  *user;
	const char *version; /* for CTCP VERSION */

	char     nick[IRC_NICK_MAX];
	bool     registered;
	bool     inChannel;
	char     channel[IRC_CHANNEL_MAX];
	char     topic[IRC_TOPIC_MAX];
	bool     showJoins;

	IrcNick  nicks[IRC_NICKS];
	unsigned nickCount;
	unsigned nicksDropped; /* names that did not fit */
	bool     namesDone;    /* the next 353 starts a fresh list */
	uint32_t nickSerial;   /* bumped whenever the list changes */

	/* ISUPPORT, with RFC 1459's defaults. */
	char     prefixModes[8];
	char     prefixChars[8];
	char     chanTypes[8];
	char     modesAlways[40]; /* channel modes that always take a parameter */
	char     modesOnSet[24];  /* ... only when set */

	char     lastPrivate[IRC_NICK_MAX];
	uint32_t now, lastCtcpReply;
	unsigned highlights;     /* counted up; the session clears it */

	/* Kept from the login until the server welcomes us. */
	char     wantNick[IRC_NICK_MAX];
	char     wantChannel[IRC_CHANNEL_MAX];
	char     nickServPassword[32];
	unsigned nickTries;

	/* Tab completion's cycle. */
	char     completeBase[IRC_NICK_MAX];
	size_t   completeStart;
	unsigned completeNext;
	uint32_t completeHash;
} IrcClient;

#ifdef __cplusplus
extern "C" {
#endif

void ircInit(IrcClient *irc, void (*send)(void *, const char *),
	void (*print)(void *, const IrcLine *), void *user);

/** Once connected: PASS, NICK and USER. Remembers what to do on welcome. */
void ircLogin(IrcClient *irc, const IrcLogin *login);

/** One line from the server, without CR LF. Changed in place. */
void ircHandleLine(IrcClient *irc, char *line);

/** What the user typed: a message, or a /command. */
IrcInputResult ircInput(IrcClient *irc, const char *text);

/** The frame counter, for rate-limiting CTCP replies. */
void ircTick(IrcClient *irc, uint32_t frame);

/** The connection is gone: back to unregistered, the channel forgotten. */
void ircDisconnected(IrcClient *irc);

/**
 * The nick list in display order - by rank, then name - as indices into
 * irc->nicks. Returns how many.
 */
unsigned ircSortedNicks(const IrcClient *irc, uint16_t *order);

/**
 * Tab completion: completes the word before `*cursor` in `line` from the
 * nick list, cycling on repeated calls with the same line. True if it did.
 */
bool ircCompleteNick(IrcClient *irc, char *line, size_t size, size_t *cursor);

/* --- the pure parts, for the host tests ------------------------------------ */

typedef struct {
	char        nick[IRC_NICK_MAX * 2]; /* the source's nick, or server */
	const char *source;                 /* all of it, or "" */
	const char *command;
	const char *params[IRC_PARAMS];
	unsigned    count;
} IrcMessage;

/** Splits `line` in place. False for an empty line. */
bool ircParse(char *line, IrcMessage *message);

/** Case-insensitive the RFC 1459 way, where []\~ are {}|^ in upper case. */
int  ircCompare(const char *a, const char *b);
/** Whether `text` mentions `nick` as a word. */
bool ircMentions(const char *text, const char *nick);

#ifdef __cplusplus
}
#endif
