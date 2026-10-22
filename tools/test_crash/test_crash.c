/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The cart's crash screen (src/crash/crash.c), drawn on the host.
 *
 * Underneath it, everything src/crash/hw.h promises: a GPU that runs the
 * handful of commands the screen sends and refuses any other, and a 2 MB RAM
 * with a crashed program's registers and memory laid out in it the way the
 * kernel leaves them. Each case then reads the screen back, cell by cell,
 * against the same font, and checks what it says - the exception, the
 * registers, the code and stack words - and that nothing was read from
 * anywhere but RAM and the BIOS, which on a console could fault again.
 *
 *     test_crash [<directory>]
 *
 * With a directory it writes each case's screen there as a PNG, at twice
 * the console's resolution.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "crash/crash_api.h"
#include "crash/hw.h"

extern const uint8_t crashFont[95][8];

static unsigned int failures;

static void fail(const char *format, ...) {
	va_list args;

	va_start(args, format);
	printf("  FAIL: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	failures++;
}

/* --- RAM ------------------------------------------------------------------ */

static uint32_t ram[0x200000 / 4];
static bool     quieted;

static void poke(uint32_t address, uint32_t value) {
	ram[(address & 0x1fffff) / 4] = value;
}

uint32_t hwPeek(uint32_t address) {
	/* crash.c's readable() hands over uncached RAM and BIOS addresses only. */
	if ((address >= 0xa0000000u) && (address < 0xa0200000u) && !(address & 3))
		return ram[(address & 0x1fffff) / 4];
	if ((address >= 0xbfc00000u) && (address < 0xbfc80000u) && !(address & 3))
		return 0xb105b105u;

	fail("read %08x, which on a console might fault", (unsigned) address);
	return 0;
}

void hwQuiet(void) {
	quieted = true;
}

/* On a console crashMain never returns; here it does, to the next case. */
void hwHalt(void) {}

/* --- the GPU ---------------------------------------------------------------- */

static uint16_t vram[512][1024];
static uint32_t statBeforeReset;
static bool     reset, displayOn, palMode, wide320;
static int      pageX, pageY;
static uint32_t command[8];
static unsigned int commandLength, commandWanted;

/* A VRAM upload in progress: where the next pair of texels goes. */
static unsigned int uploadX, uploadY, uploadW, uploadH, uploadLeft, uploadAt;

uint32_t hwGpuStat(void) {
	/* Always ready, and PAL or not as the program left it. */
	return (reset ? 0 : statBeforeReset) | (1u << 26) | (1u << 28);
}

void hwGp1(uint32_t word) {
	switch (word >> 24) {
		case 0x00: reset = true; displayOn = false; break;
		case 0x03: displayOn = !(word & 1); break;
		case 0x05: case 0x06: case 0x07: break;

		case 0x08:
			palMode = (word >> 3) & 1;
			wide320 = ((word & 0x43) == 1);
			break;

		default:
			fail("GP1(%02xh) is not one the screen should send",
				(unsigned) (word >> 24));
	}
}

static uint16_t modulate(uint16_t texel, uint32_t ink) {
	uint16_t out = 0;

	for (unsigned int c = 0; c < 3; c++) {
		unsigned int value = ((texel >> (c * 5)) & 31)
			* ((ink >> (c * 8)) & 0xff) / 0x80;

		out |= (uint16_t) (((value > 31) ? 31 : value) << (c * 5));
	}

	return out;
}

static void execute(void) {
	uint32_t     *cmd = command;
	unsigned int  op  = cmd[0] >> 24;

	switch (op) {
		case 0x00: case 0x01: break;

		case 0x02: {
			uint32_t rgb = cmd[0];
			uint16_t c   = (uint16_t) (((rgb & 0xff) >> 3)
				| (((rgb >> 8) & 0xff) >> 3) << 5
				| (((rgb >> 16) & 0xff) >> 3) << 10);

			for (unsigned int y = 0; y < (cmd[2] >> 16); y++)
				for (unsigned int x = 0; x < (cmd[2] & 0xffff); x++)
					vram[(cmd[1] >> 16) + y][(cmd[1] & 0xffff) + x] = c;
			break;
		}

		case 0x74: {
			int x = (int) (int16_t) (cmd[1] & 0xffff);
			int y = (int) (int16_t) (cmd[1] >> 16);
			int u = (int) (cmd[2] & 0xff), v = (int) ((cmd[2] >> 8) & 0xff);

			if ((x < 0) || (y < 0) || ((x + 8) > 320) || ((y + 8) > 240))
				fail("a glyph at %d,%d is off the screen", x, y);

			for (int j = 0; j < 8; j++) {
				for (int i = 0; i < 8; i++) {
					uint16_t t = vram[pageY + v + j][pageX + u + i];

					if (t && ((x + i) < 320) && ((y + j) < 240))
						vram[y + j][x + i] = modulate(t, cmd[0]);
				}
			}
			break;
		}

		case 0xa0:
			uploadX    = cmd[1] & 0xffff;
			uploadY    = cmd[1] >> 16;
			uploadW    = cmd[2] & 0xffff;
			uploadH    = cmd[2] >> 16;
			uploadLeft = (uploadW * uploadH + 1) / 2;
			uploadAt   = 0;
			break;

		case 0xe1:
			pageX = (int) (cmd[0] & 15) * 64;
			pageY = (int) ((cmd[0] >> 4) & 1) * 256;

			if (((cmd[0] >> 7) & 3) != 2)
				fail("the font page is not 16-bit");
			break;

		case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: break;

		default:
			fail("GP0(%02xh) is not one the screen should send", op);
	}
}

static unsigned int lengthOf(uint32_t first) {
	switch (first >> 24) {
		case 0x02: return 3;
		case 0x74: return 3;
		case 0xa0: return 3;
		default:   return 1;
	}
}

static void gp0(uint32_t word) {
	if (uploadLeft) {
		for (unsigned int half = 0; half < 2; half++, uploadAt++) {
			if (uploadAt >= uploadW * uploadH)
				break;

			vram[uploadY + uploadAt / uploadW][uploadX + uploadAt % uploadW] =
				(uint16_t) (word >> (half * 16));
		}

		uploadLeft--;
		return;
	}

	if (!commandLength)
		commandWanted = lengthOf(word);

	command[commandLength++] = word;

	if (commandLength == commandWanted) {
		execute();
		commandLength = 0;
	}
}

void hwGp0(uint32_t word) {
	gp0(word);
}

void hwGp0Command(uint32_t word) {
	if (commandLength || uploadLeft)
		fail("a command started in the middle of another");

	gp0(word);
}

/* --- reading the screen back ------------------------------------------------ */

/* The character in a cell, '?' if it is no glyph, ' ' if it is empty. */
static char cellAt(unsigned int column, unsigned int row) {
	uint16_t paper = vram[0][0];
	bool     lit[8][8];
	bool     any = false;

	for (unsigned int y = 0; y < 8; y++) {
		for (unsigned int x = 0; x < 8; x++) {
			lit[y][x] = vram[row * 8 + y][column * 8 + x] != paper;
			any |= lit[y][x];
		}
	}

	if (!any)
		return ' ';

	for (unsigned int g = 1; g < 95; g++) {
		bool same = true;

		for (unsigned int y = 0; (y < 8) && same; y++)
			for (unsigned int x = 0; (x < 8) && same; x++)
				same = lit[y][x] == !!(crashFont[g][y] & (8 >> (x / 2)));

		if (same)
			return (char) (' ' + g);
	}

	return '?';
}

static void rowText(unsigned int row, char *out) {
	for (unsigned int column = 0; column < 40; column++)
		out[column] = cellAt(column, row);

	out[40] = 0;

	for (int i = 39; (i >= 0) && (out[i] == ' '); i--)
		out[i] = 0;
}

static void expect(unsigned int row, const char *wanted) {
	char line[41];

	rowText(row, line);

	if (!strstr(line, wanted))
		fail("row %u reads \"%s\", not containing \"%s\"", row, line, wanted);
}

static void show(void) {
	char line[41];

	for (unsigned int row = 0; row < 30; row++) {
		rowText(row, line);

		if (line[0])
			printf("    %2u |%s\n", row, line);
	}
}

/* --- PNG -------------------------------------------------------------------- */

static void chunk(FILE *file, const char *type, const uint8_t *data,
	uint32_t length) {
	uint8_t head[8] = {
		length >> 24, length >> 16, length >> 8, length,
		type[0], type[1], type[2], type[3]
	};
	uLong crc = crc32(0, head + 4, 4);

	crc = crc32(crc, data, length);
	fwrite(head, 1, 8, file);
	fwrite(data, 1, length, file);

	uint8_t tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };

	fwrite(tail, 1, 4, file);
}

/* 320x240 doubled both ways: 640x480, square pixels, the shape a TV shows. */
static void savePng(const char *directory, const char *name) {
	char path[512];

	snprintf(path, sizeof(path), "%s/%s.png", directory, name);

	FILE *file = fopen(path, "wb");

	if (!file) {
		perror(path);
		exit(1);
	}

	int      ow = 640, oh = 480;
	size_t   rawSize = (size_t) oh * (1 + ow * 3);
	uint8_t *raw     = malloc(rawSize), *p = raw;

	for (int y = 0; y < oh; y++) {
		*(p++) = 0;

		for (int x = 0; x < ow; x++) {
			uint16_t c = vram[y / 2][x / 2];

			*(p++) = (uint8_t) ((c & 31) << 3);
			*(p++) = (uint8_t) (((c >> 5) & 31) << 3);
			*(p++) = (uint8_t) (((c >> 10) & 31) << 3);
		}
	}

	uLongf   packedSize = compressBound(rawSize);
	uint8_t *packed     = malloc(packedSize);

	compress(packed, &packedSize, raw, rawSize);

	static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
	uint8_t header[13] = {
		ow >> 24, ow >> 16, ow >> 8, ow, oh >> 24, oh >> 16, oh >> 8, oh,
		8, 2, 0, 0, 0
	};

	fwrite(signature, 1, 8, file);
	chunk(file, "IHDR", header, 13);
	chunk(file, "IDAT", packed, (uint32_t) packedSize);
	chunk(file, "IEND", NULL, 0);
	fclose(file);
	free(raw);
	free(packed);

	printf("  wrote %s\n", path);
}

/* --- the cases -------------------------------------------------------------- */

#define PCB 0x80000e00u
#define TCB 0x80000e80u
#define SP  0x801ffe00u

static void freshConsole(bool pal) {
	memset(ram, 0, sizeof(ram));
	memset(vram, 0x5a, sizeof(vram));
	statBeforeReset = pal ? (1u << 20) : 0;
	reset = displayOn = palMode = wide320 = quieted = false;
	pageX = pageY = 0;
	commandLength = uploadLeft = 0;
}

/* A program the kernel has saved: r<n> = n repeated, in the current TCB. */
static void savedProgram(void) {
	poke(0x80000108u, PCB);
	poke(PCB, TCB);
	poke(0x80000110u, TCB);
	poke(TCB, 0x4000);

	for (unsigned int n = 1; n < 32; n++)
		poke(TCB + 8 + n * 4, 0x01010101u * n);

	poke(TCB + 8 + 29 * 4, SP);
	poke(TCB + 0x8c, 0x0000aaaau); /* hi */
	poke(TCB + 0x90, 0x0000bbbbu); /* lo */

	for (unsigned int i = 0; i < 8; i++)
		poke(SP + i * 4, 0x5a000000u + i);
}

static void run(const char *name, const char *directory, uint32_t cause,
	uint32_t epc, uint32_t badVaddr) {
	printf("%s\n", name);

	crashMain(cause, epc, badVaddr, 0x40000404u);

	if (!quieted)
		fail("the GPU's DMA and the SPU were not stopped");
	if (!reset || !displayOn)
		fail("the GPU was not reset and the picture turned on");
	if (!wide320)
		fail("the picture is not 320 wide");

	show();

	if (directory)
		savePng(directory, name);
}

#define CODE 0x80012340u

int main(int argc, char **argv) {
	const char *directory = (argc > 1) ? argv[1] : NULL;

	/* An unaligned load, as tinrom's own crash test makes. */
	freshConsole(false);
	savedProgram();
	poke(CODE + 0, 0x3c010badu);
	poke(CODE + 4, 0x8c22c0deu);
	poke(CODE + 8, 0x00000000u);
	run("crash-address", directory, 4 << 2, CODE + 4, 0x0badc0deu);

	expect(1, "The game crashed.");
	expect(3, "Address error on a load");
	expect(4, "pc 80012344");
	expect(4, "badv 0badc0de");
	expect(5, "cause 00000010");
	expect(8, "at 01010101");
	expect(8, "v1 03030303");
	expect(16, "t9 19191919");
	expect(16, "gp 1c1c1c1c");
	expect(17, "fp 1e1e1e1e");
	expect(17, "hi 0000aaaa");
	expect(18, "lo 0000bbbb");
	expect(21, "00000000 3c010bad 8c22c0de 00000000");
	expect(23, "stack at 801ffe00");
	expect(24, "5a000000 5a000001 5a000002 5a000003");
	expect(25, "5a000004 5a000005 5a000006 5a000007");
	expect(27, "Reset the console");

	if (palMode)
		fail("an NTSC game's crash screen is PAL");

	/* GCC's division check, in a branch delay slot, on a PAL console. */
	freshConsole(true);
	savedProgram();
	poke(CODE + 0, 0x10000003u); /* b */
	poke(CODE + 4, 0x0007000du); /* break 7 */
	run("crash-divide", directory, (9 << 2) | (1u << 31), CODE, 0);

	expect(3, "Division by zero");
	expect(6, "The fault is in pc's delay slot.");

	if (!palMode)
		fail("a PAL game's crash screen is NTSC");

	/* Jumping off into nowhere, with the kernel's pointers trampled. */
	freshConsole(false);
	poke(0x80000108u, 0x12345678u);
	poke(0x80000110u, 0xdeadbeefu);
	run("crash-nowhere", directory, 6 << 2, 0x1f000400u, 0x1f000400u);

	expect(3, "Bus error fetching an instruction");
	expect(8, "The kernel's copy of the registers");
	expect(9, "could not be found.");
	expect(21, "-------- -------- -------- --------");

	/* Some other break, through a TCB the PCB does not lead to. */
	freshConsole(false);
	savedProgram();
	poke(0x80000108u, 0x00000002u);
	poke(CODE, 0x0123000du); /* break 0x123 */
	run("crash-break", directory, 9 << 2, CODE, 0);

	expect(3, "Break instruction, code 123");
	expect(8, "at 01010101");

	if (failures) {
		printf("%u failure(s)\n", failures);
		return 1;
	}

	printf("all passed (0 failures)\n");
	return 0;
}
