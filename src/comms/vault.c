/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The password lock. See vault.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "comms/crypto.h"
#include "comms/vault.h"
#include "hal/timer.h"
#include "libc/string.h"

#define CHECK_LABEL "tinrom password lock"

/* The unlocked key, and which lock it opens. */
static uint8_t key[CRYPTO_CHACHA_KEY];
static uint8_t keySalt[16];
static uint8_t keyCheck[16];
static bool    haveKey;

/* --- entropy ----------------------------------------------------------------- */

static uint8_t  pool[CRYPTO_SHA256_SIZE];
static uint32_t poolDraws;

void commsEntropyStir(uint32_t value) {
	/* Cheap enough for every frame: hashing waits for a draw. */
	unsigned at = (unsigned) (value ^ (value >> 13)) % (sizeof(pool) - 4);

	for (unsigned i = 0; i < 4; i++)
		pool[at + i] ^= (uint8_t) (value >> (i * 8));

	pool[sizeof(pool) - 1] += 1;
}

void commsEntropy(uint8_t *out, size_t length) {
	while (length) {
		CryptoSha256 sha;
		uint8_t      digest[CRYPTO_SHA256_SIZE];
		uint32_t     noise[4] = {
			tinromTimerNoise(), tinromFrameCounter(), ++poolDraws, 0
		};

		/* The timers move while the hash runs, so a second read differs. */
		cryptoSha256Init(&sha);
		cryptoSha256Update(&sha, pool, sizeof(pool));
		cryptoSha256Update(&sha, noise, sizeof(noise));
		noise[3] = tinromTimerNoise();
		cryptoSha256Update(&sha, &noise[3], sizeof(noise[3]));
		cryptoSha256Final(&sha, digest);

		/* Half goes out, half becomes the pool: no draw reveals the next. */
		memcpy(pool, digest, sizeof(pool));
		cryptoSha256(digest, sizeof(digest), digest);

		size_t take = (length < sizeof(digest)) ? length : sizeof(digest);

		memcpy(out, digest, take);
		out    += take;
		length -= take;
		cryptoWipe(digest, sizeof(digest));
	}
}

/* --- keys --------------------------------------------------------------------- */

static void derive(const CommsVault *vault, const char *password,
	uint8_t out[CRYPTO_CHACHA_KEY], uint8_t check[16],
	CommsVaultProgress progress) {
	uint8_t mac[CRYPTO_SHA256_SIZE];

	cryptoPbkdf2(password, strlen(password), vault->salt, sizeof(vault->salt),
		vault->iterations, out, CRYPTO_CHACHA_KEY, progress);
	cryptoHmac(out, CRYPTO_CHACHA_KEY, CHECK_LABEL, sizeof(CHECK_LABEL) - 1,
		mac);
	memcpy(check, mac, 16);
	cryptoWipe(mac, sizeof(mac));
}

/* No early exit, so the comparison's time says nothing about the bytes. */
static bool same(const uint8_t *a, const uint8_t *b, size_t length) {
	uint8_t diff = 0;

	for (size_t i = 0; i < length; i++)
		diff |= a[i] ^ b[i];

	return !diff;
}

bool commsVaultEnabled(const CommsConfig *config) {
	return config->vault.enabled != 0;
}

bool commsVaultUnlocked(const CommsConfig *config) {
	return haveKey && commsVaultEnabled(config)
		&& same(keySalt, config->vault.salt, sizeof(keySalt))
		&& same(keyCheck, config->vault.check, sizeof(keyCheck));
}

TinromResult commsVaultUnlock(CommsConfig *config, const char *password,
	CommsVaultProgress progress) {
	if (!commsVaultEnabled(config))
		return TINROM_ERR_BAD_ARG;
	if (commsVaultUnlocked(config))
		return TINROM_OK;

	uint8_t candidate[CRYPTO_CHACHA_KEY], check[16];

	derive(&config->vault, password, candidate, check, progress);

	if (!same(check, config->vault.check, sizeof(check))) {
		cryptoWipe(candidate, sizeof(candidate));

		return TINROM_ERR_BAD_ARG;
	}

	memcpy(key, candidate, sizeof(key));
	memcpy(keySalt, config->vault.salt, sizeof(keySalt));
	memcpy(keyCheck, check, sizeof(keyCheck));
	haveKey = true;
	cryptoWipe(candidate, sizeof(candidate));

	/*
	 * The counter is only as new as the copy that was loaded: an older one,
	 * or the other card's, would count over values used already. A random
	 * jump each time the lock is opened makes meeting them again unlikely,
	 * on top of the random half of every nonce.
	 */
	uint8_t jump[2];

	commsEntropy(jump, sizeof(jump));
	config->vault.counter += 1u + ((uint32_t) jump[0] << 8) + jump[1];

	return TINROM_OK;
}

void commsVaultLock(void) {
	cryptoWipe(key, sizeof(key));
	haveKey = false;
}

/* --- sealing ------------------------------------------------------------------ */

static void seal(CommsVault *vault, CommsSecret *secret) {
	uint32_t counter = ++vault->counter;

	/* A counter that never repeats under one salt, and noise besides. */
	secret->nonce[0] = (uint8_t) counter;
	secret->nonce[1] = (uint8_t) (counter >> 8);
	secret->nonce[2] = (uint8_t) (counter >> 16);
	secret->nonce[3] = (uint8_t) (counter >> 24);
	commsEntropy(&secret->nonce[4], sizeof(secret->nonce) - 4);

	cryptoChaCha20(key, secret->nonce, 1, secret->data, sizeof(secret->data));
	secret->state = COMMS_SECRET_SEALED;
}

static void unseal(CommsSecret *secret) {
	cryptoChaCha20(key, secret->nonce, 1, secret->data, sizeof(secret->data));
	memset(secret->nonce, 0, sizeof(secret->nonce));
	secret->state = COMMS_SECRET_PLAIN;
}

/* Every password in every IRC bookmark, two to a bookmark. */
static CommsSecret *secretAt(CommsConfig *config, unsigned index) {
	CommsIrcProfile *irc = &config->bookmarks[index / 2].irc;

	return (index & 1) ? &irc->nickServPassword : &irc->serverPassword;
}

#define SECRET_COUNT (COMMS_BOOKMARKS * 2)

TinromResult commsVaultEnable(CommsConfig *config, const char *password,
	CommsVaultProgress progress) {
	if (commsVaultEnabled(config) || !password[0])
		return TINROM_ERR_BAD_ARG;

	CommsVault *vault = &config->vault;
	uint8_t     newKey[CRYPTO_CHACHA_KEY], check[16];

	commsEntropy(vault->salt, sizeof(vault->salt));
	vault->iterations = COMMS_VAULT_ITERATIONS;
	vault->counter    = 0;
	derive(vault, password, newKey, check, progress);

	memcpy(key, newKey, sizeof(key));
	memcpy(keySalt, vault->salt, sizeof(keySalt));
	memcpy(keyCheck, check, sizeof(keyCheck));
	memcpy(vault->check, check, sizeof(vault->check));
	haveKey        = true;
	vault->enabled = 1;
	cryptoWipe(newKey, sizeof(newKey));

	for (unsigned i = 0; i < SECRET_COUNT; i++) {
		CommsSecret *secret = secretAt(config, i);

		if (secret->state == COMMS_SECRET_PLAIN)
			seal(vault, secret);
	}

	return TINROM_OK;
}

TinromResult commsVaultDisable(CommsConfig *config) {
	if (!commsVaultUnlocked(config))
		return TINROM_ERR_BAD_ARG;

	for (unsigned i = 0; i < SECRET_COUNT; i++) {
		CommsSecret *secret = secretAt(config, i);

		if (secret->state == COMMS_SECRET_SEALED)
			unseal(secret);
	}

	memset(&config->vault, 0, sizeof(config->vault));
	commsVaultLock();

	return TINROM_OK;
}

TinromResult commsVaultChange(CommsConfig *config, const char *password,
	CommsVaultProgress progress) {
	if (!password[0])
		return TINROM_ERR_BAD_ARG;

	TinromResult result = commsVaultDisable(config);

	if (result != TINROM_OK)
		return result;

	return commsVaultEnable(config, password, progress);
}

void commsVaultForget(CommsConfig *config) {
	for (unsigned i = 0; i < SECRET_COUNT; i++) {
		CommsSecret *secret = secretAt(config, i);

		if (secret->state == COMMS_SECRET_SEALED)
			memset(secret, 0, sizeof(*secret));
	}

	memset(&config->vault, 0, sizeof(config->vault));
	commsVaultLock();
}

/* --- one password --------------------------------------------------------------- */

bool commsSecretPresent(const CommsSecret *secret) {
	return secret->state != COMMS_SECRET_NONE;
}

bool commsSecretSet(CommsConfig *config, CommsSecret *secret,
	const char *text) {
	size_t length = strlen(text);

	if (length >= COMMS_SECRET_MAX)
		return false;
	if (commsVaultEnabled(config) && !commsVaultUnlocked(config))
		return false;

	cryptoWipe(secret, sizeof(*secret));

	if (!length)
		return true;

	secret->state   = COMMS_SECRET_PLAIN;
	secret->data[0] = (uint8_t) length;
	memcpy(&secret->data[1], text, length);

	if (commsVaultEnabled(config))
		seal(&config->vault, secret);

	return true;
}

bool commsSecretGet(const CommsConfig *config, const CommsSecret *secret,
	char *out, size_t size) {
	CommsSecret copy = *secret;
	bool        ok   = true;

	if (!size)
		return false;

	out[0] = 0;

	if (copy.state == COMMS_SECRET_NONE) {
		/* Nothing to give. */
	} else if (copy.state == COMMS_SECRET_SEALED) {
		if (!commsVaultUnlocked(config))
			ok = false;
		else
			unseal(&copy);
	}

	if (ok && (copy.state == COMMS_SECRET_PLAIN)) {
		size_t length = copy.data[0];

		if (length >= COMMS_SECRET_MAX)
			length = COMMS_SECRET_MAX - 1;
		if (length >= size)
			length = size - 1;

		memcpy(out, &copy.data[1], length);
		out[length] = 0;
	}

	cryptoWipe(&copy, sizeof(copy));

	return ok;
}
