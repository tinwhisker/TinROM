/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * The cart payload's decoder: tools/tinlz.py describes the stream and
 * writes it. The cart's stub calls this, running straight out of ROM, to
 * unpack tinrom into RAM; tools/test_unpack runs the same code on the host.
 *
 * It has to be one function with nothing outside itself: no data (there is
 * nowhere writable for it), no constants in .rodata, no calls - not even to
 * memcpy, which a compiler will happily turn a copy loop into - and so no
 * relocations, because it is never linked. Hence the helpers below are
 * forced inline, and cmake/rom.cmake builds it with flags that stop the
 * compiler inventing calls or tables. tools/checkblob.py refuses the build
 * if anything slipped through.
 */
#include <stdint.h>
#include "boot/unpack.h"

#define INLINE static inline __attribute__((always_inline))

typedef struct {
	const uint8_t *src;
	uint32_t       flags; /* the byte bits are being taken from */
	uint32_t       left;  /* how many of its bits are still to take */
} Stream;

/* The next bit, most significant first, refilling from the stream. */
INLINE uint32_t takeBit(Stream *in) {
	if (!in->left) {
		in->flags = *in->src++;
		in->left  = 8;
	}

	in->left--;

	return (in->flags >> in->left) & 1;
}

/* An Elias gamma code: a 0 for each bit after the top one, then the value. */
INLINE uint32_t takeGamma(Stream *in) {
	uint32_t extra = 0;

	while (!takeBit(in))
		extra++;

	uint32_t value = 1;

	while (extra--)
		value = (value << 1) | takeBit(in);

	return value;
}

void tinromUnpack(const uint8_t *src, uint8_t *dst, uint32_t size) {
	uint8_t       *end    = dst + size;
	uint32_t       offset = 0;
	uint32_t       length;
	const uint8_t *from;
	Stream         in     = { src, 0, 0 };

	for (;;) {
		/* A run of literal bytes - always the first item. */
		length = takeGamma(&in);

		do {
			*dst++ = *in.src++;
		} while (--length);

		if (dst >= end)
			return;

		/* After literals: a copy at the last offset, or at a new one. */
		if (takeBit(&in)) {
			length = takeGamma(&in);
		} else {
newCopy:
			offset  = (takeGamma(&in) - 1) << 8;
			offset += *in.src++;
			offset += 1;
			length  = takeGamma(&in) + 1;
		}

		/* Byte by byte: a copy may overlap what it is writing. */
		from = dst - offset;

		do {
			*dst++ = *from++;
		} while (--length);

		if (dst >= end)
			return;

		/* After a copy: another at a new offset, or literals. */
		if (takeBit(&in))
			goto newCopy;
	}
}
