/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

/*
 * Console side of the nops wire protocol. See docs/nops-protocol.md for the
 * format; this header is just the dispatch surface.
 *
 * IMPORTANT: this is a clean-room implementation written from a description
 * of the observable protocol. No NOTPSXSerial source is used. See NOTICE.md.
 */

/* Four-character tokens, stored little-endian so they compare as one word. */
#define NOPS_TOKEN(a, b, c, d) \
	((uint32_t) (a) | ((uint32_t) (b) << 8) | ((uint32_t) (c) << 16) \
	| ((uint32_t) (d) << 24))

typedef enum {
	NOPS_CMD_SEND_BIN   = NOPS_TOKEN('S','B','I','N'),
	NOPS_CMD_SEND_EXE   = NOPS_TOKEN('S','E','X','E'),
	NOPS_CMD_SEND_ROM   = NOPS_TOKEN('S','R','O','M'),
	NOPS_CMD_JUMP       = NOPS_TOKEN('J','U','M','P'),
	NOPS_CMD_CALL       = NOPS_TOKEN('C','A','L','L'),
	NOPS_CMD_DUMP       = NOPS_TOKEN('D','U','M','P'),
	NOPS_CMD_HEXDUMP    = NOPS_TOKEN('H','E','X','D'),
	NOPS_CMD_RESET      = NOPS_TOKEN('R','E','S','T'),
	NOPS_CMD_PING       = NOPS_TOKEN('P','I','N','G'),
	NOPS_CMD_DEBUG      = NOPS_TOKEN('D','E','B','G'),
	NOPS_CMD_HALT       = NOPS_TOKEN('H','A','L','T'),
	NOPS_CMD_CONTINUE   = NOPS_TOKEN('C','O','N','T'),
	NOPS_CMD_HOOK_READ  = NOPS_TOKEN('H','K','R','D'),
	NOPS_CMD_HOOK_WRITE = NOPS_TOKEN('H','K','W','R'),
	NOPS_CMD_HOOK_EXEC  = NOPS_TOKEN('H','K','E','X'),
	NOPS_CMD_UNHOOK     = NOPS_TOKEN('U','N','H','K'),
	NOPS_CMD_MC_DOWN    = NOPS_TOKEN('M','C','D','N'),
	NOPS_CMD_MC_UP      = NOPS_TOKEN('M','C','U','P'),
	NOPS_CMD_STDIN      = NOPS_TOKEN('S','T','D','I'),
	NOPS_CMD_VERSION2   = NOPS_TOKEN('U','P','V','2'),
	NOPS_CMD_VERSION3   = NOPS_TOKEN('U','P','V','3'),
	NOPS_CMD_SET_BAUD   = NOPS_TOKEN('B','A','U','D')
} NopsCommand;

/* Replies. */
#define NOPS_REPLY_OKAY  NOPS_TOKEN('O','K','A','Y')
#define NOPS_REPLY_PONG  NOPS_TOKEN('P','O','N','G')
#define NOPS_REPLY_HALTED NOPS_TOKEN('H','L','T','D')
#define NOPS_REPLY_MORE  NOPS_TOKEN('M','O','R','E')
#define NOPS_REPLY_CHECK NOPS_TOKEN('C','H','E','K')
#define NOPS_REPLY_ERROR NOPS_TOKEN('E','R','R','!')
#define NOPS_REPLY_UNSUPPORTED NOPS_TOKEN('U','N','S','P')
#define NOPS_REPLY_OKV2  NOPS_TOKEN('O','K','V','2')
#define NOPS_REPLY_OKV3  NOPS_TOKEN('O','K','V','3')
#define NOPS_REPLY_STOK  NOPS_TOKEN('S','T','O','K')
#define NOPS_REPLY_NOPE  NOPS_TOKEN('N','O','P','E')

#define NOPS_CHUNK_SIZE 2048

typedef struct {
	uint32_t commandsHandled;
	uint32_t bytesReceived;
	uint32_t bytesSent;
	uint32_t lastCommand;
	bool     halted;
} NopsServerState;

#ifdef __cplusplus
extern "C" {
#endif

void                   nopsServerInit(void);
/**
 * Non-blocking. Reads at most one command from SIO1 and services it.
 * Returns true if a command was handled this call.
 */
bool                   nopsServerPoll(void);
const NopsServerState *nopsServerStatus(void);

#ifdef __cplusplus
}
#endif
