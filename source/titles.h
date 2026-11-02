/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Títulos instalados en la SD (juegos y apps; las actualizaciones y DLC van
 * con su juego) y copias de sus partidas, en la carpeta y el formato de
 * Checkpoint (/3ds/Checkpoint/saves/0xXXXXX Nombre/AAAAMMDD-HHMMSS/). Lo del
 * sistema ni se lista. Restaurar una copia no se hace aquí: con Checkpoint. */

#define CHECKPOINT_DIR "/3ds/Checkpoint/saves"

typedef struct {
	u64  tid;
	u16  version, upd_version;
	u64  size, upd_size, dlc_size;
	bool has_upd;
	int  dlc;          /* contenidos descargables instalados */
	char title[64];    /* en el idioma de la consola */
	char short_en[64]; /* el corto en inglés: el que usa Checkpoint para su carpeta */
	char pub[64];
	bool smdh;         /* se pudo leer su nombre e icono */
} TitleInfo;

/* los juegos y apps de la SD, por nombre; *out se libera con free */
int  titles_list(TitleInfo **out);
bool title_icon(u64 tid, u16 *px48);             /* 48×48 RGB565 */
bool title_is_self(u64 tid);                     /* Cloud3DS */
int  title_uninstall(u64 tid);                   /* el juego, su actualización y sus DLC; 0 bien */
/* copia la partida a la carpeta de Checkpoint; out = la carpeta de la copia. <0: -1 sin partida, -2 error */
int  save_backup(const TitleInfo *t, char *out, size_t n);
int  save_backups_count(const TitleInfo *t, u64 *last); /* cuántas copias hay y la fecha de la última */
void save_dir(const TitleInfo *t, char *out, size_t n);  /* su carpeta de copias (puede no existir aún) */
