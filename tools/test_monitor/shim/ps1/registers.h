/* tinrom - (C) 2026 TinWhisker - MIT licensed
 * Host-test stand-in for ps1-bare-metal's registers.h: only what the monitor
 * core touches outside sio.c. */
#pragma once
#include <stdint.h>
extern volatile uint16_t fakeIrqMask;
#define IRQ_MASK fakeIrqMask
#define F_CPU 33868800
