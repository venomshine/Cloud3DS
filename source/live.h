/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Escribir desde el PC en el editor de la consola. La web manda trozos
 * («las líneas [i, i+d) pasan a ser este texto») que la consola aplica en
 * su editor; la consola publica el texto entero cada vez que cambia y la
 * web lo consulta. Se puede usar desde cualquier hilo. */

typedef struct {
	int i, d, cur;
	char *text; /* NULL: quitar esas líneas */
} LiveOp;

/* web: empezar en ese archivo (la consola lo abre en su editor) */
void live_open(const char *path, const char *device);
void live_close(void);
bool live_push(const char *path, int i, int d, const char *text, int cur); /* false: no está abierto así */
bool live_want_save(void);                 /* la consola: ¿lo pidió la web? (y lo olvida) */
void live_request_save(void);
/* consola */
bool live_take_open(char *path, size_t n); /* ¿hay que abrirlo? */
bool live_pop(LiveOp *op);                 /* el siguiente trozo; free(op->text) */
void live_publish(const char *path, const char *text, int cur, bool dirty, bool ok);
bool live_active(char *device, size_t n);  /* alguien escribe desde la web (y quién) */
/* web: el texto si cambió desde rev (malloc); NULL si no */
char *live_get(u32 since, u32 *rev, int *cur, bool *dirty, bool *ok, char *path, size_t pn);
