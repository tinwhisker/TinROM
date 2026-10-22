/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The password lock: IRC bookmarks' passwords sealed with a master password.
 *
 * Off, passwords are stored as typed. On, each is encrypted with ChaCha20
 * under a key derived from the master password by PBKDF2-HMAC-SHA-256 with a
 * random salt, so the card holds nothing a save editor can read off. The
 * master password itself is never stored - only a check value derived from
 * the key, which tells a right one from a wrong one.
 *
 * The key is kept in RAM once unlocked, until commsVaultLock() or power-off.
 *
 * What it is not: a memory card can be copied, and a copy can be guessed at
 * on a PC without limit. A few thousand PBKDF2 iterations is what a 33 MHz
 * console can afford in a second, which is little to a PC; so a short master
 * password only keeps out the casual. A long one keeps out much more.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "comms/config.h"
#include "core/result.h"

#define COMMS_VAULT_ITERATIONS 4096

typedef void (*CommsVaultProgress)(uint32_t done, uint32_t total);

#ifdef __cplusplus
extern "C" {
#endif

bool commsVaultEnabled(const CommsConfig *config);
/** Whether the key in RAM is the one `config`'s lock was made with. */
bool commsVaultUnlocked(const CommsConfig *config);

/** TINROM_ERR_BAD_ARG for a wrong password; the key stays unknown. */
TinromResult commsVaultUnlock(CommsConfig *config, const char *password,
	CommsVaultProgress progress);
/** Forgets the key. Sealed passwords stay sealed. */
void         commsVaultLock(void);

/** Turns the lock on with `password`, sealing every stored password. */
TinromResult commsVaultEnable(CommsConfig *config, const char *password,
	CommsVaultProgress progress);
/** Unlocked only: a new master password, every password sealed again. */
TinromResult commsVaultChange(CommsConfig *config, const char *password,
	CommsVaultProgress progress);
/** Unlocked only: the lock off, every password stored as typed again. */
TinromResult commsVaultDisable(CommsConfig *config);
/** For a forgotten master password: the lock off, sealed passwords gone. */
void         commsVaultForget(CommsConfig *config);

/** Whether a password is stored at all - known even while locked. */
bool commsSecretPresent(const CommsSecret *secret);
/**
 * Stores `text` ("" for none), sealed if the lock is on. False if the lock
 * is on but not unlocked, or `text` is too long; the secret is unchanged.
 */
bool commsSecretSet(CommsConfig *config, CommsSecret *secret,
	const char *text);
/** The password as typed; false (and "") if it is sealed and locked. */
bool commsSecretGet(const CommsConfig *config, const CommsSecret *secret,
	char *out, size_t size);

/** Folds something unpredictable into the pool salts and nonces come from. */
void commsEntropyStir(uint32_t value);
void commsEntropy(uint8_t *out, size_t length);

#ifdef __cplusplus
}
#endif
