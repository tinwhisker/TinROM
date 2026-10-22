/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The modem. See modem.h.
 *
 * Written from the Hayes command set as the WiFi adapters document it (the
 * result words, the +++ escape with a second's silence either side, ATH,
 * ATD with a host:port); see NOTICE.md. No adapter firmware was read.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "comms/modem.h"
#include "core/log.h"
#include "hal/timer.h"
#include "hal/uart_sio.h"
#include "hal/video.h"
#include "libc/string.h"

typedef enum {
	STEP_NONE = 0,
	STEP_INIT,         /* init string sent, waiting for OK */
	STEP_DIAL,         /* dial sent, waiting for CONNECT or why not */
	STEP_GUARD_BEFORE, /* silence before +++ */
	STEP_GUARD_AFTER,  /* silence after it */
	STEP_ATH,          /* ATH sent */
	STEP_DTR           /* DTR held low */
} Step;

static struct {
	CommsModemProfile profile;
	CommsModemState   state;
	CommsResult       result;
	Step              step;
	uint32_t          stepStart;
	uint32_t          lastSent;
	uint32_t          savedBaud;
	char              target[64];
	char              status[48];
	char              line[96];
	size_t            lineLength;
	CommsCarrierWatch watch, connectWatch;
	/* CONNECT ends in CR LF and the line reader stopped at the CR. */
	bool              skipLf;
	/* Connected at some point and not cleanly hung up since. */
	bool              wasOnline;
} modem;

/* --- time ---------------------------------------------------------------- */

static uint32_t now(void) {
	return tinromFrameCounter();
}

static uint32_t seconds(unsigned int count) {
	return count * (tinromVideoIsPAL() ? 50u : 60u);
}

static uint32_t sinceStep(void) {
	return now() - modem.stepStart;
}

static void startStep(Step step) {
	modem.step      = step;
	modem.stepStart = now();
}

/* --- sending ------------------------------------------------------------- */

static void send(const char *text) {
	tinromSioWriteString(text);
	modem.lastSent = now();
}

static void sendCommand(const char *command) {
	send(command);
	send("\r");
	modem.lineLength = 0;
}

static void setStatus(const char *text) {
	snprintf(modem.status, sizeof(modem.status), "%s", text);
}

/* --- parsing ------------------------------------------------------------- */

static bool startsWith(const char *text, const char *prefix) {
	return !strncmp(text, prefix, strlen(prefix));
}

CommsResult commsModemParseResult(const char *line) {
	/* Verbose result words first, then the numeric ones ATV0 gives. */
	static const struct {
		const char *word;
		CommsResult result;
	} words[] = {
		{ "OK",          COMMS_RESULT_OK },
		{ "CONNECT",     COMMS_RESULT_CONNECT },
		{ "NO CARRIER",  COMMS_RESULT_NO_CARRIER },
		{ "NO DIALTONE", COMMS_RESULT_NO_DIALTONE },
		{ "NO DIAL TONE", COMMS_RESULT_NO_DIALTONE },
		{ "NO ANSWER",   COMMS_RESULT_NO_ANSWER },
		{ "BUSY",        COMMS_RESULT_BUSY },
		{ "ERROR",       COMMS_RESULT_ERROR },
		/* Some adapters answer ATH with this instead of OK. */
		{ "HANGUP",      COMMS_RESULT_OK }
	};

	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		size_t length = strlen(words[i].word);

		/* "CONNECT 9600" counts; "OKAY" does not. */
		if (startsWith(line, words[i].word)
			&& ((line[length] == 0) || (line[length] == ' ')))
			return words[i].result;
	}

	if ((line[0] >= '0') && (line[0] <= '9')) {
		int code = 0;

		for (const char *c = line; *c; c++) {
			if ((*c < '0') || (*c > '9'))
				return COMMS_RESULT_NONE;

			code = code * 10 + (*c - '0');
		}

		switch (code) {
			case 0:  return COMMS_RESULT_OK;
			case 1:  return COMMS_RESULT_CONNECT;
			case 3:  return COMMS_RESULT_NO_CARRIER;
			case 4:  return COMMS_RESULT_ERROR;
			case 6:  return COMMS_RESULT_NO_DIALTONE;
			case 7:  return COMMS_RESULT_BUSY;
			case 8:  return COMMS_RESULT_NO_ANSWER;
			default: return (code >= 10) ? COMMS_RESULT_CONNECT
				: COMMS_RESULT_NONE;
		}
	}

	return COMMS_RESULT_NONE;
}

static bool watchFeed(CommsCarrierWatch *watch, uint8_t byte,
	const char *pattern) {
	bool lineStart = !watch->midLine;

	watch->midLine = (byte != '\r') && (byte != '\n');

	if (watch->matched && (byte == (uint8_t) pattern[watch->matched])) {
		if (!pattern[++watch->matched]) {
			watch->matched = 0;

			return true;
		}

		return false;
	}

	watch->matched = (lineStart && (byte == (uint8_t) pattern[0])) ? 1 : 0;

	return false;
}

bool commsCarrierWatchFeed(CommsCarrierWatch *watch, uint8_t byte) {
	return watchFeed(watch, byte, "NO CARRIER");
}

bool commsConnectWatchFeed(CommsCarrierWatch *watch, uint8_t byte) {
	return watchFeed(watch, byte, "CONNECT");
}

/*
 * Assembles what the modem says in command mode into lines. Returns the next
 * complete one, or NULL when there is none yet.
 */
static const char *nextLine(void) {
	uint8_t byte;

	while (tinromSioReceive(&byte, 1)) {
		if ((byte == '\r') || (byte == '\n')) {
			if (!modem.lineLength)
				continue;

			modem.line[modem.lineLength] = 0;
			modem.lineLength             = 0;

			return modem.line;
		}

		if ((byte >= 0x20) && (byte < 0x7f)
			&& (modem.lineLength + 1 < sizeof(modem.line)))
			modem.line[modem.lineLength++] = (char) byte;
	}

	return NULL;
}

/* --- the profile and the port ------------------------------------------- */

void commsModemDefaults(CommsModemProfile *profile) {
	memset(profile, 0, sizeof(*profile));

	/* Echo off, verbose results. Everything else is left as configured. */
	snprintf(profile->init, sizeof(profile->init), "ATE0V1");
	snprintf(profile->dialPrefix, sizeof(profile->dialPrefix), "ATDT");
	profile->baud   = 9600;
	profile->hangup = COMMS_HANGUP_ESCAPE;
}

void commsModemOpen(const CommsModemProfile *profile) {
	memset(&modem, 0, sizeof(modem));
	modem.profile   = *profile;
	modem.savedBaud = tinromSioBaud();

	tinromSioSetBaud(profile->baud ? profile->baud : 9600);
	tinromSioBufferStart();
	tinromSioSetCtsFlow(profile->ctsFlow);
	tinromSioSetDtr(true);

	modem.state = COMMS_MODEM_IDLE;
	setStatus("Ready");
	TINROM_LOG("modem: open at %u baud", (unsigned int) tinromSioBaud());
}

void commsModemClose(void) {
	if (modem.state == COMMS_MODEM_CLOSED)
		return;

	/* A dial left running would connect with nobody listening. */
	if (modem.state == COMMS_MODEM_DIALLING)
		send("\r");

	/*
	 * No time for the polite way: a dropped DTR is all there is. Also after
	 * a call that ended without a clean hang-up, in case the adapter is in
	 * fact still connected.
	 */
	if ((modem.state == COMMS_MODEM_ONLINE) || modem.wasOnline) {
		tinromSioSetDtr(false);
		tinromDelayMicroseconds(500000);
		tinromSioSetDtr(true);
	}

	tinromSioBufferStop();
	tinromSioSetBaud(modem.savedBaud);
	modem.state = COMMS_MODEM_CLOSED;
	TINROM_LOG("modem: closed");
}

CommsModemState commsModemState(void) {
	return modem.state;
}

CommsResult commsModemResult(void) {
	return modem.result;
}

const char *commsModemStatus(void) {
	return modem.status;
}

/* --- dialling and hanging up -------------------------------------------- */

static void beginDial(void) {
	char command[COMMS_PREFIX_MAX + sizeof(modem.target)];

	snprintf(command, sizeof(command), "%s%s", modem.profile.dialPrefix,
		modem.target);
	sendCommand(command);
	startStep(STEP_DIAL);
	setStatus("Dialling...");
}

void commsModemDial(const char *target) {
	if (modem.state != COMMS_MODEM_IDLE)
		return;

	snprintf(modem.target, sizeof(modem.target), "%s", target);
	modem.state  = COMMS_MODEM_DIALLING;
	modem.result = COMMS_RESULT_NONE;
	modem.watch.matched = 0;
	tinromSioFlushInput();

	if (modem.profile.init[0]) {
		sendCommand(modem.profile.init);
		startStep(STEP_INIT);
		setStatus("Initialising...");
	} else {
		beginDial();
	}

	TINROM_LOG("modem: dial %s", target);
}

static void finish(CommsModemState state, CommsResult result,
	const char *status) {
	modem.state  = state;
	modem.result = result;
	modem.step   = STEP_NONE;

	if (status)
		setStatus(status);
}

void commsModemHangup(void) {
	if (modem.state != COMMS_MODEM_ONLINE)
		return;

	modem.state = COMMS_MODEM_HANGING_UP;
	setStatus("Hanging up...");

	if (modem.profile.hangup == COMMS_HANGUP_DTR) {
		tinromSioSetDtr(false);
		startStep(STEP_DTR);
	} else {
		startStep(STEP_GUARD_BEFORE);
	}
}

void commsModemDirect(void) {
	if (modem.state != COMMS_MODEM_IDLE)
		return;

	modem.state        = COMMS_MODEM_DIRECT;
	modem.watch        = (CommsCarrierWatch) { 0 };
	modem.connectWatch = (CommsCarrierWatch) { 0 };
	setStatus("Direct to the modem");
}

void commsModemCancel(void) {
	switch (modem.state) {
		case COMMS_MODEM_DIALLING:
			/* Any character aborts a dial on a Hayes modem. */
			send("\r");
			finish(COMMS_MODEM_IDLE, COMMS_RESULT_CANCELLED, "Cancelled");
			break;

		case COMMS_MODEM_ONLINE:
			commsModemHangup();
			break;

		default:
			break;
	}
}

static void pollCommand(void) {
	const char *line;

	while ((line = nextLine())) {
		/* With echo on, the command comes back first. */
		if (startsWith(line, "AT") || startsWith(line, "at"))
			continue;

		CommsResult result = commsModemParseResult(line);

		switch (modem.step) {
			case STEP_INIT:
				/* An adapter that dislikes part of the init can still dial. */
				if (result == COMMS_RESULT_OK || result == COMMS_RESULT_ERROR) {
					if (result == COMMS_RESULT_ERROR)
						TINROM_LOG("modem: init string rejected");

					beginDial();
				}
				break;

			case STEP_DIAL:
				if (result == COMMS_RESULT_CONNECT) {
					finish(COMMS_MODEM_ONLINE, result, line);
					modem.skipLf    = true;
					modem.wasOnline = true;
					modem.watch     = (CommsCarrierWatch) { 0 };
					TINROM_LOG("modem: %s", line);

					return;
				}

				if (result != COMMS_RESULT_NONE && result != COMMS_RESULT_OK) {
					finish(COMMS_MODEM_IDLE, result, line);
					TINROM_LOG("modem: %s", line);

					return;
				}

				/* Anything else is the adapter narrating; show it. */
				if (result == COMMS_RESULT_NONE)
					setStatus(line);
				break;

			case STEP_ATH:
				if (result != COMMS_RESULT_NONE) {
					finish(COMMS_MODEM_IDLE, COMMS_RESULT_OK, "Hung up");
					modem.wasOnline = false;

					return;
				}
				break;

			default:
				break;
		}
	}
}

void commsModemPoll(void) {
	switch (modem.step) {
		case STEP_INIT:
			pollCommand();

			if ((modem.step == STEP_INIT) && (sinceStep() > seconds(5)))
				beginDial();
			break;

		case STEP_DIAL:
			pollCommand();

			if ((modem.step == STEP_DIAL) && (sinceStep() > seconds(60))) {
				send("\r");
				finish(COMMS_MODEM_IDLE, COMMS_RESULT_TIMEOUT, "No answer");
			}
			break;

		case STEP_GUARD_BEFORE: {
			uint8_t sink[64];

			/* Whatever is still arriving is of no interest now. */
			(void) tinromSioReceive(sink, sizeof(sink));

			if ((now() - modem.lastSent) > seconds(1) + 10) {
				send("+++");
				startStep(STEP_GUARD_AFTER);
			}
			break;
		}

		case STEP_GUARD_AFTER:
			if (sinceStep() > seconds(1) + 10) {
				tinromSioFlushInput();
				sendCommand("ATH");
				startStep(STEP_ATH);
			}
			break;

		case STEP_ATH:
			pollCommand();

			if ((modem.step == STEP_ATH) && (sinceStep() > seconds(5)))
				finish(COMMS_MODEM_IDLE, COMMS_RESULT_TIMEOUT,
					"Hung up (no reply)");
			break;

		case STEP_DTR:
			if (sinceStep() > seconds(1) / 2) {
				tinromSioSetDtr(true);
				tinromSioFlushInput();
				finish(COMMS_MODEM_IDLE, COMMS_RESULT_OK, "Hung up");
				modem.wasOnline = false;
			}
			break;

		default:
			if ((modem.state == COMMS_MODEM_ONLINE)
				&& modem.profile.carrierOnDsr && !tinromSioDsr())
				finish(COMMS_MODEM_IDLE, COMMS_RESULT_NO_CARRIER,
					"Carrier lost");
			break;
	}
}

/* --- online -------------------------------------------------------------- */

/*
 * NO CARRIER seen in the stream. With the carrier wired to DSR, that has the
 * last word: the text could be the far end's.
 */
static bool carrierGone(void) {
	return !modem.profile.carrierOnDsr || !tinromSioDsr();
}

size_t commsModemRead(uint8_t *out, size_t max) {
	if (modem.state == COMMS_MODEM_DIRECT) {
		size_t count = tinromSioReceive(out, max);

		for (size_t i = 0; i < count; i++) {
			if (modem.state == COMMS_MODEM_ONLINE) {
				/* What follows CONNECT is the far end's, watched as such. */
				if (commsCarrierWatchFeed(&modem.watch, out[i]) && carrierGone()) {
					finish(COMMS_MODEM_IDLE, COMMS_RESULT_NO_CARRIER,
						"NO CARRIER");

					return i + 1;
				}
			} else if (commsConnectWatchFeed(&modem.connectWatch, out[i])) {
				modem.state     = COMMS_MODEM_ONLINE;
				modem.result    = COMMS_RESULT_CONNECT;
				modem.wasOnline = true;
				modem.watch     = (CommsCarrierWatch) { 0, true };
				setStatus("CONNECT");
				TINROM_LOG("modem: connected by hand");
			}
		}

		return count;
	}

	if (modem.state != COMMS_MODEM_ONLINE)
		return 0;

	size_t count = tinromSioReceive(out, max);

	if (modem.skipLf && count) {
		modem.skipLf = false;

		if (out[0] == '\n')
			memmove(out, out + 1, --count);
	}

	for (size_t i = 0; i < count; i++) {
		if (commsCarrierWatchFeed(&modem.watch, out[i]) && carrierGone()) {
			finish(COMMS_MODEM_IDLE, COMMS_RESULT_NO_CARRIER, "NO CARRIER");
			TINROM_LOG("modem: NO CARRIER");

			/* The rest is the modem talking, not the far end. */
			return i + 1;
		}
	}

	return count;
}

void commsModemWrite(const void *data, size_t length) {
	if ((modem.state != COMMS_MODEM_ONLINE)
		&& (modem.state != COMMS_MODEM_DIRECT))
		return;

	tinromSioWrite(data, length);
	modem.lastSent = now();
}
