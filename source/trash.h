/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Papelera: borrar mueve el archivo o la carpeta (renombrar: instantáneo) a
 * /3ds/Cloud3DS/trash/<id>/ y apunta de dónde venía en trash/index.json. Se
 * recupera a su sitio o se borra del todo; lo que lleva más de TRASH_DAYS días
 * se borra solo. Se puede usar desde cualquier hilo. */

#define TRASH_DIR "/3ds/Cloud3DS/trash"
#define TRASH_DAYS 7
#define TRASH_MAX 256

typedef struct {
	u32  id;
	char name[128];
	char from[512];   /* ruta original completa */
	char device[48];  /* quién lo borró */
	u64  when;        /* unix */
	u64  size;        /* archivos: su tamaño; carpetas: lo que suman (al borrarlas) */
	bool dir;
	u8   color;       /* el que tenía, para devolvérselo */
} TrashItem;

void trash_init(void);
int  trash_put(const char *path, const char *device, u8 color); /* 0 bien; <0 error (entonces no se movió) */
int  trash_list(TrashItem *out, int max);                        /* lo más reciente primero */
bool trash_get(u32 id, TrashItem *out);
int  trash_restore(u32 id, char *dst, size_t n);                 /* 0 bien: dst = dónde quedó */
int  trash_purge(u32 id);                                        /* borrar del todo */
int  trash_empty(void);                                          /* cuántos se borraron */
void trash_expire(void);                                         /* los de más de TRASH_DAYS días */
u64  trash_size(int *count);
u64  trash_make_room(u64 need);                                  /* borra lo más viejo hasta liberar need bytes */
