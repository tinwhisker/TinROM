/* tinrom - (C) 2026 TinWhisker - MIT licensed */
/* Serial over a socket, for live tests against real host tools. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "monitor/mon.h"
int sioFd = -1;
static int readByte(int timeoutMs) {
	struct pollfd p = { sioFd, POLLIN, 0 };
	if (poll(&p, 1, timeoutMs) <= 0) return -1;
	uint8_t b; if (read(sioFd, &b, 1) != 1) { fprintf(stderr, "[console] host went away\n"); exit(0); }
	monitorState.bytesIn++; return b;
}
void monSioPut(uint8_t v) { if (write(sioFd, &v, 1) != 1) exit(0); monitorState.bytesOut++; }
void monSioPutWord(uint32_t v) { for (int i = 0; i < 4; i++) monSioPut(v >> (i * 8)); }
void monSioPutString(const char *t) { while (*t) monSioPut(*t++); }
int monSioPoll(void) { return readByte(0); }
int monSioGet(uint32_t ticks) { return readByte(ticks >= MON_TIMEOUT_BYTE ? 500 : 100); }
int monSioGetBlocking(void) { return readByte(-1); }
bool monSioGetWord(uint32_t *o, uint32_t t) {
	uint32_t v = 0; for (int i = 0; i < 4; i++) { int c = monSioGet(t); if (c < 0) return false; v |= (uint32_t) c << (i*8); }
	*o = v; return true;
}
void monSioFlushInput(void) { while (readByte(30) >= 0) ; }
extern uint32_t lastBaud;
void monSioSetBaud(uint32_t b) { lastBaud = b; fprintf(stderr, "[console] baud %u\n", b); }
int consoleRead(int timeoutMs) { return readByte(timeoutMs); }
