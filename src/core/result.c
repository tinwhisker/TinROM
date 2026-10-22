/* tinrom - (C) 2026 TinWhisker - MIT licensed */

#include "core/result.h"

const char *tinromResultString(TinromResult result) {
	switch (result) {
		case TINROM_OK:                return "ok";
		case TINROM_ERR_UNIMPLEMENTED: return "not implemented yet";
		case TINROM_ERR_NOT_PRESENT:   return "not present";
		case TINROM_ERR_IO:            return "I/O error";
		case TINROM_ERR_TIMEOUT:       return "timed out";
		case TINROM_ERR_BAD_ARG:       return "bad argument";
		case TINROM_ERR_NO_SPACE:      return "out of space";
		case TINROM_ERR_VERIFY:        return "verify failed";
		case TINROM_ERR_ABORTED:       return "aborted";
		case TINROM_ERR_UNSUPPORTED:   return "unsupported here";
		case TINROM_ERR_BUSY:          return "busy";
		case TINROM_ERR_CORRUPT:       return "corrupt data";
		default:                       return "unknown error";
	}
}
