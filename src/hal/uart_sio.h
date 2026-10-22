/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

/*
 * SIO1 (the serial/expansion port). Everything the nops-compatible server,
 * the GDB stub and the TTY mirror go through.
 *
 * The baud table matches what the host tooling expects to negotiate; see
 * docs/nops-protocol.md. 2073600 is the SIO1 base clock, so usable rates are
 * its integer divisions.
 */

#define TINROM_SIO_BASE_CLOCK 2073600

typedef struct {
	uint32_t    baud;
	const char *label;
	uint32_t    token; /* four-character token the host sends, little-endian */
} TinromSioBaud;

#ifdef __cplusplus
extern "C" {
#endif

extern const TinromSioBaud tinromSioBaudTable[];
extern const unsigned int  tinromSioBaudCount;

void         tinromSioInit(uint32_t baud);
TinromResult tinromSioSetBaud(uint32_t baud);
uint32_t     tinromSioBaud(void);

bool         tinromSioReadReady(void);
bool         tinromSioWriteReady(void);

void         tinromSioWriteByte(uint8_t value);
uint8_t      tinromSioReadByte(void);
/** Returns false on timeout. timeoutFrames of 0 means "don't block". */
bool         tinromSioReadByteTimeout(uint8_t *out, unsigned int timeoutFrames);

void         tinromSioWrite(const void *data, size_t length);
void         tinromSioWriteString(const char *string);
/** Reads exactly `length` bytes or fails. */
TinromResult tinromSioRead(void *data, size_t length, unsigned int timeoutFrames);

void         tinromSioFlushInput(void);

/*
 * Buffered receive, for a link that talks back: a modem, a BBS, an IRC
 * server.
 *
 * SIO1 holds eight received bytes. At 115200 baud that is 0.7ms, far less
 * than a frame, and tinrom polls. So while buffering is on, the received
 * bytes are moved into a 4 KB ring by tinromSioPump(), which is called from
 * the vsync wait (where a frame spends most of its time) and may be called
 * from anything else that runs long. RTS goes low while the ring is nearly
 * full and high again once it has drained, so a modem doing hardware flow
 * control stops sending in time; one that is not simply has bytes dropped
 * when the ring is full, and tinromSioOverruns() counts them.
 *
 * While buffering is on the port is claimed: the serial server stops
 * answering and the log mirror stops writing, since a byte from either would
 * go to the modem.
 */
void         tinromSioBufferStart(void);
void         tinromSioBufferStop(void);
bool         tinromSioBuffering(void);
/** Drain the hardware FIFO into the ring. Cheap when there is nothing. */
void         tinromSioPump(void);
/** Moves up to `max` buffered bytes into `out`; returns how many. */
size_t       tinromSioReceive(uint8_t *out, size_t max);
size_t       tinromSioPending(void);
/** Bytes lost, to a full ring or to the hardware FIFO overflowing. */
uint32_t     tinromSioOverruns(void);
/**
 * Holds RTS low regardless of the ring, for a stretch of work that may not
 * pump often enough - parsing a screenful, drawing it. A modem doing
 * hardware flow control stops within a byte or two, which the FIFO holds.
 */
void         tinromSioHold(bool hold);

/**
 * With this on, every byte sent waits for CTS first (up to a frame, then it
 * goes anyway). Only for a cable that wires CTS: an unwired one can read as
 * never clear to send.
 */
void         tinromSioSetCtsFlow(bool enabled);
/** DTR high or low. Modems set to AT&D2 hang up when it drops. */
void         tinromSioSetDtr(bool asserted);
/** The DSR input, which some cables wire to the modem's carrier detect. */
bool         tinromSioDsr(void);

#ifdef __cplusplus
}
#endif
