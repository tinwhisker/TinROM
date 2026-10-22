/* tinrom - (C) 2026 TinWhisker - MIT licensed */
/* Host-side fakes for the monitor: memory, serial, the handler's entry. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor/mon.h"

volatile uint16_t fakeIrqMask;
TinromExceptionFrame tinromExceptionFrame;
uint8_t _monitorImageEnd[1], _monitorBssEnd[1];
void tinromExceptionHandler(void) {}

uint8_t ram[0x200000];
uint8_t rom[0x80000];
static uint8_t dbgconByte;
int flushes;
uint32_t lastBaud;
uint32_t lastBaud;

/* Map a CPU address to fake storage, or NULL if nothing is there. */
static uint8_t *map(uint32_t a) {
	if (a >= 0xc0000000u) return NULL;
	uint32_t p = a & 0x1fffffffu;
	if (p < 0x00800000u) return &ram[p & 0x1fffff];
	if (p >= 0x1fc00000u && p < 0x1fc80000u) return &rom[p - 0x1fc00000u];
	return NULL;
}
volatile uint32_t *monTestRam32(uint32_t a) {
	if (a == 0x1f802080u) return (volatile uint32_t *) &dbgconByte;
	uint8_t *p = map(a); if (!p) { fprintf(stderr, "raw access to %08x\n", a); abort(); }
	return (volatile uint32_t *) p;
}
volatile uint8_t *monTestRam8(uint32_t a) {
	if (a == 0x1f802080u) return &dbgconByte;
	return (volatile uint8_t *) monTestRam32(a);
}
int monitorPeek(uint32_t a, uint32_t size, uint32_t *out) {
	uint8_t *p = map(a); if (!p) return 0;
	uint32_t v = 0; if (size != 2 && size != 4) size = 1;
	memcpy(&v, p, size); *out = v; return 1;
}
int monitorPoke(uint32_t a, uint32_t size, uint32_t value) {
	uint8_t *p = map(a); if (!p) return 0;
	if (p >= rom && p < rom + sizeof(rom)) return 1; /* ROM ignores writes */
	if (size != 2 && size != 4) size = 1;
	memcpy(p, &value, size); return 1;
}
void flushCache(void) { flushes++; }

/* --- the break tinrom executes to halt itself --- */
#define BREAKIN_ADDR 0x801e9000u
uint32_t breakInReturn;
void monitorDispatch(void);
void monitorBreakInstruction(void) {
	uint32_t w = 0x03fe000du; memcpy(&ram[BREAKIN_ADDR & 0x1fffff], &w, 4);
	tinromExceptionFrame.epc   = BREAKIN_ADDR;
	tinromExceptionFrame.cause = TINROM_EXC_BP << 2;
	monitorDispatch();
	breakInReturn = tinromExceptionFrame.epc;
}
