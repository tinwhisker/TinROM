/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Clean-room console-side implementation of the nops wire protocol.
 * Written from the protocol description in docs/nops-protocol.md; no
 * NOTPSXSerial source was used. See NOTICE.md.
 *
 * This is the half that runs while tinrom's menu is up. The debug commands
 * and the memory transfers they are built from (DEBG, HALT, CONT, HKEX,
 * UNHK, DUMP, SBIN and the rate tokens) are the resident monitor's: it has to
 * answer exactly the same way while tinrom is halted, so it answers them in
 * both states and this file hands them over. See docs/debugger.md.
 *
 * The same port also carries GDB's own protocol. A GDB packet starts with
 * '$' and no nops command does, so the first byte is enough to tell them
 * apart; a packet halts tinrom in the debugger, which then reads the rest.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "hal/debugger.h"
#include "hal/uart_sio.h"
#include "modules/siocons/nops_server.h"

#define ARG_TIMEOUT_FRAMES     8 /* once a command starts, wait for its args */

static NopsServerState state;

static void writeToken(uint32_t token) {
	tinromSioWrite(&token, sizeof(token));
}

static bool readWord(uint32_t *out) {
	return tinromSioRead(out, sizeof(*out), ARG_TIMEOUT_FRAMES) == TINROM_OK;
}

static void handleSendExecutable(void) {
	/*
	 * STUB. The host sends the 800h-byte PS-EXE header first; tinrom needs to
	 * parse it (load address, size, PC, initial SP), receive the body with
	 * receiveBlock(), shut the UI and the modules down, flush the caches and
	 * jump. The handoff hook on TinromContext exists for exactly this.
	 */
	writeToken(NOPS_REPLY_UNSUPPORTED);
}

static void handleVersion(uint32_t requested) {
	/*
	 * The host announces which protocol revision it speaks; tinrom answers
	 * with the matching OKVn so the host knows it is talking to something
	 * that understands it.
	 */
	writeToken(
		(requested == NOPS_CMD_VERSION3) ? NOPS_REPLY_OKV3 : NOPS_REPLY_OKV2
	);
}

static void handleSetBaud(void) {
	uint32_t token;

	if (!readWord(&token)) {
		writeToken(NOPS_REPLY_NOPE);

		return;
	}

	for (unsigned int i = 0; i < tinromSioBaudCount; i++) {
		if (tinromSioBaudTable[i].token != token)
			continue;

		/* Acknowledge at the old rate, then switch. */
		writeToken(NOPS_REPLY_OKAY);
		tinromSioSetBaud(tinromSioBaudTable[i].baud);
		TINROM_LOG("nops: baud -> %u", (unsigned int) tinromSioBaudTable[i].baud);

		return;
	}

	writeToken(NOPS_REPLY_NOPE);
}

void nopsServerInit(void) {
	state.commandsHandled = 0;
	state.bytesReceived   = 0;
	state.bytesSent       = 0;
	state.lastCommand     = 0;
	state.halted          = false;

	tinromSioFlushInput();
}

bool nopsServerPoll(void) {
	/*
	 * Only start reading if a byte is already waiting, so the UI is never
	 * stalled by a quiet port.
	 */
	if (!tinromSioReadReady())
		return false;

	uint8_t first = tinromSioReadByte();

	switch (first) {
		case '$':
			/* GDB. The monitor reads the rest of the packet, halted. */
			state.commandsHandled++;
			tinromDebugHalt(TINROM_BREAKIN_RSP, first);
			return true;

		case 0x03:
			/* GDB's Ctrl-C, sent when it thinks the target is running. */
			tinromDebugHalt(TINROM_BREAKIN_INTERRUPT, first);
			return true;

		case '+':
		case '-':
			/* GDB acknowledging a packet. Nothing to do. */
			return true;

		default:
			break;
	}

	uint8_t rest[3];

	if (tinromSioRead(rest, sizeof(rest), ARG_TIMEOUT_FRAMES) != TINROM_OK)
		return false;

	uint32_t command = (uint32_t) first | ((uint32_t) rest[0] << 8)
		| ((uint32_t) rest[1] << 16) | ((uint32_t) rest[2] << 24);

	state.lastCommand = command;
	state.commandsHandled++;

	switch (command) {
		case NOPS_CMD_PING:
			writeToken(NOPS_REPLY_PONG);
			break;

		case NOPS_CMD_VERSION2:
		case NOPS_CMD_VERSION3:
			handleVersion(command);
			break;

		case NOPS_CMD_SET_BAUD:
			handleSetBaud();
			break;

		case NOPS_CMD_SEND_EXE:
			handleSendExecutable();
			break;

		/* Everything below needs a subsystem that is not built yet. */
		case NOPS_CMD_SEND_ROM:   /* needs hal/exp flashing            */
		case NOPS_CMD_JUMP:       /* needs the handoff path            */
		case NOPS_CMD_CALL:
		case NOPS_CMD_RESET:
		case NOPS_CMD_HEXDUMP:
		case NOPS_CMD_MC_DOWN:    /* needs hal/mcard                   */
		case NOPS_CMD_MC_UP:
		case NOPS_CMD_STDIN:
			writeToken(NOPS_REPLY_UNSUPPORTED);
			break;

		default:
			/* The debug commands, DUMP, SBIN and the rate tokens. */
			if (tinromDebugNopsCommand(command))
				break;

			/* Garbage or a newer host. Resynchronise rather than guess. */
			tinromSioFlushInput();
			writeToken(NOPS_REPLY_UNSUPPORTED);
			break;
	}

	return true;
}

const NopsServerState *nopsServerStatus(void) {
	return &state;
}
