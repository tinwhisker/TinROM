/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Just enough telnet (RFC 854) for a BBS on port 23.
 *
 * A WiFi modem usually passes the TCP stream straight through, so a telnet
 * server's option negotiation arrives as bytes. Left alone, IAC sequences
 * print as rubbish and the server waits for answers that never come. This
 * strips them out of what is received and answers them:
 *
 *   the server may ECHO and SUPPRESS-GO-AHEAD (character-at-a-time mode,
 *   which is what a BBS wants); tinrom will send TERMINAL-TYPE ("ANSI") and
 *   NAWS (80x25); everything else is refused.
 *
 * Answers are only sent when they change an option's state, which is what
 * stops two ends refusing each other forever (RFC 854's loop rule).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*CommsTelnetSend)(const uint8_t *data, size_t length);

typedef struct {
	uint8_t         state;
	uint8_t         command;
	uint8_t         sbOption;
	uint8_t         sbLength;
	uint8_t         sb[8];
	/* Per option: bit 0 the server's side is on, bit 1 ours is. */
	uint8_t         options[40];
	CommsTelnetSend send;
	uint8_t         columns, rows;
} CommsTelnet;

#ifdef __cplusplus
extern "C" {
#endif

void commsTelnetInit(
	CommsTelnet *telnet, CommsTelnetSend send, uint8_t columns, uint8_t rows
);

/**
 * Filters received bytes in place, answering negotiation through `send`.
 * Returns how many bytes are left for the terminal.
 */
size_t commsTelnetReceive(CommsTelnet *telnet, uint8_t *data, size_t length);

/**
 * Sends `length` bytes of user input: 0FFh doubled, and a lone CR followed by
 * NUL, as the NVT requires.
 */
void commsTelnetSendInput(
	CommsTelnet *telnet, const uint8_t *data, size_t length
);

#ifdef __cplusplus
}
#endif
