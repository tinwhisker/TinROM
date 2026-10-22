/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PS-EXE header, as documented by psx-spx ("CDROM File Formats" - PSX
 * executables). Offsets are into the 2048-byte header sector:
 *
 *   +000h  8   "PS-X EXE"
 *   +010h  4   initial PC
 *   +014h  4   initial GP
 *   +018h  4   destination address in RAM
 *   +01Ch  4   filesize, excluding the header
 *   +020h  4   memfill start address
 *   +024h  4   memfill size
 *   +030h  4   initial SP base
 *   +034h  4   initial SP offset
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/psexe.h"
#include "libc/string.h"

#define EXE_PC      0x10
#define EXE_GP      0x14
#define EXE_ADDRESS 0x18
#define EXE_SIZE    0x1c
#define EXE_FILL    0x20
#define EXE_FILLLEN 0x24
#define EXE_SP_BASE 0x30
#define EXE_SP_OFF  0x34

/* Main RAM, and the same window through the uncached and cached segments. */
#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x00200000u

/* --- SYSTEM.CNF --------------------------------------------------------- */

static bool isSpace(char c) {
	return (c == ' ') || (c == '\t') || (c == '\r');
}

static char upper(char c) {
	return ((c >= 'a') && (c <= 'z')) ? (char) (c - 'a' + 'A') : c;
}

bool tinromParseBootLine(
	const char *text, size_t length, char *out, size_t size
) {
	if (!text || !out || (size < 2))
		return false;

	static const char key[] = "BOOT";
	const size_t      keyLength = sizeof(key) - 1;

	size_t i = 0;

	while (i < length) {
		/* Start of a line. */
		size_t lineEnd = i;

		while ((lineEnd < length) && (text[lineEnd] != '\n'))
			lineEnd++;

		size_t cursor = i;

		while ((cursor < lineEnd) && isSpace(text[cursor]))
			cursor++;

		bool matches = ((lineEnd - cursor) >= keyLength);

		for (size_t k = 0; matches && (k < keyLength); k++) {
			if (upper(text[cursor + k]) != key[k])
				matches = false;
		}

		if (matches) {
			cursor += keyLength;

			while ((cursor < lineEnd) && isSpace(text[cursor]))
				cursor++;

			/*
			 * BOOT2 exists on PS2 discs and is not ours; requiring the "="
			 * right after the key and its spaces rejects it.
			 */
			if ((cursor < lineEnd) && (text[cursor] == '=')) {
				cursor++;

				while ((cursor < lineEnd) && isSpace(text[cursor]))
					cursor++;

				size_t end = lineEnd;

				while ((end > cursor) && isSpace(text[end - 1]))
					end--;

				size_t valueLength = end - cursor;

				if (!valueLength || ((valueLength + 1) > size))
					return false;

				memcpy(out, &text[cursor], valueLength);
				out[valueLength] = '\0';

				return true;
			}
		}

		i = lineEnd + 1;
	}

	return false;
}

/* --- the executable header ---------------------------------------------- */

static uint32_t read32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Accepts KUSEG, KSEG0 and KSEG1 views of the same two megabytes. */
static bool inMainRam(uint32_t address, uint32_t length) {
	uint32_t offset = address & 0x1fffffffu;

	if (offset >= RAM_SIZE)
		return false;

	return (RAM_SIZE - offset) >= length;
}

TinromResult tinromExeParse(const void *headerSector, TinromExeHeader *out) {
	if (!headerSector || !out)
		return TINROM_ERR_BAD_ARG;

	const uint8_t *header = (const uint8_t *) headerSector;

	if (memcmp(header, TINROM_EXE_MAGIC, 8))
		return TINROM_ERR_CORRUPT;

	out->pc          = read32(&header[EXE_PC]);
	out->gp          = read32(&header[EXE_GP]);
	out->loadAddress = read32(&header[EXE_ADDRESS]);
	out->loadSize    = read32(&header[EXE_SIZE]);
	out->fillStart   = read32(&header[EXE_FILL]);
	out->fillSize    = read32(&header[EXE_FILLLEN]);
	out->spBase      = read32(&header[EXE_SP_BASE]);
	out->spOffset    = read32(&header[EXE_SP_OFF]);

	if (!out->loadSize)
		return TINROM_ERR_CORRUPT;

	if (!inMainRam(out->loadAddress, out->loadSize))
		return TINROM_ERR_UNSUPPORTED;

	if (!inMainRam(out->pc, 4))
		return TINROM_ERR_UNSUPPORTED;

	if (out->fillSize && !inMainRam(out->fillStart, out->fillSize))
		return TINROM_ERR_UNSUPPORTED;

	return TINROM_OK;
}

#ifdef __mips__

void tinromExeRun(const TinromExeHeader *header) {
	uint32_t pc = header->pc;
	uint32_t gp = header->gp;
	uint32_t sp = header->spBase + header->spOffset;

	/*
	 * A zero stack base means "keep whatever is already there", which is what
	 * the kernel's own loader does. tinrom's stack is as good as any.
	 */
	if (!header->spBase) {
		__asm__ volatile("move %0, $sp\n" : "=r"(sp));
	}

	/*
	 * No going back past this point. Interrupts off first, so nothing fires
	 * between loading the new stack pointer and arriving at the entry point.
	 */
	__asm__ volatile(
		".set push\n"
		".set noreorder\n"
		"mtc0 $zero, $12\n"   /* COP0 Status: all interrupts disabled */
		"nop\n"
		"move $gp, %1\n"
		"move $sp, %2\n"
		"move $fp, %2\n"
		"move $a0, $zero\n"
		"move $a1, $zero\n"
		"move $a2, $zero\n"
		"move $a3, $zero\n"
		"jr   %0\n"
		"nop\n"
		".set pop\n"
		:
		: "r"(pc), "r"(gp), "r"(sp)
		: "memory"
	);

	__builtin_unreachable();
}

#else

/*
 * Host builds compile the parser for testing; there is nothing to hand over
 * to. See scripts/syntax-check.sh and tools/test_cdboot.c.
 */
void tinromExeRun(const TinromExeHeader *header) {
	(void) header;
}

#endif
