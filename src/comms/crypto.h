/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * The little cryptography the IRC client's password lock needs, written from
 * the published specifications (see NOTICE.md):
 *
 *   SHA-256        FIPS 180-4
 *   HMAC-SHA-256   RFC 2104 (test vectors from RFC 4231)
 *   PBKDF2         RFC 8018, with HMAC-SHA-256 as the PRF
 *   ChaCha20       RFC 8439
 *
 * Pure C, no hardware; tools/test_irc checks each against its vectors.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CRYPTO_SHA256_SIZE  32
#define CRYPTO_CHACHA_KEY   32
#define CRYPTO_CHACHA_NONCE 12

typedef struct {
	uint32_t state[8];
	uint64_t length;      /* bytes hashed so far */
	uint8_t  block[64];
	unsigned used;        /* bytes waiting in `block` */
} CryptoSha256;

typedef struct {
	CryptoSha256 inner, outer;
} CryptoHmac;

#ifdef __cplusplus
extern "C" {
#endif

void cryptoSha256Init(CryptoSha256 *sha);
void cryptoSha256Update(CryptoSha256 *sha, const void *data, size_t length);
void cryptoSha256Final(CryptoSha256 *sha, uint8_t digest[CRYPTO_SHA256_SIZE]);
void cryptoSha256(const void *data, size_t length,
	uint8_t digest[CRYPTO_SHA256_SIZE]);

void cryptoHmacInit(CryptoHmac *hmac, const void *key, size_t keyLength);
void cryptoHmacUpdate(CryptoHmac *hmac, const void *data, size_t length);
void cryptoHmacFinal(CryptoHmac *hmac, uint8_t mac[CRYPTO_SHA256_SIZE]);
void cryptoHmac(const void *key, size_t keyLength, const void *data,
	size_t length, uint8_t mac[CRYPTO_SHA256_SIZE]);

/**
 * PBKDF2-HMAC-SHA-256. `progress`, if set, is called every 256 iterations
 * with how many are done, so a slow derivation can keep a screen alive.
 */
void cryptoPbkdf2(
	const void *password, size_t passwordLength,
	const void *salt, size_t saltLength,
	uint32_t iterations, uint8_t *out, size_t outLength,
	void (*progress)(uint32_t done, uint32_t total)
);

/** XORs `length` bytes of ChaCha20 keystream into `data`, from `counter`. */
void cryptoChaCha20(
	const uint8_t key[CRYPTO_CHACHA_KEY],
	const uint8_t nonce[CRYPTO_CHACHA_NONCE],
	uint32_t counter, uint8_t *data, size_t length
);

/** The 64-byte block function on its own, for the test vectors. */
void cryptoChaCha20Block(
	const uint8_t key[CRYPTO_CHACHA_KEY],
	const uint8_t nonce[CRYPTO_CHACHA_NONCE],
	uint32_t counter, uint8_t out[64]
);

/** memset that the compiler may not drop: for keys and passwords. */
void cryptoWipe(void *data, size_t length);

#ifdef __cplusplus
}
#endif
