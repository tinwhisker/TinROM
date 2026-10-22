/* tinrom - (C) 2026 TinWhisker - MIT licensed */
#pragma once

/*
 * Every fallible tinrom call returns a TinromResult. Stubs return
 * TINROM_ERR_UNIMPLEMENTED, which the UI renders as "not implemented yet"
 * rather than pretending the operation succeeded. Grep for it to find what is
 * left to do.
 */
typedef enum {
	TINROM_OK                = 0,
	TINROM_ERR_UNIMPLEMENTED = -1,
	TINROM_ERR_NOT_PRESENT   = -2,
	TINROM_ERR_IO            = -3,
	TINROM_ERR_TIMEOUT       = -4,
	TINROM_ERR_BAD_ARG       = -5,
	TINROM_ERR_NO_SPACE      = -6,
	TINROM_ERR_VERIFY        = -7,
	TINROM_ERR_ABORTED       = -8,
	TINROM_ERR_UNSUPPORTED   = -9,
	TINROM_ERR_BUSY          = -10,
	TINROM_ERR_CORRUPT       = -11
} TinromResult;

#ifdef __cplusplus
extern "C" {
#endif

const char *tinromResultString(TinromResult result);

#ifdef __cplusplus
}
#endif
