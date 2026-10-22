/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Digital controller polling over SIO0.
 *
 * The packet framing (address 01h, command 42h, little-endian button word
 * with active-low bits) is documented hardware behaviour; see psx-spx in
 * NOTICE.md. The bus itself - /CS, framing, the /ACK handshake - lives in
 * hal/sio0.c, which memory cards share.
 *
 * STUB: analog sticks, multitap and non-digital pad types are not handled.
 * Only the digital button word is decoded.
 */

#include <stdbool.h>
#include <stdint.h>
#include "hal/pad.h"
#include "hal/sio0.h"

#define PAD_PORTS        2
#define PAD_REPEAT_DELAY 18
#define PAD_REPEAT_RATE   4

#define SIO0_ADDR_CONTROLLER 0x01
#define SIO0_CMD_POLL        0x42

typedef struct {
	uint16_t held;
	uint16_t pressed;
	uint16_t repeated;
	uint16_t previous;
	uint8_t  repeatTimer;
	bool     connected;
} PadState;

static PadState padState[PAD_PORTS];

static bool pollPort(unsigned int port, uint16_t *buttons) {
	tinromSio0Select(port);

	(void) tinromSio0Exchange(SIO0_ADDR_CONTROLLER);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_PAD)) {
		tinromSio0Deselect();

		return false;
	}

	uint8_t idLow = tinromSio0Exchange(SIO0_CMD_POLL);

	if (!tinromSio0WaitAck(TINROM_SIO0_ACK_PAD)) {
		tinromSio0Deselect();

		return false;
	}

	(void) tinromSio0Exchange(0x00); /* id high, ignored for now */
	tinromSio0WaitAck(TINROM_SIO0_ACK_PAD);

	uint8_t low  = tinromSio0Exchange(0x00);
	tinromSio0WaitAck(TINROM_SIO0_ACK_PAD);
	uint8_t high = tinromSio0Exchange(0x00);
	tinromSio0WaitAck(TINROM_SIO0_ACK_PAD);

	tinromSio0Deselect();

	/* Buttons are active low on the wire. */
	*buttons = (uint16_t) ~(((uint16_t) high << 8) | low);

	/*
	 * STUB: idLow's upper nibble is the number of extra half-words the pad
	 * wants to send (analog pads send four more). They are currently dropped,
	 * which is harmless for digital use but wrong for analog.
	 */
	(void) idLow;

	return true;
}

void tinromPadInit(void) {
	/* The bus is brought up by tinromHalInit() before we get here. */
	for (unsigned int port = 0; port < PAD_PORTS; port++) {
		padState[port].held        = 0;
		padState[port].pressed     = 0;
		padState[port].repeated    = 0;
		padState[port].previous    = 0;
		padState[port].repeatTimer = 0;
		padState[port].connected   = false;
	}
}

void tinromPadPoll(void) {
	for (unsigned int port = 0; port < PAD_PORTS; port++) {
		PadState *state   = &padState[port];
		uint16_t  buttons = 0;

		state->connected = pollPort(port, &buttons);

		if (!state->connected)
			buttons = 0;

		state->pressed  = (uint16_t) (buttons & ~state->previous);
		state->repeated = state->pressed;
		state->held     = buttons;

		if (!buttons || state->pressed) {
			state->repeatTimer = PAD_REPEAT_DELAY;
		} else if (state->repeatTimer) {
			state->repeatTimer--;

			if (!state->repeatTimer) {
				state->repeated    = buttons;
				state->repeatTimer = PAD_REPEAT_RATE;
			}
		}

		state->previous = buttons;
	}
}

uint16_t tinromPadHeld(unsigned int port) {
	return (port < PAD_PORTS) ? padState[port].held : 0;
}

uint16_t tinromPadPressed(unsigned int port) {
	return (port < PAD_PORTS) ? padState[port].pressed : 0;
}

uint16_t tinromPadRepeated(unsigned int port) {
	return (port < PAD_PORTS) ? padState[port].repeated : 0;
}

bool tinromPadConnected(unsigned int port) {
	return (port < PAD_PORTS) ? padState[port].connected : false;
}

bool tinromPadAbortHeld(void) {
	tinromPadPoll();

	for (unsigned int port = 0; port < PAD_PORTS; port++) {
		uint16_t held = padState[port].held;

		if ((held & TINROM_PAD_ABORT_COMBO) == TINROM_PAD_ABORT_COMBO)
			return true;
	}

	return false;
}
