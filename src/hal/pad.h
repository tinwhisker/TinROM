/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Bit layout matches the digital-pad response word, inverted to active-high. */
typedef enum {
	TINROM_PAD_SELECT   = 1 <<  0,
	TINROM_PAD_L3       = 1 <<  1,
	TINROM_PAD_R3       = 1 <<  2,
	TINROM_PAD_START    = 1 <<  3,
	TINROM_PAD_UP       = 1 <<  4,
	TINROM_PAD_RIGHT    = 1 <<  5,
	TINROM_PAD_DOWN     = 1 <<  6,
	TINROM_PAD_LEFT     = 1 <<  7,
	TINROM_PAD_L2       = 1 <<  8,
	TINROM_PAD_R2       = 1 <<  9,
	TINROM_PAD_L1       = 1 << 10,
	TINROM_PAD_R1       = 1 << 11,
	TINROM_PAD_TRIANGLE = 1 << 12,
	TINROM_PAD_CIRCLE   = 1 << 13,
	TINROM_PAD_CROSS    = 1 << 14,
	TINROM_PAD_SQUARE   = 1 << 15
} TinromPadButton;

/* Held at boot, this skips autoboot. */
#define TINROM_PAD_ABORT_COMBO (TINROM_PAD_SELECT | TINROM_PAD_START)

#ifdef __cplusplus
extern "C" {
#endif

void     tinromPadInit(void);
/** Poll both ports. Call once per frame, before reading state. */
void     tinromPadPoll(void);

uint16_t tinromPadHeld(unsigned int port);
/** Rising edge since the previous poll. */
uint16_t tinromPadPressed(unsigned int port);
/** Rising edge plus key repeat, for menu navigation. */
uint16_t tinromPadRepeated(unsigned int port);
bool     tinromPadConnected(unsigned int port);

/** Either port holding the abort combo. Used before autoboot. */
bool     tinromPadAbortHeld(void);

#ifdef __cplusplus
}
#endif
