/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* MD5 y SHA-256 de un archivo, en un hilo aparte y de uno en uno (la consola y
 * la web ven el mismo). Al terminar mira si al lado hay un .md5/.sha256 que lo
 * mencione y dice si coincide. */

enum { HASH_IDLE, HASH_RUN, HASH_DONE, HASH_ERR };
enum { HCHK_NONE, HCHK_OK, HCHK_BAD };

typedef struct {
	int  st;
	char path[512];
	u64  size, done, mtime;
	char md5[33], sha[65];
	int  check;          /* lo que dice el .md5/.sha256 de al lado */
	char check_file[128]; /* cuál */
	u32  id;             /* cambia con cada cálculo nuevo */
} HashJob;

void hash_start(const char *path); /* si ya está (o se está calculando) ese mismo archivo sin cambios, no hace nada */
void hash_get(HashJob *out);
void hash_cancel(void);
int  hash_compare(const HashJob *j, const char *expected); /* HCHK_*; NONE si no parece un MD5 ni un SHA-256 */
int  hash_save_sha256(const HashJob *j);                   /* <nombre>.sha256 al lado; 0 bien */
