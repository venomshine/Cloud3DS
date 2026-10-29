/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* MD5 y SHA-256 en C puro (sin nada de la consola: se prueban en el PC). */

typedef struct {
	uint32_t h[4];
	uint64_t len;
	uint8_t buf[64];
} Md5;

typedef struct {
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
} Sha256;

void md5_init(Md5 *c);
void md5_update(Md5 *c, const void *data, size_t n);
void md5_final(Md5 *c, uint8_t out[16]);

void sha256_init(Sha256 *c);
void sha256_update(Sha256 *c, const void *data, size_t n);
void sha256_final(Sha256 *c, uint8_t out[32]);

void digest_hex(const uint8_t *d, size_t n, char *out); /* out: 2n+1 */
