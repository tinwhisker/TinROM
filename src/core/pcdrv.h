/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * PCDRV: files on the developer's PC, as seen from the console.
 *
 * Seven calls, documented by psx-spx under "BIOS PC File Server" as the SN
 * Systems break-instruction interface that Caetla and the DTL-H2000 devkits
 * exposed. A program on the console issues `break 101h`..`break 107h` with
 * arguments in A1, A2 and A3 - not A0, unlike every kernel call - and gets
 * two results back, in V0 and V1:
 *
 *   101h  PCInit()
 *   102h  PCCreat(filename, attributes)
 *   103h  PCOpen(filename, accessmode)
 *   104h  PCClose(handle)
 *   105h  PCRead(handle, length, destination)
 *   106h  PCWrite(handle, length, source)
 *   107h  PClSeek(handle, offset, mode)
 *
 *   V0  0 on success, -1 on failure
 *   V1  handle, byte count or file position - or an error code when V0 is -1
 *
 * On a devkit the break was caught by a debug stub and forwarded over the
 * DECI link to the host card. tinrom forwards it down the serial cable
 * instead; see docs/pcdrv.md for the framing.
 *
 * The same entry point serves both users: tinrom's own modules calling out to
 * the host, and - once the exception handler exists - a running program's
 * breaks being dispatched here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/result.h"

#define TINROM_PCDRV_INIT  0x101
#define TINROM_PCDRV_CREAT 0x102
#define TINROM_PCDRV_OPEN  0x103
#define TINROM_PCDRV_CLOSE 0x104
#define TINROM_PCDRV_READ  0x105
#define TINROM_PCDRV_WRITE 0x106
#define TINROM_PCDRV_SEEK  0x107

/* Access modes, as the SN interface defines them. */
#define TINROM_PCDRV_O_RDONLY 0x00
#define TINROM_PCDRV_O_WRONLY 0x01
#define TINROM_PCDRV_O_RDWR   0x02

/* Seek modes. */
#define TINROM_PCDRV_SEEK_SET 0
#define TINROM_PCDRV_SEEK_CUR 1
#define TINROM_PCDRV_SEEK_END 2

/** Longest path tinrom will ask the host for. */
#define TINROM_PCDRV_PATH_MAX 128

typedef struct {
	int32_t v0; /* 0 or -1, exactly as the break interface returns it */
	int32_t v1; /* handle, count, position, or an error code */
} TinromPcdrvResult;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The one entry point. `a1`, `a2` and `a3` are the break interface's
 * arguments in its own order, so a dispatcher can pass a trapped frame
 * straight through.
 *
 * A host that is not listening looks like a failure, not a hang: every
 * exchange is bounded by a timeout.
 */
TinromPcdrvResult tinromPcdrvCall(
	uint32_t function, uint32_t a1, uint32_t a2, uint32_t a3
);

/** True if a host answered the last exchange. Cleared by a timeout. */
bool tinromPcdrvHostPresent(void);

/** Convenience wrappers, for tinrom's own modules. */
TinromResult tinromPcdrvOpen(const char *path, uint32_t mode, int32_t *handle);
TinromResult tinromPcdrvCreate(const char *path, uint32_t attributes,
	int32_t *handle);
TinromResult tinromPcdrvClose(int32_t handle);
/** Reads up to `length` bytes; `read` gets how many arrived. */
TinromResult tinromPcdrvRead(int32_t handle, void *buffer, uint32_t length,
	uint32_t *read);
TinromResult tinromPcdrvWrite(int32_t handle, const void *buffer,
	uint32_t length, uint32_t *written);
TinromResult tinromPcdrvSeek(int32_t handle, int32_t offset, uint32_t mode,
	uint32_t *position);
/** Length of an open file, leaving the position back at the start. */
TinromResult tinromPcdrvLength(int32_t handle, uint32_t *length);

#ifdef __cplusplus
}
#endif
