/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * A Hayes-style modem on SIO1: in practice a WiFi-to-serial adapter that
 * takes ATDT host:port and opens a TCP connection.
 *
 * Nothing here blocks. Dialling and hanging up take seconds, and the screen
 * has to keep drawing and the pad has to keep cancelling, so each is started
 * with one call and moved along by commsModemPoll(), once a frame. While
 * online, commsModemRead() hands over what the far end sent, and watches it
 * for the modem's NO CARRIER.
 *
 * The adapters agree on the basics - AT, ATD, +++, ATH, the result words -
 * and differ on most else, so what is sent is a profile the user can change.
 * See docs/terminal.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	/* "+++", a pause, then "ATH". Works on everything. */
	COMMS_HANGUP_ESCAPE = 0,
	/* Drop DTR for half a second. Needs the adapter set to AT&D2 and a
	 * cable that wires DTR. Much quicker. */
	COMMS_HANGUP_DTR    = 1
} CommsHangup;

#define COMMS_INIT_MAX   48
#define COMMS_PREFIX_MAX 8

typedef struct {
	char     init[COMMS_INIT_MAX];       /* sent before dialling; may be "" */
	char     dialPrefix[COMMS_PREFIX_MAX];
	uint32_t baud;
	bool     ctsFlow;                    /* see tinromSioSetCtsFlow() */
	bool     carrierOnDsr;               /* a cable wiring DCD to DSR */
	uint8_t  hangup;                     /* CommsHangup */
} CommsModemProfile;

typedef enum {
	COMMS_MODEM_CLOSED = 0, /* port not ours */
	COMMS_MODEM_IDLE,       /* port ours, command mode, nothing going on */
	COMMS_MODEM_DIALLING,
	COMMS_MODEM_ONLINE,
	COMMS_MODEM_HANGING_UP,
	/* Typing straight at the modem, AT commands and all. A CONNECT in what
	 * it says makes it ONLINE, so an ATD typed by hand works too. */
	COMMS_MODEM_DIRECT
} CommsModemState;

/* How the last dial or hang-up ended. */
typedef enum {
	COMMS_RESULT_NONE = 0,
	COMMS_RESULT_CONNECT,
	COMMS_RESULT_OK,
	COMMS_RESULT_NO_CARRIER,
	COMMS_RESULT_BUSY,
	COMMS_RESULT_NO_ANSWER,
	COMMS_RESULT_NO_DIALTONE,
	COMMS_RESULT_ERROR,
	COMMS_RESULT_TIMEOUT,
	COMMS_RESULT_CANCELLED
} CommsResult;

#ifdef __cplusplus
extern "C" {
#endif

/** The profile a fresh settings block starts with. */
void commsModemDefaults(CommsModemProfile *profile);

/** Claims SIO1 at the profile's rate, with buffered receive. */
void commsModemOpen(const CommsModemProfile *profile);
/** Hangs up hard if online (DTR), and gives the port back as it was. */
void commsModemClose(void);

CommsModemState commsModemState(void);
CommsResult     commsModemResult(void);
/**
 * What the modem said last, or what it is doing - "CONNECT 9600",
 * "Initialising...". For a status line.
 */
const char     *commsModemStatus(void);

/** Sends the init string and then the dial prefix and `target`. */
void commsModemDial(const char *target);
/** From idle: pass everything both ways, for typing at the modem. */
void commsModemDirect(void);
/** Stops a dial in progress, or starts hanging up a connection. */
void commsModemCancel(void);
void commsModemHangup(void);

/** Once a frame. Moves dialling and hanging up along. */
void commsModemPoll(void);

/**
 * Online or direct: up to `max` bytes the far end (or the modem) sent. NO
 * CARRIER in the stream ends a connection - the text itself is passed on
 * too, so a terminal shows it. Returns 0 otherwise.
 */
size_t commsModemRead(uint8_t *out, size_t max);
void   commsModemWrite(const void *data, size_t length);

/*
 * The pure parts, for the host tests: what a line from the modem means, and
 * the stream watcher.
 */
CommsResult commsModemParseResult(const char *line);

/*
 * Watches a stream for one of the modem's words at the start of a line - so
 * a BBS that happens to print "NO CARRIER" mid-sentence does not end the
 * call. Zeroed, it counts the first byte as the start of a line.
 */
typedef struct {
	uint8_t matched;
	bool    midLine;
} CommsCarrierWatch;

/** True when `byte` completes "NO CARRIER". */
bool commsCarrierWatchFeed(CommsCarrierWatch *watch, uint8_t byte);
/** True when `byte` completes "CONNECT". */
bool commsConnectWatchFeed(CommsCarrierWatch *watch, uint8_t byte);

#ifdef __cplusplus
}
#endif
