/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Biblioteca de música: busca en la SD los archivos de audio (MP3, OGG y WAV) en su propio hilo, lee sus etiquetas
 * (título, artista, álbum y pista: ID3v1/ID3v2 y comentarios Vorbis) y las guarda en APP_DIR/musica.json para no
 * volver a leer lo que no cambió. Lo que no trae etiquetas queda marcado (notag) para agruparlo aparte. */

#define MLIB_MAX 3000

typedef struct {
	char path[256];
	char title[96], artist[64], album[64];
	u16  track;
	bool notag;   /* sin título, artista ni álbum en sus etiquetas */
	bool hidden;  /* ocultada por el usuario */
	u64  size, mtime;
} MSong;

enum { MLS_ALL, MLS_MUSIC }; /* dónde buscar: toda la SD o solo /Music (y /música, /MP3…) */

void mlib_init(void);          /* lee la caché y, si nunca se buscó, busca */
void mlib_exit(void);
void mlib_scan(void);          /* volver a buscar (en su hilo; lo que no cambió no se vuelve a leer) */
bool mlib_scanning(int *found); /* buscando, y cuántos lleva */
u32  mlib_rev(void);           /* cambia con cada novedad */
/* la lista (no cambia mientras se tiene): devuelve cuántas */
int  mlib_lock(const MSong **out);
void mlib_unlock(void);
void mlib_hide(const char *path, bool hide);
void mlib_forget(const char *path); /* (borrada: fuera de la lista) */
int  mlib_where(void);              /* MLS_* */
void mlib_set_where(int w);         /* y vuelve a buscar */
