/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The monitor's own serial driver. It cannot use tinrom's: that may have
 * been overwritten by the program being debugged.
 *
 * SIO1 is polled, exactly as tinrom's driver does it (hal/uart_sio.c), and
 * the same 8N1 framing and baud divisor are used so the two can hand the port
 * back and forth without the host noticing.
 */

#include "monitor/mon.h"
#include "ps1/registers.h"

void monSioPut(uint8_t value) {
	while (!(SIO_SR(1) & SIO_SR_TXRDY))
		__asm__ volatile("");

	SIO_DR(1) = value;
	monitorState.bytesOut++;
}

void monSioPutWord(uint32_t value) {
	for (unsigned int i = 0; i < 4; i++)
		monSioPut((uint8_t) (value >> (i * 8)));
}

void monSioPutString(const char *text) {
	while (*text)
		monSioPut((uint8_t) *(text++));
}

int monSioPoll(void) {
	uint16_t status = SIO_SR(1);

	/*
	 * An overrun or framing error latches until acknowledged, and a latched
	 * error can stop the receiver reporting new data. Clearing it costs one
	 * byte that was already lost.
	 */
	if (status & (SIO_SR_OE | SIO_SR_FE | SIO_SR_PERROR))
		SIO_CR(1) |= SIO_CR_ERRRST;

	if (!(status & SIO_SR_RXRDY))
		return -1;

	monitorState.bytesIn++;

	return SIO_DR(1);
}

int monSioGet(uint32_t ticks) {
	do {
		int c = monSioPoll();

		if (c >= 0)
			return c;
	} while (ticks--);

	return -1;
}

int monSioGetBlocking(void) {
	for (;;) {
		int c = monSioPoll();

		if (c >= 0)
			return c;
	}
}

bool monSioGetWord(uint32_t *out, uint32_t ticks) {
	uint32_t value = 0;

	for (unsigned int i = 0; i < 4; i++) {
		int c = monSioGet(ticks);

		if (c < 0)
			return false;

		value |= (uint32_t) c << (i * 8);
	}

	*out = value;

	return true;
}

void monSioFlushInput(void) {
	/* Drain until the line has been quiet for a moment, not just empty now. */
	while (monSioGet(MON_TIMEOUT_SHORT / 4) >= 0)
		;
}

void monSioSetBaud(uint32_t baud) {
	if (!baud)
		return;

	/* Let anything already in the shift register go out at the old rate. */
	while (!(SIO_SR(1) & SIO_SR_TXU))
		__asm__ volatile("");

	SIO_CR(1) = SIO_CR_INTRST;
	SIO_MR(1) = SIO_MR_BR_DIV1 | SIO_MR_CHLEN_8 | SIO_MR_SB_1;
	SIO_BR(1) = (uint16_t) (F_CPU / baud);
	SIO_CR(1) = SIO_CR_TXEN | SIO_CR_RXEN | SIO_CR_RTS | SIO_CR_DTR;
}
