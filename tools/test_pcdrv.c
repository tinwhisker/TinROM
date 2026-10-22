/* tinrom - host test for the PCDRV console side.
 *
 * Links the real src/core/pcdrv.c against a fake SIO1 that is a pair of
 * pipes to the Python host server, so both halves of the protocol are
 * exercised against each other rather than against my idea of each other.
 * See scripts/test-pcdrv.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core/pcdrv.h"

/* --- the fake cable ----------------------------------------------------- */

int toHost = -1, fromHost = -1;

void tinromSioWrite(const void *data, size_t length) {
	if (write(toHost, data, length) != (ssize_t) length)
		abort();
}

void tinromSioFlushInput(void) {}

TinromResult tinromSioRead(void *data, size_t length, unsigned int timeout) {
	(void) timeout;
	uint8_t *out = data;
	size_t got = 0;
	while (got < length) {
		ssize_t n = read(fromHost, out + got, length - got);
		if (n <= 0) return TINROM_ERR_TIMEOUT;
		got += (size_t) n;
	}
	return TINROM_OK;
}

void tinromLogWrite(const char *format, ...) { (void) format; }

/* --- checks ------------------------------------------------------------- */

static int failures;
static void check(const char *what, int ok) {
	printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

int main(int argc, char **argv) {
	if (argc < 3) return 2;
	toHost = atoi(argv[1]);
	fromHost = atoi(argv[2]);

	int32_t handle = 0;
	uint32_t got = 0, length = 0;
	static char buffer[4096];

	check("init", tinromPcdrvCall(TINROM_PCDRV_INIT, 0, 0, 0).v0 == 0);
	check("host seen", tinromPcdrvHostPresent());

	check("open a file",
		tinromPcdrvOpen("HELLO.TXT", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_OK);

	check("length via seek-to-end",
		tinromPcdrvLength(handle, &length) == TINROM_OK);
	printf("      length = %u\n", length);
	check("length is right", length == 13);

	check("read", tinromPcdrvRead(handle, buffer, length, &got) == TINROM_OK);
	buffer[length] = 0;
	printf("      read back '%s'\n", buffer);
	check("contents match", !strcmp(buffer, "hello, host!\n"));

	check("seek back to start",
		tinromPcdrvSeek(handle, 0, TINROM_PCDRV_SEEK_SET, &got) == TINROM_OK
			&& got == 0);
	check("close", tinromPcdrvClose(handle) == TINROM_OK);

	check("DOS path with a drive letter",
		tinromPcdrvOpen("C:\\HELLO.TXT", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_OK);
	check("close again", tinromPcdrvClose(handle) == TINROM_OK);

	check("subdirectory",
		tinromPcdrvOpen("\\SUB\\DEEP.TXT", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_OK);
	check("close third", tinromPcdrvClose(handle) == TINROM_OK);

	check("missing file is refused, not a hang",
		tinromPcdrvOpen("NOPE.TXT", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_ERR_NOT_PRESENT);

	check("escaping the root is refused",
		tinromPcdrvOpen("..\\..\\etc\\passwd", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_ERR_NOT_PRESENT);

	check("create and write back",
		tinromPcdrvCreate("OUT.BIN", 0, &handle) == TINROM_OK);
	memset(buffer, 0x5a, 1024);
	check("write 1 KB",
		tinromPcdrvWrite(handle, buffer, 1024, &got) == TINROM_OK
			&& got == 1024);
	check("close written file", tinromPcdrvClose(handle) == TINROM_OK);

	check("reopen what we wrote",
		tinromPcdrvOpen("OUT.BIN", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_OK);
	memset(buffer, 0, sizeof(buffer));
	check("read it back",
		tinromPcdrvRead(handle, buffer, 1024, &got) == TINROM_OK);
	int same = 1;
	for (int i = 0; i < 1024; i++) if ((unsigned char) buffer[i] != 0x5a) same = 0;
	check("1 KB round trip intact", same);
	check("final close", tinromPcdrvClose(handle) == TINROM_OK);

	/* psx-spx: PCRead pads past the end of file rather than stopping. */
	check("reopen short file",
		tinromPcdrvOpen("HELLO.TXT", TINROM_PCDRV_O_RDONLY, &handle)
			== TINROM_OK);
	memset(buffer, 0xff, 64);
	check("over-read succeeds",
		tinromPcdrvRead(handle, buffer, 32, &got) == TINROM_OK && got == 32);
	check("padded with zeroes past EOF",
		(unsigned char) buffer[13] == 0 && (unsigned char) buffer[31] == 0);
	check("close", tinromPcdrvClose(handle) == TINROM_OK);

	printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
	return failures ? 1 : 0;
}
