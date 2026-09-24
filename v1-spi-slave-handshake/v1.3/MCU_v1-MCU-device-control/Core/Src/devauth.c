/*
 * SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104) in plain C, small and
 * portable, for a Cortex-M3 without crypto hardware. See devauth.h.
 */
#include "devauth.h"

#include <string.h>

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

struct sha256_ctx {
	uint32_t h[8];
	uint8_t block[64];
	size_t used;     /* bytes in block */
	uint64_t total;  /* bytes hashed */
};

static void sha256_compress(uint32_t h[8], const uint8_t b[64])
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
		       ((uint32_t)b[4 * i + 2] << 8) | (uint32_t)b[4 * i + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
		uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
		hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
	}
	h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_init(struct sha256_ctx *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(c->h, iv, sizeof(iv));
	c->used = 0;
	c->total = 0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *p, size_t n)
{
	c->total += n;
	while (n) {
		size_t take = 64 - c->used;
		if (take > n)
			take = n;
		memcpy(c->block + c->used, p, take);
		c->used += take;
		p += take;
		n -= take;
		if (c->used == 64) {
			sha256_compress(c->h, c->block);
			c->used = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits = c->total * 8;
	uint8_t pad = 0x80;
	sha256_update(c, &pad, 1);
	pad = 0;
	while (c->used != 56)
		sha256_update(c, &pad, 1);
	uint8_t len[8];
	for (int i = 0; i < 8; i++)
		len[i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(c, len, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
	struct sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, msg, len);
	sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
	uint8_t k[64] = { 0 };
	uint8_t pad[64];
	uint8_t inner[32];
	struct sha256_ctx c;

	if (key_len > 64)
		sha256(key, key_len, k);
	else
		memcpy(k, key, key_len);

	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&c);
	sha256_update(&c, pad, 64);
	sha256_update(&c, msg, msg_len);
	sha256_final(&c, inner);

	for (int i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha256_init(&c);
	sha256_update(&c, pad, 64);
	sha256_update(&c, inner, 32);
	sha256_final(&c, out);

	/* Don't leave key-derived material on the stack. */
	memset(k, 0, sizeof(k));
	memset(pad, 0, sizeof(pad));
}

int devauth_key_present(const uint8_t key[DEVAUTH_KEY_LEN])
{
	for (unsigned i = 0; i < DEVAUTH_KEY_LEN; i++)
		if (key[i] != 0xFF)
			return 1;
	return 0;
}

void devauth_compute(const uint8_t key[DEVAUTH_KEY_LEN],
                     uint32_t device_id, uint32_t fw_version,
                     const uint8_t nonce[DEVAUTH_NONCE_LEN],
                     uint8_t mac[DEVAUTH_MAC_LEN])
{
	uint8_t msg[sizeof(DEVAUTH_DOMAIN) - 1 + 8 + DEVAUTH_NONCE_LEN];
	uint8_t full[32];
	size_t n = sizeof(DEVAUTH_DOMAIN) - 1;

	memcpy(msg, DEVAUTH_DOMAIN, n);
	for (int i = 0; i < 4; i++)
		msg[n + i] = (uint8_t)(device_id >> (24 - 8 * i));
	for (int i = 0; i < 4; i++)
		msg[n + 4 + i] = (uint8_t)(fw_version >> (24 - 8 * i));
	memcpy(msg + n + 8, nonce, DEVAUTH_NONCE_LEN);

	hmac_sha256(key, DEVAUTH_KEY_LEN, msg, sizeof(msg), full);
	memcpy(mac, full, DEVAUTH_MAC_LEN);
	memset(full, 0, sizeof(full));
}
