/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ZIP: leer lo que trae, extraerlo, crear uno y las cabeceras para mandarlo
 * al vuelo por la web. Las rutas son de la SD ("/3ds/x"); ZIP_ROOT se pone
 * delante para abrirlas ("sdmc:" en la consola, "" en las pruebas del PC). */

#ifndef ZIP_ROOT
#define ZIP_ROOT "sdmc:"
#endif

enum {
	ZIP_OK = 0,
	ZIP_BAD = -1,      /* no es un ZIP o está dañado */
	ZIP_UNSUP = -2,    /* cifrado o con un método que no sabemos leer */
	ZIP_IO = -3,
	ZIP_NO_SPACE = -4,
	ZIP_CANCELED = -5,
	ZIP_CRC = -6,      /* un archivo salió distinto de como se guardó */
	ZIP_TOO_BIG = -7,  /* crear: más de 4 GB en total */
	ZIP_BUSY = -8,
	ZIP_EMPTY = -9,
};

typedef struct {
	char *name;        /* ruta dentro del ZIP, en UTF-8 y con '/' */
	uint32_t crc, dostime;
	uint64_t csize, usize, off;
	uint16_t method, flags;
	bool dir;
} ZipEnt;

typedef struct {
	ZipEnt *e;
	int n;
	uint64_t usize, csize;   /* lo que ocupa extraído y comprimido */
	int nfiles, ndirs;
	bool encrypted, unsupported;
} ZipDir;

typedef struct {
	volatile bool *cancel;
	volatile uint64_t *done;  /* bytes ya tratados (sin comprimir) */
} ZipProgress;

int  zip_read_dir(const char *zip, ZipDir *z);
void zip_dir_free(ZipDir *z);
/* lo de primer nivel (carpetas como «Midnight/») y cuántos archivos hay bajo cada una */
int  zip_top_level(const ZipDir *z, char (*names)[256], int *counts, uint64_t *sizes, bool *dirs, int max);

/* extrae todo en dest (que se crea); nunca pisa: si algo ya existe se guarda como «x (2)» */
int  zip_extract(const char *zip, const ZipDir *z, const char *dest, ZipProgress *pr);

/* lo que entraría en un ZIP con names (archivos o carpetas, dentro de base), recorriendo las carpetas */
typedef struct {
	char *rel;       /* ruta dentro del ZIP («Carpeta/a.txt», las carpetas acaban en '/'); en la SD: base/rel */
	bool dir;
	uint64_t size;
} ZipItem;
int  zip_plan(const char *base, char (*names)[256], int n, ZipItem **items, int *count, uint64_t *total);
void zip_plan_free(ZipItem *items, int count);
void zip_item_path(const char *base, const ZipItem *it, char *out, size_t n); /* su ruta en la SD */

/* crea dst con names (archivos o carpetas, dentro de base); total = bytes a leer */
int  zip_plan_size(const char *base, char (*names)[256], int n, uint64_t *total, int *count);
int  zip_create(const char *dst, const char *base, char (*names)[256], int n, ZipProgress *pr);

/* para mandarlo al vuelo sin comprimir (store): cabeceras sueltas */
uint32_t zip_dostime(uint64_t unix_s);
size_t zip_local_header(uint8_t *out, const char *name, uint32_t dostime, uint64_t size, bool dir, bool stream);
size_t zip_data_desc(uint8_t *out, uint32_t crc, uint64_t size);
size_t zip_central(uint8_t *out, const char *name, uint32_t dostime, uint32_t crc, uint64_t csize, uint64_t usize,
                   uint32_t off, bool dir, uint16_t method, bool stream);
size_t zip_end(uint8_t *out, int n, uint32_t cd_size, uint32_t cd_off);
uint32_t zip_crc(uint32_t crc, const void *p, size_t n);

/* la ruta de un nombre del ZIP, limpia: sin «..», sin '\', sin caracteres que FAT no admite; false si no queda nada */
bool zip_safe_name(const char *in, char *out, size_t n);
