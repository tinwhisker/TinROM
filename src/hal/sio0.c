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

static uint16_t portControl(unsigned int port) {
	return (uint16_t) (
		SIO_CR_TXEN | SIO_CR_RXEN | SIO_CR_DSRIEN
		| SIO_CR_DTR
		| (port ? SIO_CR_PORT_2 : SIO_CR_PORT_1)
	);
}

static void quiesce(void) {
	SIO_CR(0) = 0;

	while (SIO_SR(0) & SIO_SR_RXRDY)
		(void) SIO_DR(0);

	/* IRQ_STAT acknowledges by writing a zero to the bit. */
	IRQ_STAT = ~(1 << IRQ_SIO0);
}

void tinromSio0Init(void) {
	SIO_CR(0) = SIO_CR_INTRST;

	SIO_MR(0) = SIO_MR_BR_DIV1 | SIO_MR_CHLEN_8;
	SIO_BR(0) = F_CPU / SIO0_BAUD;

	quiesce();

	IRQ_MASK &= ~(1 << IRQ_SIO0);
}

void tinromSio0Select(unsigned int port) {
	quiesce();

	SIO_CR(0) = portControl(port);

	tinromDelayMicroseconds(SIO0_SETTLE_US);
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
	for (unsigned int elapsed = 0; elapsed < timeoutMicroseconds; elapsed += 10) {
		if (IRQ_STAT & (1 << IRQ_SIO0)) {
			IRQ_STAT     = ~(1 << IRQ_SIO0);
			SIO_CR(0) |= SIO_CR_ERRRST;

			return true;
		}

		tinromDelayMicroseconds(10);
	}

	return false;
}
