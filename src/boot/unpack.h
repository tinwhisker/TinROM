/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * The cart payload's decoder. tools/tinlz.py describes the format and is
 * the encoder; src/boot/unpack.c is this.
 */
#pragma once

#include <stdint.h>

/* tools/tinlz.py's FORMAT_VERSION: the two change together. */
#define TINROM_UNPACK_FORMAT 1

/**
 * Decodes a tinlz stream at `src` into exactly `size` bytes at `dst`.
 *
 * In the cart it runs straight out of ROM, called by src/boot/rom_stub.S
 * before tinrom is in RAM, so it is built on its own into code with no
 * relocations, no data and no calls - one function, which makerom.py places
 * and tools/checkblob.py vets. Nothing is checked: the stream is the one
 * makerom.py wrote and verified, and there is nowhere to report a fault.
 */
void tinromUnpack(const uint8_t *src, uint8_t *dst, uint32_t size);
