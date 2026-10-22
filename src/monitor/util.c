/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Hex, crash-report text, and the two libc functions the compiler is allowed
 * to call behind our backs. The monitor cannot use tinrom's copies: they may
 * have been overwritten by the program being debugged.
 */

#include "monitor/mon.h"

/* --- hex ------------------------------------------------------------------ */

char monHexDigit(uint32_t value) {
	value &= 15;

	return (char) ((value < 10) ? ('0' + value) : ('a' + value - 10));
}

int monHexValue(char c) {
	if ((c >= '0') && (c <= '9'))
		return c - '0';
	if ((c >= 'a') && (c <= 'f'))
		return c - 'a' + 10;
	if ((c >= 'A') && (c <= 'F'))
		return c - 'A' + 10;

	return -1;
}

uint32_t monParseHex(const char **text) {
	uint32_t value = 0;
	int      digit;

	while ((digit = monHexValue(**text)) >= 0) {
		value = (value << 4) | (uint32_t) digit;
		(*text)++;
	}

	return value;
}

/* --- reports ---------------------------------------------------------------- */

void monReport(const char *text) {
	monSioPutString(text);
}

void monReportHex(uint32_t value) {
	for (int shift = 28; shift >= 0; shift -= 4)
		monSioPut((uint8_t) monHexDigit(value >> shift));
}

/* --- what the compiler may emit calls to ------------------------------------ */

void *memcpy(void *dest, const void *src, size_t n) {
	uint8_t       *d = (uint8_t *) dest;
	const uint8_t *s = (const uint8_t *) src;

	while (n--)
		*(d++) = *(s++);

	return dest;
}

void *memset(void *dest, int c, size_t n) {
	uint8_t *d = (uint8_t *) dest;

	while (n--)
		*(d++) = (uint8_t) c;

	return dest;
}

int monStrStarts(const char *text, const char *prefix) {
	while (*prefix) {
		if (*(text++) != *(prefix++))
			return 0;
	}

	return 1;
}
