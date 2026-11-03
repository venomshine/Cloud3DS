/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* Favoritos e historial del navegador. Se guardan en la SD, en dos archivos de texto (una línea
 * por página, con tabuladores) dentro de la carpeta de la app; no salen de la consola. */

#include <stdbool.h>
#include <stdint.h>

#define BM_FAV_MAX  48
#define BM_HIST_MAX 200

typedef struct {
	char title[96];
	char url[400];
} BmFav;

typedef struct {
	int64_t t;           /* cuándo (segundos, hora de la consola) */
	char title[96];
	char url[400];
} BmHist;

void bm_load(void);                      /* la primera vez lee los archivos; luego no hace nada */

int bm_nfav(void);
const BmFav *bm_fav(int i);
int bm_find_fav(const char *url);        /* -1 si no está */
bool bm_add_fav(const char *title, const char *url); /* false si ya no caben */
void bm_del_fav(int i);
void bm_rename_fav(int i, const char *title);
void bm_move_fav(int i, int to);

int bm_nhist(void);
const BmHist *bm_hist(int i);            /* 0 es la más reciente */
void bm_visit(const char *title, const char *url);
void bm_del_hist(int i);
void bm_clear_hist(void);

/* el nombre corto de una dirección (sin www.) y una letra y un color para su icono */
void bm_host(const char *url, char *out, int n);
char bm_letter(const char *title, const char *url);
uint32_t bm_color(const char *url);      /* ABGR, como los colores de la interfaz */
