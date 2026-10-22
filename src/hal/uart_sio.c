/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * SIO1 polled driver. No interrupts: the serial server runs a tight loop
 * while a transfer is in progress, and the TTY mirror is low-volume. A link
 * that talks back unprompted - the terminal and IRC clients' modem - turns on
 * buffered receive instead, which is still polled; see uart_sio.h. If the
 * GDB stub ever needs to interrupt a running game, this grows an IRQ path -
 * see docs/roadmap.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hal/uart_sio.h"
#include "hal/timer.h"
#include "ps1/registers.h"

const TinromSioBaud tinromSioBaudTable[] = {
	{ 115200, "115200 (default)",   0x574f4c53 }, /* "SLOW" */
	{ 414720, "414720 (medium)",    0x4944454d }, /* "MEDI" */
	{ 518400, "518400 (fast)",      0x54534146 }, /* "FAST" */
	{ 921600, "921600 (superfast)", 0x414f4857 }  /* "WHOA" */
};

const unsigned int tinromSioBaudCount =
	sizeof(tinromSioBaudTable) / sizeof(tinromSioBaudTable[0]);

static uint32_t currentBaud;

/*
 * The control register is write-only in practice (reading it back is not
 * something to rely on), so the lines tinrom drives are kept here and every
 * write goes through setControl().
 */
static uint16_t control;

static void setControl(uint16_t value) {
	control   = value;
	SIO_CR(1) = value;
}

/* Buffered receive; see uart_sio.h. */
#define RING_SIZE 4096 /* a power of two */
#define RING_HIGH (RING_SIZE - 512) /* RTS drops above this */
#define RING_LOW  (RING_SIZE / 4)   /* and comes back below this */

static uint8_t           ring[RING_SIZE];
static volatile uint32_t ringHead, ringTail;
static bool              buffering, ctsFlow, holding;
static uint32_t          overruns;

void tinromSioInit(uint32_t baud) {
	SIO_CR(1) = SIO_CR_INTRST;

	SIO_MR(1) = SIO_MR_BR_DIV1 | SIO_MR_CHLEN_8 | SIO_MR_SB_1;
	/* No parity: that is SIO_MR_PE left clear, and there is no flag for it. */
	SIO_BR(1) = (uint16_t) (F_CPU / baud);
	setControl(SIO_CR_TXEN | SIO_CR_RXEN | SIO_CR_RTS | SIO_CR_DTR);

	IRQ_MASK &= ~(1 << IRQ_SIO1);

	currentBaud = baud;
}

TinromResult tinromSioSetBaud(uint32_t baud) {
	if (!baud)
		return TINROM_ERR_BAD_ARG;

	/* Let anything already in the shift register go out at the old rate. */
	while (!(SIO_SR(1) & SIO_SR_TXU))
		__asm__ volatile("");

	tinromSioInit(baud);

	return TINROM_OK;
}

/*
 * Read back from the divisor rather than remembered: the resident monitor
 * drives the same port with its own driver, and changes the rate itself when
 * nops asks it to while tinrom is halted.
 */
uint32_t tinromSioBaud(void) {
	uint16_t divisor = SIO_BR(1);

	if (!divisor)
		return currentBaud;

	uint32_t actual = F_CPU / divisor;

	for (unsigned int i = 0; i < tinromSioBaudCount; i++) {
		uint32_t nominal = tinromSioBaudTable[i].baud;
		uint32_t error   = (actual > nominal)
			? (actual - nominal) : (nominal - actual);

		/* The divisor is an integer, so the achieved rate is only close. */
		if (error <= (nominal / 50))
			return nominal;
	}

	return actual;
}

bool tinromSioReadReady(void) {
	return (SIO_SR(1) & SIO_SR_RXRDY) != 0;
}

bool tinromSioWriteReady(void) {
	return (SIO_SR(1) & SIO_SR_TXRDY) != 0;
}

void tinromSioWriteByte(uint8_t value) {
	/*
	 * Bytes keep arriving while one goes out, so the ring is kept fed. A
	 * byte takes 87us at 115200 baud: well inside the 0.7ms the FIFO lasts.
	 */
	if (ctsFlow) {
		for (unsigned int i = 0; (i < 20000) && !(SIO_SR(1) & SIO_SR_CTS); i++)
			tinromSioPump();
	}

	while (!tinromSioWriteReady())
		tinromSioPump();

	SIO_DR(1) = value;
}

uint8_t tinromSioReadByte(void) {
	while (!tinromSioReadReady())
		__asm__ volatile("");

	return SIO_DR(1);
}

bool tinromSioReadByteTimeout(uint8_t *out, unsigned int timeoutFrames) {
	/* One "frame" here is ~16ms of polling, not a video frame. */
	for (unsigned int i = 0; i <= timeoutFrames; i++) {
		for (unsigned int j = 0; j < 1000; j++) {
			if (tinromSioReadReady()) {
				*out = SIO_DR(1);

				return true;
			}
		}

		if (!timeoutFrames)
			break;

		tinromDelayMicroseconds(16);
	}

	return false;
}

void tinromSioWrite(const void *data, size_t length) {
	const uint8_t *bytes = (const uint8_t *) data;

	for (size_t i = 0; i < length; i++)
		tinromSioWriteByte(bytes[i]);
}

void tinromSioWriteString(const char *string) {
	while (*string)
		tinromSioWriteByte((uint8_t) *(string++));
}

TinromResult tinromSioRead(
	void *data, size_t length, unsigned int timeoutFrames
) {
	uint8_t *bytes = (uint8_t *) data;

	for (size_t i = 0; i < length; i++) {
		if (!tinromSioReadByteTimeout(&bytes[i], timeoutFrames))
			return TINROM_ERR_TIMEOUT;
	}

	return TINROM_OK;
}

void tinromSioFlushInput(void) {
	while (tinromSioReadReady())
		(void) SIO_DR(1);

	ringHead = ringTail = 0;
}

/* --- buffered receive -------------------------------------------------- */

static uint32_t ringUsed(void) {
	return (ringHead - ringTail) & (RING_SIZE - 1);
}

void tinromSioBufferStart(void) {
	tinromSioFlushInput();
	overruns  = 0;
	buffering = true;
	setControl(control | SIO_CR_RTS);
}

void tinromSioBufferStop(void) {
	buffering = false;
	holding   = false;
	ctsFlow   = false;
	ringHead  = ringTail = 0;
	setControl(control | SIO_CR_RTS);
}

bool tinromSioBuffering(void) {
	return buffering;
}

void tinromSioPump(void) {
	if (!buffering)
		return;

	uint16_t status;

	while ((status = SIO_SR(1)) & SIO_SR_RXRDY) {
		uint8_t  value = (uint8_t) SIO_DR(1);
		uint32_t next  = (ringHead + 1) & (RING_SIZE - 1);

		if (next == ringTail) {
			overruns++;
			continue;
		}

		ring[ringHead] = value;
		ringHead       = next;
	}

	/* The FIFO filled before anyone looked: those bytes are gone. */
	if (status & (SIO_SR_OE | SIO_SR_FE)) {
		overruns++;
		SIO_CR(1) = control | SIO_CR_ERRRST;
	}

	uint32_t used = ringUsed();

	if ((holding || (used > RING_HIGH)) && (control & SIO_CR_RTS))
		setControl(control & ~SIO_CR_RTS);
	else if (!holding && (used < RING_LOW) && !(control & SIO_CR_RTS))
		setControl(control | SIO_CR_RTS);
}

void tinromSioHold(bool hold) {
	holding = hold;
	tinromSioPump();
}

size_t tinromSioReceive(uint8_t *out, size_t max) {
	tinromSioPump();

	size_t count = 0;

	while ((count < max) && (ringTail != ringHead)) {
		out[count++] = ring[ringTail];
		ringTail     = (ringTail + 1) & (RING_SIZE - 1);
	}

	/* Let the far end go again as soon as there is room. */
	tinromSioPump();

	return count;
}

size_t tinromSioPending(void) {
	return ringUsed();
}

uint32_t tinromSioOverruns(void) {
	return overruns;
}

void tinromSioSetCtsFlow(bool enabled) {
	ctsFlow = enabled;
}

void tinromSioSetDtr(bool asserted) {
	setControl(asserted ? (control | SIO_CR_DTR) : (control & ~SIO_CR_DTR));
}

bool tinromSioDsr(void) {
	return (SIO_SR(1) & SIO_SR_DSR) != 0;
}
