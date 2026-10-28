/* tinrom - (C) 2026 The tinrom contributors - MIT licensed
 *
 * Host-side test of the cart's payload decoder (src/boot/unpack.c): the
 * same code the cart runs, against streams tools/tinlz.py wrote. Run by
 * scripts/test-unpack.sh, which makes the cases with
 * tools/test_unpack/cases.py: for each NAME.lz given, NAME.bin is what it
 * must unpack to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot/unpack.h"

#define GUARD 64

static unsigned char *load(const char *path, size_t *size) {
	FILE *file = fopen(path, "rb");

	if (!file)
		return NULL;

	fseek(file, 0, SEEK_END);
	*size = (size_t) ftell(file);
	fseek(file, 0, SEEK_SET);

	/* Exactly its size, so a sanitizer sees a read past the stream. */
	unsigned char *data = malloc(*size ? *size : 1);

	if (data && fread(data, 1, *size, file) != *size) {
		free(data);
		data = NULL;
	}

	fclose(file);

	return data;
}

int main(int argc, char **argv) {
	int fails = 0;

	for (int i = 1; i < argc; i++) {
		char   expectedPath[4096];
		size_t streamSize, size;

		snprintf(expectedPath, sizeof(expectedPath), "%.*s.bin",
			(int) (strlen(argv[i]) - 3), argv[i]);

		unsigned char *stream   = load(argv[i], &streamSize);
		unsigned char *expected = load(expectedPath, &size);

		if (!stream || !expected) {
			printf("  FAIL %s: cannot read it or %s\n", argv[i], expectedPath);
			fails++;
			continue;
		}

		/* Guard bytes either side catch a write out of bounds. */
		unsigned char *buffer = malloc(size + 2 * GUARD);

		memset(buffer, 0xa5, size + 2 * GUARD);
		tinromUnpack(stream, buffer + GUARD, (uint32_t) size);

		int bad = memcmp(buffer + GUARD, expected, size) != 0;

		for (size_t j = 0; j < GUARD; j++) {
			if ((buffer[j] != 0xa5) || (buffer[GUARD + size + j] != 0xa5))
				bad = 1;
		}

		const char *name = strrchr(argv[i], '/');

		printf("  %-4s %s: %zu -> %zu bytes\n", bad ? "FAIL" : "ok",
			name ? name + 1 : argv[i], streamSize, size);
		fails += bad;

		free(buffer);
		free(stream);
		free(expected);
	}

	if (argc < 2) {
		printf("  FAIL: no cases\n");
		fails++;
	}

	printf("%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails,
		(fails == 1) ? "" : "s");

	return fails ? 1 : 0;
}
