/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The SIO0 bus itself: framing, /CS (slot select) and the /ACK handshake.
 *
 * Controllers and memory cards share one bus, one data line and one RX FIFO.
 * They differ only in the address byte they answer to (01h vs 81h) and in how
 * long they take to acknowledge. Everything that talks to a peripheral goes
 * through here so that the bus is left in one known state between
 * transactions - a driver that leaves /CS asserted, or a byte sitting in the
 * FIFO, corrupts whatever runs next, and the symptom is a reply turning up
 * one transaction late.
 *
 * Framing and the /ACK behaviour are documented hardware; see psx-spx in
 * NOTICE.md. The polling approach follows ps1-bare-metal's controller example
 * (MIT, spicyjpeg).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* /ACK deadlines, in microseconds. */
#define TINROM_SIO0_ACK_PAD    300
#define TINROM_SIO0_ACK_CARD  1000
/* A card's write-back stalls for ~31000 cycles before acknowledging. */
#define TINROM_SIO0_ACK_WRITE 3000

#ifdef __cplusplus
extern "C" {
#endif

/** Resets SIO0 and leaves every slot deselected. */
void tinromSio0Init(void);

/**
 * Asserts /CS on `port` (0 or 1) and waits for the peripheral to wake up.
 * Flushes anything the previous transaction left behind first.
 */
void tinromSio0Select(unsigned int port);

/** Releases /CS. Always call this, including on the error paths. */
void tinromSio0Deselect(void);

/** Sends one byte and returns the one clocked in alongside it. */
uint8_t tinromSio0Exchange(uint8_t value);

/** Waits for /ACK (DSR). False means the peripheral never answered. */
bool tinromSio0WaitAck(unsigned int timeoutMicroseconds);

#ifdef __cplusplus
}
#endif
