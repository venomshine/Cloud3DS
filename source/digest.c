/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "digest.h"

#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* ---------------------------------------------------------------- MD5 (RFC 1321) */
static const uint32_t MD5_K[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t MD5_S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(Md5 *c, const uint8_t *p)
{
	uint32_t m[16];
	for (int i = 0; i < 16; i++)
		m[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
	uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
	for (int i = 0; i < 64; i++) {
		uint32_t f;
		int g;
		if (i < 16)
			f = (b & cc) | (~b & d), g = i;
		else if (i < 32)
			f = (d & b) | (~d & cc), g = (5 * i + 1) & 15;
		else if (i < 48)
			f = b ^ cc ^ d, g = (3 * i + 5) & 15;
		else
			f = cc ^ (b | ~d), g = (7 * i) & 15;
		uint32_t t = d;
		d = cc;
		cc = b;
		b = b + ROL(a + f + MD5_K[i] + m[g], MD5_S[i]);
		a = t;
	}
	c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d;
}

void md5_init(Md5 *c)
{
	c->h[0] = 0x67452301, c->h[1] = 0xefcdab89, c->h[2] = 0x98badcfe, c->h[3] = 0x10325476;
	c->len = 0;
}

void md5_update(Md5 *c, const void *data, size_t n)
{
	const uint8_t *p = data;
	size_t have = c->len & 63;
	c->len += n;
	if (have) {
		size_t k = 64 - have < n ? 64 - have : n;
		memcpy(c->buf + have, p, k);
		p += k, n -= k;
		if (have + k < 64)
			return;
		md5_block(c, c->buf);
	}
	for (; n >= 64; p += 64, n -= 64)
		md5_block(c, p);
	memcpy(c->buf, p, n);
}

void md5_final(Md5 *c, uint8_t out[16])
{
	uint64_t bits = c->len * 8;
	uint8_t pad[72] = {0x80};
	size_t have = c->len & 63, pn = have < 56 ? 56 - have : 120 - have;
	for (int i = 0; i < 8; i++)
		pad[pn + i] = (uint8_t)(bits >> (8 * i));
	md5_update(c, pad, pn + 8);
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 4; k++)
			out[i * 4 + k] = (uint8_t)(c->h[i] >> (8 * k));
}

/* ---------------------------------------------------------------- SHA-256 (FIPS 180-4) */
static const uint32_t SHA_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha_block(Sha256 *c, const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
	}
	c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

void sha256_init(Sha256 *c)
{
	static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(c->h, H0, sizeof H0);
	c->len = 0;
}

void sha256_update(Sha256 *c, const void *data, size_t n)
{
	const uint8_t *p = data;
	size_t have = c->len & 63;
	c->len += n;
	if (have) {
		size_t k = 64 - have < n ? 64 - have : n;
		memcpy(c->buf + have, p, k);
		p += k, n -= k;
		if (have + k < 64)
			return;
		sha_block(c, c->buf);
	}
	for (; n >= 64; p += 64, n -= 64)
		sha_block(c, p);
	memcpy(c->buf, p, n);
}

void sha256_final(Sha256 *c, uint8_t out[32])
{
	uint64_t bits = c->len * 8;
	uint8_t pad[72] = {0x80};
	size_t have = c->len & 63, pn = have < 56 ? 56 - have : 120 - have;
	for (int i = 0; i < 8; i++)
		pad[pn + i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_update(c, pad, pn + 8);
	for (int i = 0; i < 8; i++)
		for (int k = 0; k < 4; k++)
			out[i * 4 + k] = (uint8_t)(c->h[i] >> (24 - 8 * k));
}

void digest_hex(const uint8_t *d, size_t n, char *out)
{
	static const char X[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++)
		out[i * 2] = X[d[i] >> 4], out[i * 2 + 1] = X[d[i] & 15];
	out[n * 2] = 0;
}
