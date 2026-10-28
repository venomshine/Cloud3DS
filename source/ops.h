/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Copiar y mover dentro de la SD. Mover es instantáneo (rename); copiar va
 * en su propio hilo para no frenar el servidor, de una en una. */

enum { OPS_OK = 0, OPS_NOT_FOUND = -1, OPS_IO = -2, OPS_TYPE = -3, OPS_NO_SPACE = -4, OPS_BUSY = -5, OPS_CANCELED = -6 };
enum { OPS_IDLE, OPS_RUNNING, OPS_DONE, OPS_FAILED };

bool ops_inside(const char *src, const char *dst);   /* dst es src o está dentro */
void ops_free_name(char *out, size_t n, const char *dir, const char *name); /* «nombre (2).ext» libre */
int  ops_move(const char *src, const char *dst);      /* si dst existe lo sustituye (carpetas: se combinan) */
int  ops_copy_start(const char *src, const char *dst, const char *device);
void ops_job(int *st, int *err, u64 *done, u64 *total);
void ops_cancel(void);
void ops_exit(void);
