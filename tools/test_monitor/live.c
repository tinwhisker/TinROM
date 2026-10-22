/* tinrom - (C) 2026 TinWhisker - MIT licensed */
/* A pretend console: tinrom's serial server loop plus a toy CPU that walks
 * RAM treating every instruction as a nop until it meets a break. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "monitor/mon.h"
extern int sioFd; extern uint8_t ram[];
int consoleRead(int timeoutMs);
void monitorDispatch(void);
extern uint32_t breakInReturn;
#define F tinromExceptionFrame
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, &ram[a & 0x1fffff], 4); return v; }
static int pcdrvHook(TinromExceptionFrame *f, uint32_t fn) { return 0; }

/* Run the "program" from pc until a break, for at most n instructions. */
static void runProgram(uint32_t pc) {
	for (int n = 0; n < 4096; n++) {
		uint32_t w = rd32(pc);
		if ((w & 0xfc00003fu) == 0x0000000du) {
			fprintf(stderr, "[console] cpu: break %08x at %08x\n", w, pc);
			F.epc = pc; F.cause = TINROM_EXC_BP << 2;
			monitorDispatch();
			pc = F.epc;
			if ((pc & 0x1fffffff) >= 0x1e8000 && (pc & 0x1fffffff) < 0x1f0000) return; /* back into tinrom */
			continue;
		}
		pc += 4;
	}
	fprintf(stderr, "[console] cpu: ran off, back to tinrom\n");
}

int main(int argc, char **argv) {
	int port = atoi(argv[1]);
	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	bind(s, (void *) &a, sizeof(a)); listen(s, 1);
	fprintf(stderr, "[console] listening on %d\n", port);
	sioFd = accept(s, NULL, NULL);

	memset(ram, 0, 0x200000);
	uint32_t tcb = 0x8000e000u; memcpy(&ram[0x110], &tcb, 4);
	monitorState.tinromStart = 0x10000; monitorState.tinromEnd = 0x40000;
	monitorState.tinromIntact = 1; monitorState.pcdrv = pcdrvHook;
	if (argc > 2) monitorState.attachOnContact = 1;
	for (int i = 1; i < 32; i++) F.r[i] = 0x1000 + i;
	F.r[29] = 0x801ffe00u;

	for (;;) { /* tinrom's menu: nopsServerPoll */
		int c = consoleRead(-1);
		if (c == 36) { monInstallVector(); monitorState.armed = 1; monBreakInNow(TINROM_BREAKIN_RSP, c); }
		else if (c == 3) monBreakInNow(TINROM_BREAKIN_INTERRUPT, c);
		else if (c == '+' || c == '-') continue;
		else {
			uint32_t tok = c;
			for (int i = 1; i < 4; i++) { int d = consoleRead(200); if (d < 0) goto next; tok |= (uint32_t) d << (i * 8); }
			if (tok == 0x474e4950u) { uint32_t r = 0x474e4f50u; for (int i=0;i<4;i++) monSioPut(r >> (i*8)); }
			else if (!monNopsCommand(tok)) { uint32_t r = 0x50534e55u; for (int i=0;i<4;i++) monSioPut(r >> (i*8)); }
		}
		/* If the debugger moved pc out of tinrom, the loaded program runs. */
		if (breakInReturn && ((breakInReturn & 0x1fffffff) < 0x1e8000 || (breakInReturn & 0x1fffffff) >= 0x1f0000)) {
			uint32_t pc = breakInReturn; breakInReturn = 0;
			fprintf(stderr, "[console] resumed at %08x: running program\n", pc);
			runProgram(pc);
		}
		next: ;
	}
}
