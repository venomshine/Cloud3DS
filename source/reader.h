/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Lector: PDF, CBZ (manga y cómics), EPUB, DOCX y TXT con MuPDF.
 * - Todo lo de MuPDF va en su propio hilo (su contexto no se comparte): la interfaz pide páginas ya dibujadas a un
 *   tamaño y con un giro, y las recoge cuando están (RGB565). Las siguientes se preparan por adelantado.
 * - Los documentos que se recolocan (TXT, DOCX, EPUB) se paginan para el tamaño de la pantalla y la letra elegida;
 *   al cambiarlos, se vuelve a la misma parte del texto.
 * - La biblioteca busca en la SD (en su propio hilo, como la de música) y guarda en APP_DIR/lector.json cada
 *   documento y por dónde se iba leyendo. */

enum { RK_PDF, RK_CBZ, RK_EPUB, RK_DOCX, RK_TXT, RK_N };
int  rd_kind(const char *path); /* RK_* o -1 si no es algo que lea el Lector */

/* ---------------------------------------------------------------- biblioteca */
#define RLIB_MAX 2000
typedef struct {
	char path[256];
	char title[96];   /* (el del documento, si lo tiene; si no, vacío y se usa el nombre del archivo) */
	u8   kind;        /* RK_* */
	u64  size, mtime;
	int  page, pages; /* por dónde va (desde 0) y cuántas tenía la última vez */
	u64  last;        /* cuándo se leyó por última vez (0: nunca) */
} RBook;

enum { RLS_ALL, RLS_BOOKS }; /* dónde buscar: toda la SD o solo las carpetas de lectura (/Books, /Manga…) */

void rlib_init(void);
void rlib_exit(void);
void rlib_scan(void);
bool rlib_scanning(int *found);
u32  rlib_rev(void);
int  rlib_lock(const RBook **out); /* la lista (no cambia mientras se tiene) */
void rlib_unlock(void);
int  rlib_where(void);
void rlib_set_where(int w);
/* por dónde va un documento (también de uno que no está en la lista: se añade). title puede ser NULL */
void rlib_progress(const char *path, int page, int pages, const char *title);
int  rlib_last_page(const char *path, int *pages); /* por dónde iba (y cuántas tenía); -1 si no se había abierto */
void rlib_forget(const char *path);    /* quitar (borrado) */

/* ---------------------------------------------------------------- documento abierto */
enum { RD_NONE, RD_OPENING, RD_OPEN, RD_ERROR };
bool rd_open(const char *path);   /* en su hilo; false si el hilo no se pudo crear */
void rd_close(void);
int  rd_state(int *pages, bool *reflow, char *err, size_t en);
void rd_title(char *out, size_t n); /* (del documento, si lo trae) */
/* recolocar (TXT, DOCX, EPUB): página de w×h puntos con letra de em puntos; keep: la página que se está leyendo.
 * Devuelve el número de la generación: rd_relaid dice a qué página fue a parar keep cuando termina */
u32  rd_layout(float w, float h, float em, int keep);
bool rd_relaid(u32 gen, int *page, int *pages);

/* una página dibujada dentro de tw×th (fit 0: entera; 1: a lo ancho, hasta 1024 de alto), girada rot (0, 90, 270) */
typedef struct {
	int page, tw, th, rot, fit;
} RdReq;
/* si ya está, la copia en *px (malloc, w×h RGB565) y devuelve true; si no, la pide (y las de prefetch después) */
bool rd_get(const RdReq *r, u16 **px, int *w, int *h);
void rd_prefetch(const RdReq *r); /* prepararla sin esperarla */

/* portada (página 1, dentro de tw×th) de un documento cualquiera, para la biblioteca: igual que rd_get */
bool rd_cover(const char *path, int tw, int th, u16 **px, int *w, int *h);

void rd_exit(void);
