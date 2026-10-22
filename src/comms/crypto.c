/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * SHA-256, HMAC, PBKDF2 and ChaCha20. See crypto.h for where each comes
 * from. Written for clarity before speed: the slowest use, deriving the
 * password lock's key, is a second or so on the console and happens once.
 */

#include <stddef.h>
#include <stdint.h>
#include "comms/crypto.h"
#include "libc/string.h"

static uint32_t rotr(uint32_t x, unsigned n) {
	return (x >> n) | (x << (32 - n));
}

static uint32_t rotl(uint32_t x, unsigned n) {
	return (x << n) | (x >> (32 - n));
}

static uint32_t load32be(const uint8_t *p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
		| ((uint32_t) p[2] << 8) | p[3];
}

static uint32_t load32le(const uint8_t *p) {
	return ((uint32_t) p[3] << 24) | ((uint32_t) p[2] << 16)
		| ((uint32_t) p[1] << 8) | p[0];
}

static void store32be(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t) (v >> 24);
	p[1] = (uint8_t) (v >> 16);
	p[2] = (uint8_t) (v >> 8);
	p[3] = (uint8_t) v;
}

static void store32le(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

void cryptoWipe(void *data, size_t length) {
	volatile uint8_t *p = (volatile uint8_t *) data;

	while (length--)
		*p++ = 0;
}

/* --- SHA-256, FIPS 180-4 section 6.2 -------------------------------------- */

/* The first 32 bits of the fractional parts of the cube roots of the first
 * 64 primes (FIPS 180-4, 4.2.2). */
static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256Block(uint32_t state[8], const uint8_t block[64]) {
	uint32_t w[64];

	for (unsigned t = 0; t < 16; t++)
		w[t] = load32be(&block[t * 4]);

	for (unsigned t = 16; t < 64; t++) {
		uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18)
			^ (w[t - 15] >> 3);
		uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19)
			^ (w[t - 2] >> 10);

		w[t] = w[t - 16] + s0 + w[t - 7] + s1;
	}

	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

	for (unsigned t = 0; t < 64; t++) {
		uint32_t S1  = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch  = (e & f) ^ (~e & g);
		uint32_t t1  = h + S1 + ch + K[t] + w[t];
		uint32_t S0  = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2  = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void cryptoSha256Init(CryptoSha256 *sha) {
	/* FIPS 180-4, 5.3.3: square roots of the first eight primes. */
	static const uint32_t initial[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	memcpy(sha->state, initial, sizeof(initial));
	sha->length = 0;
	sha->used   = 0;
}

void cryptoSha256Update(CryptoSha256 *sha, const void *data, size_t length) {
	const uint8_t *bytes = (const uint8_t *) data;

	sha->length += length;

	while (length) {
		size_t take = 64 - sha->used;

		if (take > length)
			take = length;

		memcpy(&sha->block[sha->used], bytes, take);
		sha->used += (unsigned) take;
		bytes     += take;
		length    -= take;

		if (sha->used == 64) {
			sha256Block(sha->state, sha->block);
			sha->used = 0;
		}
	}
}

void cryptoSha256Final(CryptoSha256 *sha, uint8_t digest[CRYPTO_SHA256_SIZE]) {
	uint64_t bits = sha->length * 8;

	/* 5.1.1: a 1 bit, zeros to 56 mod 64, then the length in bits. */
	sha->block[sha->used++] = 0x80;

	if (sha->used > 56) {
		memset(&sha->block[sha->used], 0, 64 - sha->used);
		sha256Block(sha->state, sha->block);
		sha->used = 0;
	}

	memset(&sha->block[sha->used], 0, 56 - sha->used);
	store32be(&sha->block[56], (uint32_t) (bits >> 32));
	store32be(&sha->block[60], (uint32_t) bits);
	sha256Block(sha->state, sha->block);

	for (unsigned i = 0; i < 8; i++)
		store32be(&digest[i * 4], sha->state[i]);

	cryptoWipe(sha, sizeof(*sha));
}

void cryptoSha256(const void *data, size_t length,
	uint8_t digest[CRYPTO_SHA256_SIZE]) {
	CryptoSha256 sha;

	cryptoSha256Init(&sha);
	cryptoSha256Update(&sha, data, length);
	cryptoSha256Final(&sha, digest);
}

/* --- HMAC, RFC 2104 ----------------------------------------------------- */

void cryptoHmacInit(CryptoHmac *hmac, const void *key, size_t keyLength) {
	uint8_t block[64];

	memset(block, 0, sizeof(block));

	/* Keys longer than the block are hashed first. */
	if (keyLength > sizeof(block))
		cryptoSha256(key, keyLength, block);
	else
		memcpy(block, key, keyLength);

	for (unsigned i = 0; i < 64; i++)
		block[i] ^= 0x36;

	cryptoSha256Init(&hmac->inner);
	cryptoSha256Update(&hmac->inner, block, 64);

	/* 36h ^ 5Ch, so one more XOR turns ipad into opad. */
	for (unsigned i = 0; i < 64; i++)
		block[i] ^= 0x36 ^ 0x5c;

	cryptoSha256Init(&hmac->outer);
	cryptoSha256Update(&hmac->outer, block, 64);
	cryptoWipe(block, sizeof(block));
}

void cryptoHmacUpdate(CryptoHmac *hmac, const void *data, size_t length) {
	cryptoSha256Update(&hmac->inner, data, length);
}

void cryptoHmacFinal(CryptoHmac *hmac, uint8_t mac[CRYPTO_SHA256_SIZE]) {
	uint8_t inner[CRYPTO_SHA256_SIZE];

	cryptoSha256Final(&hmac->inner, inner);
	cryptoSha256Update(&hmac->outer, inner, sizeof(inner));
	cryptoSha256Final(&hmac->outer, mac);
	cryptoWipe(inner, sizeof(inner));
}

void cryptoHmac(const void *key, size_t keyLength, const void *data,
	size_t length, uint8_t mac[CRYPTO_SHA256_SIZE]) {
	CryptoHmac hmac;

	cryptoHmacInit(&hmac, key, keyLength);
	cryptoHmacUpdate(&hmac, data, length);
	cryptoHmacFinal(&hmac, mac);
}

/* --- PBKDF2, RFC 8018 section 5.2 ------------------------------------------ */

void cryptoPbkdf2(
	const void *password, size_t passwordLength,
	const void *salt, size_t saltLength,
	uint32_t iterations, uint8_t *out, size_t outLength,
	void (*progress)(uint32_t done, uint32_t total)
) {
	/*
	 * The keyed starting states are computed once and copied for every
	 * iteration, which halves the work against calling cryptoHmac() each
	 * time.
	 */
	CryptoHmac keyed;

	cryptoHmacInit(&keyed, password, passwordLength);

	uint32_t blocks = (uint32_t) ((outLength + CRYPTO_SHA256_SIZE - 1)
		/ CRYPTO_SHA256_SIZE);
	uint32_t total  = blocks * iterations;
	uint32_t done   = 0;

	for (uint32_t i = 1; i <= blocks; i++) {
		uint8_t    u[CRYPTO_SHA256_SIZE], t[CRYPTO_SHA256_SIZE], index[4];
		CryptoHmac hmac = keyed;

		store32be(index, i);
		cryptoHmacUpdate(&hmac, salt, saltLength);
		cryptoHmacUpdate(&hmac, index, sizeof(index));
		cryptoHmacFinal(&hmac, u);
		memcpy(t, u, sizeof(t));

		for (uint32_t j = 1; j < iterations; j++) {
			hmac = keyed;
			cryptoHmacUpdate(&hmac, u, sizeof(u));
			cryptoHmacFinal(&hmac, u);

			for (unsigned k = 0; k < sizeof(t); k++)
				t[k] ^= u[k];

			if (progress && !(++done & 255))
				progress(done, total);
		}

		size_t offset = (size_t) (i - 1) * CRYPTO_SHA256_SIZE;
		size_t take   = outLength - offset;

		if (take > sizeof(t))
			take = sizeof(t);

		memcpy(out + offset, t, take);
		cryptoWipe(u, sizeof(u));
		cryptoWipe(t, sizeof(t));
	}

	cryptoWipe(&keyed, sizeof(keyed));
}

/* --- ChaCha20, RFC 8439 section 2 ----------------------------------------- */

#define QUARTER(a, b, c, d) do { \
	a += b; d ^= a; d = rotl(d, 16); \
	c += d; b ^= c; b = rotl(b, 12); \
	a += b; d ^= a; d = rotl(d, 8);  \
	c += d; b ^= c; b = rotl(b, 7);  \
} while (0)

void cryptoChaCha20Block(
	const uint8_t key[CRYPTO_CHACHA_KEY],
	const uint8_t nonce[CRYPTO_CHACHA_NONCE],
	uint32_t counter, uint8_t out[64]
) {
	uint32_t in[16], x[16];

	/* "expand 32-byte k" */
	in[0] = 0x61707865; in[1] = 0x3320646e;
	in[2] = 0x79622d32; in[3] = 0x6b206574;

	for (unsigned i = 0; i < 8; i++)
		in[4 + i] = load32le(&key[i * 4]);

	in[12] = counter;

	for (unsigned i = 0; i < 3; i++)
		in[13 + i] = load32le(&nonce[i * 4]);

	memcpy(x, in, sizeof(x));

	/* Twenty rounds: ten of a column round then a diagonal one. */
	for (unsigned i = 0; i < 10; i++) {
		QUARTER(x[0], x[4], x[8],  x[12]);
		QUARTER(x[1], x[5], x[9],  x[13]);
		QUARTER(x[2], x[6], x[10], x[14]);
		QUARTER(x[3], x[7], x[11], x[15]);
		QUARTER(x[0], x[5], x[10], x[15]);
		QUARTER(x[1], x[6], x[11], x[12]);
		QUARTER(x[2], x[7], x[8],  x[13]);
		QUARTER(x[3], x[4], x[9],  x[14]);
	}

	for (unsigned i = 0; i < 16; i++)
		store32le(&out[i * 4], x[i] + in[i]);

	cryptoWipe(in, sizeof(in));
	cryptoWipe(x, sizeof(x));
}

void cryptoChaCha20(
	const uint8_t key[CRYPTO_CHACHA_KEY],
	const uint8_t nonce[CRYPTO_CHACHA_NONCE],
	uint32_t counter, uint8_t *data, size_t length
) {
	uint8_t stream[64];

	while (length) {
		size_t take = (length < sizeof(stream)) ? length : sizeof(stream);

		cryptoChaCha20Block(key, nonce, counter++, stream);

		for (size_t i = 0; i < take; i++)
			data[i] ^= stream[i];

		data   += take;
		length -= take;
	}

	cryptoWipe(stream, sizeof(stream));
}
