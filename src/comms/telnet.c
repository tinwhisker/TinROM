/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Telnet negotiation. See telnet.h. Written from RFC 854, RFC 1091
 * (TERMINAL-TYPE) and RFC 1073 (NAWS).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "comms/telnet.h"

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

#define OPT_ECHO   1
#define OPT_SGA    3
#define OPT_TTYPE  24
#define OPT_NAWS   31

#define TTYPE_IS   0
#define TTYPE_SEND 1

#define HIS  1 /* the server's side of an option is on */
#define MINE 2 /* ours is */

enum {
	S_DATA = 0,
	S_IAC,
	S_OPTION, /* after WILL, WONT, DO or DONT */
	S_SB,
	S_SB_IAC
};

void commsTelnetInit(
	CommsTelnet *telnet, CommsTelnetSend send, uint8_t columns, uint8_t rows
) {
	*telnet = (CommsTelnet) { 0 };
	telnet->send    = send;
	telnet->columns = columns;
	telnet->rows    = rows;
}

static void reply(CommsTelnet *telnet, uint8_t verb, uint8_t option) {
	const uint8_t bytes[3] = { IAC, verb, option };

	telnet->send(bytes, 3);
}

static void sendWindowSize(CommsTelnet *telnet) {
	const uint8_t bytes[9] = {
		IAC, SB, OPT_NAWS, 0, telnet->columns, 0, telnet->rows, IAC, SE
	};

	telnet->send(bytes, sizeof(bytes));
}

static void sendTerminalType(CommsTelnet *telnet) {
	static const uint8_t bytes[] = {
		IAC, SB, OPT_TTYPE, TTYPE_IS, 'A', 'N', 'S', 'I', IAC, SE
	};

	telnet->send(bytes, sizeof(bytes));
}

static uint8_t *optionState(CommsTelnet *telnet, uint8_t option) {
	static uint8_t unknown;

	if (option < sizeof(telnet->options))
		return &telnet->options[option];

	/* Never on, so never acknowledged twice. */
	unknown = 0;

	return &unknown;
}

static void negotiate(CommsTelnet *telnet, uint8_t verb, uint8_t option) {
	uint8_t *state = optionState(telnet, option);

	switch (verb) {
		case WILL: {
			bool wanted = (option == OPT_ECHO) || (option == OPT_SGA);

			if (wanted && !(*state & HIS)) {
				*state |= HIS;
				reply(telnet, DO, option);
			} else if (!wanted && !(*state & 0x80)) {
				/* Refused once; a server that keeps asking gets silence. */
				*state |= 0x80;
				reply(telnet, DONT, option);
			}
			break;
		}

		case WONT:
			if (*state & HIS) {
				*state &= ~HIS;
				reply(telnet, DONT, option);
			}
			break;

		case DO: {
			bool willing = (option == OPT_TTYPE) || (option == OPT_NAWS)
				|| (option == OPT_SGA);

			if (willing) {
				if (!(*state & MINE)) {
					*state |= MINE;
					reply(telnet, WILL, option);
				}

				if (option == OPT_NAWS)
					sendWindowSize(telnet);
			} else if (!(*state & 0x40)) {
				*state |= 0x40;
				reply(telnet, WONT, option);
			}
			break;
		}

		case DONT:
			if (*state & MINE) {
				*state &= ~MINE;
				reply(telnet, WONT, option);
			}
			break;
	}
}

static void subnegotiation(CommsTelnet *telnet) {
	if ((telnet->sbOption == OPT_TTYPE) && telnet->sbLength
		&& (telnet->sb[0] == TTYPE_SEND))
		sendTerminalType(telnet);
}

size_t commsTelnetReceive(CommsTelnet *telnet, uint8_t *data, size_t length) {
	size_t out = 0;

	for (size_t i = 0; i < length; i++) {
		uint8_t byte = data[i];

		switch (telnet->state) {
			case S_DATA:
				if (byte == IAC)
					telnet->state = S_IAC;
				else
					data[out++] = byte;
				break;

			case S_IAC:
				switch (byte) {
					case IAC:
						data[out++]   = IAC; /* a literal 0FFh */
						telnet->state = S_DATA;
						break;

					case WILL: case WONT: case DO: case DONT:
						telnet->command = byte;
						telnet->state   = S_OPTION;
						break;

					case SB:
						telnet->sbLength = 0;
						telnet->sbOption = 0;
						telnet->state    = S_SB;
						break;

					default:
						/* NOP, GA, AYT and the rest: nothing to do. */
						telnet->state = S_DATA;
						break;
				}
				break;

			case S_OPTION:
				negotiate(telnet, telnet->command, byte);
				telnet->state = S_DATA;
				break;

			case S_SB:
				if (byte == IAC) {
					telnet->state = S_SB_IAC;
				} else if (!telnet->sbOption && !telnet->sbLength) {
					telnet->sbOption = byte;
					telnet->sbLength = 0;
					/* sbOption 0 is BINARY, never subnegotiated here. */
					if (!byte)
						telnet->sbOption = 0xff;
				} else if (telnet->sbLength < sizeof(telnet->sb)) {
					telnet->sb[telnet->sbLength++] = byte;
				}
				break;

			case S_SB_IAC:
				if (byte == SE) {
					subnegotiation(telnet);
					telnet->state = S_DATA;
				} else {
					/* IAC IAC inside a subnegotiation is a data byte. */
					if (telnet->sbLength < sizeof(telnet->sb))
						telnet->sb[telnet->sbLength++] = byte;

					telnet->state = S_SB;
				}
				break;
		}
	}

	return out;
}

void commsTelnetSendInput(
	CommsTelnet *telnet, const uint8_t *data, size_t length
) {
	static const uint8_t iacIac[2] = { IAC, IAC };
	static const uint8_t crNul[2]  = { '\r', 0 };

	for (size_t i = 0; i < length; i++) {
		if (data[i] == IAC) {
			telnet->send(iacIac, 2);
		} else if ((data[i] == '\r')
			&& !((i + 1 < length) && (data[i + 1] == '\n'))) {
			telnet->send(crNul, 2);
		} else {
			telnet->send(&data[i], 1);
		}
	}
}
