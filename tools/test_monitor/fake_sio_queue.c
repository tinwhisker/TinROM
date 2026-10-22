/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor/mon.h"
/* --- serial --- */
uint8_t in[1 << 20]; size_t inHead, inTail;
uint8_t out[1 << 20]; size_t outLen;
jmp_buf starved; int starvedArmed;

void monSioPut(uint8_t v) { out[outLen++] = v; monitorState.bytesOut++; }
void monSioPutWord(uint32_t v) { for (int i = 0; i < 4; i++) monSioPut(v >> (i * 8)); }
void monSioPutString(const char *t) { while (*t) monSioPut(*t++); }
int monSioPoll(void) { return (inHead < inTail) ? in[inHead++] : -1; }
int monSioGet(uint32_t ticks) { return monSioPoll(); }
int monSioGetBlocking(void) {
	int c = monSioPoll();
	if (c < 0) { if (starvedArmed) longjmp(starved, 1); fprintf(stderr, "starved\n"); abort(); }
	return c;
}
bool monSioGetWord(uint32_t *o, uint32_t t) {
	uint32_t v = 0; for (int i = 0; i < 4; i++) { int c = monSioGet(t); if (c < 0) return false; v |= (uint32_t) c << (i*8); }
	*o = v; return true;
}
void monSioFlushInput(void) { inHead = inTail; }
extern uint32_t lastBaud;
void monSioSetBaud(uint32_t b) { lastBaud = b; }

