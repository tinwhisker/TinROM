/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include <stdint.h>
#include "common/gpu.h"
#include "hal/timer.h"
#include "hal/uart_sio.h"
#include "ps1/delay.h"
#include "ps1/registers.h"

static uint32_t frameCounter;

void tinromTimerInit(void) {
	frameCounter = 0;
}

uint32_t tinromFrameCounter(void) {
	return frameCounter;
}

void tinromWaitVSync(void) {
	/*
	 * ps1-bare-metal's waitForVSync(), with the serial ring fed while it
	 * waits: most of a frame is spent here, and buffered receive (see
	 * uart_sio.h) depends on being pumped often.
	 */
	while (!(IRQ_STAT & (1 << IRQ_VSYNC)))
		tinromSioPump();

	IRQ_STAT = ~(1 << IRQ_VSYNC);
	frameCounter++;
}

void tinromWaitNextVSync(void) {
	if (IRQ_STAT & (1 << IRQ_VSYNC)) {
		IRQ_STAT = ~(1 << IRQ_VSYNC);
		frameCounter++;
	}

	tinromWaitVSync();
}

uint32_t tinromTimerNoise(void) {
	uint32_t noise = frameCounter;

	for (unsigned int i = 0; i < 3; i++)
		noise = (noise << 11) ^ (noise >> 21) ^ TIMER_COUNT(i);

	return noise;
}

void tinromDelayMicroseconds(int microseconds) {
	/*
	 * ps1-bare-metal's ps1/delay.h. This used to be a copy of the same loop,
	 * lifted from one of its examples before the header existed; the two were
	 * checked to produce identical cycle counts for every delay from 0 to 2s
	 * before this delegated to it.
	 */
	delayMicroseconds(microseconds);
}
