/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdbool.h>
#include <stdint.h>
#include "hal/sio0.h"
#include "hal/timer.h"
#include "ps1/registers.h"

/*
 * Slot select is SIO_CR bit 13, and it only means anything while /CS
 * (SIO_CR_DTR - "DTR is /CS on SIO0") is asserted. Clearing bit 1 does not
 * select port 1; it selects nothing at all, and then the data line floats.
 */
#define SIO0_BAUD      250000
#define SIO0_SETTLE_US 60

/*
 * Nothing on this bus may spin forever. A byte at 250 kHz takes 32 us, so a
 * peripheral that has not moved the status flag after tens of thousands of
 * tight reads is not going to. Returning FFh on a timeout is enough: the ID,
 * acknowledge and checksum comparisons downstream all reject it, and the
 * command unwinds through the caller's normal error path.
 */
#define SIO0_SPIN_LIMIT 20000

/*
 * RXEN is set once and never cleared. On real hardware, setting it makes the
 * port receive one byte on its own ("force receive single byte", psx-spx),
 * so a driver that wrote it afresh on every select - as this one did - starts
 * every transaction with a junk byte queued ahead of the real replies, and
 * reads each of them one byte late: on a pad, the low button byte lands where
 * the high one belongs. pcsx-redux does not model it. ps1-bare-metal's own
 * driver sets it once at init too, and drains the FIFO before each packet.
 */
#define SIO0_IDLE (SIO_CR_TXEN | SIO_CR_RXEN | SIO_CR_DSRIEN)

static uint16_t portSelect(unsigned int port) {
	return (uint16_t) (port ? SIO_CR_PORT_2 : SIO_CR_PORT_1);
}

static void drain(void) {
	while (SIO_SR(0) & SIO_SR_RXRDY)
		(void) SIO_DR(0);
}

/*
 * A device acknowledges a byte by holding /ACK low for a few microseconds,
 * which the port latches in SIO_SR bit 9 and passes on to IRQ_STAT. Both
 * have to be cleared before the next byte, and in this order:
 *
 * - Not while /ACK is still low. The port sets bit 9 again at once if it
 *   is, so the next wait sees an acknowledge that never happened, and the
 *   real one after it is lost behind the flag that never cleared - on a
 *   console, an empty slot answered FFh and a card in the other read as not
 *   there. The old wait was 20 register reads, 3-4 us at 33 MHz, which is
 *   about the pulse itself. pcsx-redux makes the pulse instant.
 * - The port's flag first, then IRQ_STAT, so the line is low by the time
 *   its latch is cleared.
 */
#define SIO0_ACK_RELEASE_US 100

static void clearAcknowledge(void) {
	for (unsigned int waited = 0; waited < SIO0_ACK_RELEASE_US; waited++) {
		if (!(SIO_SR(0) & SIO_SR_DSR))
			break;

		tinromDelayMicroseconds(1);
	}

	SIO_CR(0) |= SIO_CR_ERRRST;

	/* IRQ_STAT acknowledges by writing a zero to the bit. */
	IRQ_STAT = ~(1 << IRQ_SIO0);
}

/* /CS off, the receive FIFO empty and any pending acknowledge cleared. */
static void quiesce(void) {
	SIO_CR(0) = SIO0_IDLE | (SIO_CR(0) & SIO_CR_PORT_BITMASK);

	drain();
	clearAcknowledge();
}

void tinromSio0Init(void) {
	SIO_CR(0) = SIO_CR_INTRST;

	SIO_MR(0) = SIO_MR_BR_DIV1 | SIO_MR_CHLEN_8;
	SIO_BR(0) = F_CPU / SIO0_BAUD;
	SIO_CR(0) = SIO0_IDLE;

	/* Let the byte RXEN forces in arrive (32 us at 250 kHz), then drop it. */
	tinromDelayMicroseconds(SIO0_SETTLE_US);
	quiesce();

	IRQ_MASK &= ~(1 << IRQ_SIO0);
}

void tinromSio0Select(unsigned int port) {
	/* Choose the slot with /CS still off, then assert it. */
	SIO_CR(0) = SIO0_IDLE | portSelect(port);
	quiesce();

	SIO_CR(0) |= SIO_CR_DTR;

	tinromDelayMicroseconds(SIO0_SETTLE_US);

	/* Nothing has been sent yet, so anything here is not a reply. */
	drain();
}

void tinromSio0Deselect(void) {
	quiesce();

	/* /CS has to stay high long enough for the next select to be seen. */
	tinromDelayMicroseconds(SIO0_SETTLE_US);
}

static bool waitStatus(uint16_t mask) {
	for (unsigned int spins = 0; spins < SIO0_SPIN_LIMIT; spins++) {
		if (SIO_SR(0) & mask)
			return true;

		__asm__ volatile("");
	}

	return false;
}

uint8_t tinromSio0Exchange(uint8_t value) {
	if (!waitStatus(SIO_SR_TXRDY))
		return 0xff;

	SIO_DR(0) = value;

	if (!waitStatus(SIO_SR_RXRDY))
		return 0xff;

	return SIO_DR(0);
}

bool tinromSio0WaitAck(unsigned int timeoutMicroseconds) {
	for (unsigned int elapsed = 0; elapsed < timeoutMicroseconds; elapsed += 5) {
		/* The port's own latch, or what it passed on: either is the pulse. */
		if ((SIO_SR(0) & SIO_SR_IRQ) || (IRQ_STAT & (1 << IRQ_SIO0))) {
			clearAcknowledge();

			return true;
		}

		tinromDelayMicroseconds(5);
	}

	return false;
}
