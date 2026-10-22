/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PCDRV over the serial link. The framing is documented in docs/pcdrv.md;
 * the calls themselves are psx-spx's, see pcdrv.h.
 *
 * Every other command on this link is host-initiated: the host says SBIN and
 * tinrom answers. PCDRV is the other way round - the console asks and the
 * host answers - so the host has to already be listening. That is the whole
 * reason for the request token: it lets a host that is sitting in its monitor
 * loop tell a PCDRV request from line noise.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/log.h"
#include "core/pcdrv.h"
#include "hal/uart_sio.h"
#include "libc/string.h"

/* Four-character tokens, little endian on the wire, like the rest of the link. */
#define PCDRV_REQUEST  0x52444350 /* "PCDR" */
#define PCDRV_REPLY_OK 0x4b4f4350 /* "PCOK" */
#define PCDRV_REPLY_ER 0x52454350 /* "PCER" */

/*
 * How long to wait for the host. A read that has to come off a spinning disk
 * on the other end can take a moment; a host that is not there should not
 * cost the user more than a beat.
 */
#define PCDRV_TIMEOUT_FRAMES 60

#define PCDRV_MAX_PAYLOAD 65536u

static bool hostPresent;

static void writeWord(uint32_t value) {
	tinromSioWrite(&value, sizeof(value));
}

static bool readWord(uint32_t *value) {
	return tinromSioRead(value, sizeof(*value), PCDRV_TIMEOUT_FRAMES)
		== TINROM_OK;
}

/* Plain 32-bit additive sum, the same one the block transfers use. */
static uint32_t checksum(const uint8_t *data, uint32_t length) {
	uint32_t sum = 0;

	for (uint32_t i = 0; i < length; i++)
		sum += data[i];

	return sum;
}

static TinromPcdrvResult failure(void) {
	TinromPcdrvResult result = { -1, -1 };

	return result;
}

/*
 * One exchange. Sending is unconditional - there is no handshake to lose - so
 * the only failure modes are a host that never answers and a payload that
 * does not survive the cable.
 */
static TinromPcdrvResult exchange(
	uint32_t       function,
	uint32_t       a1,
	uint32_t       a2,
	uint32_t       a3,
	const void    *outgoing,
	uint32_t       outgoingLength,
	void          *incoming,
	uint32_t       incomingCapacity
) {
	tinromSioFlushInput();

	writeWord(PCDRV_REQUEST);
	writeWord(function);
	writeWord(a1);
	writeWord(a2);
	writeWord(a3);
	writeWord(outgoingLength);

	if (outgoingLength) {
		tinromSioWrite(outgoing, outgoingLength);
		writeWord(checksum((const uint8_t *) outgoing, outgoingLength));
	}

	uint32_t token;

	if (!readWord(&token)) {
		hostPresent = false;

		TINROM_LOG("pcdrv: no host");

		return failure();
	}

	hostPresent = true;

	if ((token != PCDRV_REPLY_OK) && (token != PCDRV_REPLY_ER)) {
		TINROM_LOG("pcdrv: bad reply %08x", token);

		return failure();
	}

	uint32_t v0 = 0, v1 = 0, length = 0;

	if (!readWord(&v0) || !readWord(&v1) || !readWord(&length))
		return failure();

	if (length) {
		if (length > incomingCapacity) {
			/*
			 * Do not half-read it: the link would be left mid-payload and
			 * every later call would see garbage.
			 */
			TINROM_LOG("pcdrv: host sent %u bytes, room for %u",
				length, incomingCapacity);
			tinromSioFlushInput();

			return failure();
		}

		if (tinromSioRead(incoming, length, PCDRV_TIMEOUT_FRAMES) != TINROM_OK)
			return failure();

		uint32_t expected;

		if (!readWord(&expected))
			return failure();

		if (expected != checksum((const uint8_t *) incoming, length)) {
			TINROM_LOG("pcdrv: payload checksum");

			return failure();
		}
	}

	TinromPcdrvResult result = { (int32_t) v0, (int32_t) v1 };

	return result;
}

bool tinromPcdrvHostPresent(void) {
	return hostPresent;
}

/*
 * The operations, taking real pointers. Everything tinrom calls itself comes
 * through here; tinromPcdrvCall() below is the narrow door for a trapped
 * break, where the arguments really are 32-bit console addresses.
 */
static TinromPcdrvResult operation(
	uint32_t    function,
	uint32_t    a1,
	uint32_t    a2,
	uint32_t    a3,
	const void *outgoing,
	uint32_t    outgoingLength,
	void       *incoming,
	uint32_t    incomingCapacity
) {
	return exchange(
		function, a1, a2, a3,
		outgoing, outgoingLength, incoming, incomingCapacity
	);
}

static uint32_t pathLength(const char *path) {
	uint32_t length = 0;

	while (path[length] && (length < (TINROM_PCDRV_PATH_MAX - 1)))
		length++;

	return length;
}

TinromPcdrvResult tinromPcdrvCall(
	uint32_t function, uint32_t a1, uint32_t a2, uint32_t a3
) {
	switch (function) {
		case TINROM_PCDRV_INIT:
		case TINROM_PCDRV_CLOSE:
		case TINROM_PCDRV_SEEK:
			return operation(function, a1, a2, a3, NULL, 0, NULL, 0);

		case TINROM_PCDRV_CREAT:
		case TINROM_PCDRV_OPEN: {
			/*
			 * A1 is a pointer to the filename in the console's memory. The
			 * host cannot follow it, so the string travels with the request.
			 */
			const char *path = (const char *) (uintptr_t) a1;

			return operation(
				function, a1, a2, a3, path, pathLength(path) + 1, NULL, 0
			);
		}

		case TINROM_PCDRV_READ:
			/* A2 is the length, A3 the destination in console memory. */
			if (a2 > PCDRV_MAX_PAYLOAD)
				return failure();

			return operation(
				function, a1, a2, a3, NULL, 0, (void *) (uintptr_t) a3, a2
			);

		case TINROM_PCDRV_WRITE:
			if (a2 > PCDRV_MAX_PAYLOAD)
				return failure();

			return operation(
				function, a1, a2, a3,
				(const void *) (uintptr_t) a3, a2, NULL, 0
			);

		default:
			return failure();
	}
}

/* --- wrappers ----------------------------------------------------------- */

/*
 * Two different failures wear the same -1 on the wire, and telling them apart
 * matters: "there is no host listening" is a cable problem, "the host said
 * no" is a file problem.
 */
static TinromResult toResult(TinromPcdrvResult result, uint32_t *out) {
	if (!tinromPcdrvHostPresent())
		return TINROM_ERR_TIMEOUT;

	if (result.v0 != 0)
		return TINROM_ERR_IO;

	if (out)
		*out = (uint32_t) result.v1;

	return TINROM_OK;
}

TinromResult tinromPcdrvOpen(const char *path, uint32_t mode, int32_t *handle) {
	if (!path || !handle)
		return TINROM_ERR_BAD_ARG;

	TinromPcdrvResult result = operation(
		TINROM_PCDRV_OPEN, 0, mode, 0, path, pathLength(path) + 1, NULL, 0
	);

	if (!tinromPcdrvHostPresent())
		return TINROM_ERR_TIMEOUT;

	/* The host answered and refused: the file is not there, or not readable. */
	if (result.v0 != 0)
		return TINROM_ERR_NOT_PRESENT;

	*handle = result.v1;

	return TINROM_OK;
}

TinromResult tinromPcdrvCreate(
	const char *path, uint32_t attributes, int32_t *handle
) {
	if (!path || !handle)
		return TINROM_ERR_BAD_ARG;

	TinromPcdrvResult result = operation(
		TINROM_PCDRV_CREAT, 0, attributes, 0, path, pathLength(path) + 1,
		NULL, 0
	);

	if (!tinromPcdrvHostPresent())
		return TINROM_ERR_TIMEOUT;

	if (result.v0 != 0)
		return TINROM_ERR_IO;

	*handle = result.v1;

	return TINROM_OK;
}

TinromResult tinromPcdrvClose(int32_t handle) {
	return toResult(
		operation(TINROM_PCDRV_CLOSE, (uint32_t) handle, 0, 0,
			NULL, 0, NULL, 0),
		NULL
	);
}

TinromResult tinromPcdrvRead(
	int32_t handle, void *buffer, uint32_t length, uint32_t *read
) {
	if (!buffer)
		return TINROM_ERR_BAD_ARG;

	if (length > PCDRV_MAX_PAYLOAD)
		return TINROM_ERR_BAD_ARG;

	return toResult(
		operation(TINROM_PCDRV_READ, (uint32_t) handle, length, 0,
			NULL, 0, buffer, length),
		read
	);
}

TinromResult tinromPcdrvWrite(
	int32_t handle, const void *buffer, uint32_t length, uint32_t *written
) {
	if (!buffer)
		return TINROM_ERR_BAD_ARG;

	if (length > PCDRV_MAX_PAYLOAD)
		return TINROM_ERR_BAD_ARG;

	return toResult(
		operation(TINROM_PCDRV_WRITE, (uint32_t) handle, length, 0,
			buffer, length, NULL, 0),
		written
	);
}

TinromResult tinromPcdrvSeek(
	int32_t handle, int32_t offset, uint32_t mode, uint32_t *position
) {
	return toResult(
		operation(TINROM_PCDRV_SEEK, (uint32_t) handle, (uint32_t) offset,
			mode, NULL, 0, NULL, 0),
		position
	);
}

TinromResult tinromPcdrvLength(int32_t handle, uint32_t *length) {
	/*
	 * psx-spx spells this out, including the trap: PCRead does not stop at
	 * the end of the file, it pads with zeroes. So the only way to know how
	 * much is really there is to seek to the end - and then put the position
	 * back, which is the part that is easy to forget.
	 */
	TinromResult result = tinromPcdrvSeek(
		handle, 0, TINROM_PCDRV_SEEK_END, length
	);

	if (result != TINROM_OK)
		return result;

	return tinromPcdrvSeek(handle, 0, TINROM_PCDRV_SEEK_SET, NULL);
}
