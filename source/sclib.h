/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "sc.h"

/* La biblioteca de SoundCloud de la consola: las canciones con «Me gusta» y las listas propias, en
 * APP_DIR/soundcloud_biblioteca.json. De cada canción se guarda lo que se enseña (número, título, artista, autor y
 * duración); para escucharla se pide de nuevo a SoundCloud (sc_open_ids), porque su enlace de audio caduca. */

#define SCL_TRACKS SC_MAX /* por lista (y en «Me gusta») */
#define SCL_LISTS  24
#define SCL_LIKES  (-1)   /* «lista» de las canciones con «Me gusta» */

typedef struct {
	char id[24];
	char title[128], artist[96];
	char uid[16];
	u32  dur_ms;
} ScSaved;

void scl_load(void);
bool scl_liked(const char *id);
/* 1 hecho; 0 ya estaba (o no estaba); -1 lleno */
int  scl_like(const ScTrack *t, bool on);
int  scl_nlists(void);
const char *scl_list_name(int i);
int  scl_count(int list);                 /* SCL_LIKES o una lista */
int  scl_list_new(const char *name);      /* su índice, o -1 */
bool scl_list_rename(int i, const char *name);
void scl_list_delete(int i);
int  scl_add(int list, const ScTrack *t); /* como scl_like */
void scl_remove(int list, const char *id);
bool scl_has(int list, const char *id);
int  scl_tracks(int list, ScSaved *out, int max);
/* los números de sus canciones, separados por comas (para sc_open_ids) */
void scl_ids(int list, char *out, size_t n);
