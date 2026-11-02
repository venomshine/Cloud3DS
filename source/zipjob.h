/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"
#include "zip.h"

/* Comprimir y extraer en un hilo aparte, de uno en uno, con su progreso en
 * las transferencias (pantalla de arriba y web). */

enum { ZJ_IDLE, ZJ_RUN, ZJ_DONE, ZJ_FAIL };

typedef struct {
	int  st, err;
	bool create;
	u64  done, total;
	char result[700]; /* el ZIP creado o la carpeta donde se extrajo */
	u32  id;
} ZipJob;

/* extraer zip en dest (si new_dir, en una carpeta nueva con su nombre al lado) */
int  zipjob_extract(const char *zip, bool new_dir, const char *device);
/* names de base → base/zipname (si ya existe, «x (2).zip») */
int  zipjob_create(const char *base, char (*names)[256], int n, const char *zipname, const char *device);
void zipjob_get(ZipJob *out);
void zipjob_cancel(void);
