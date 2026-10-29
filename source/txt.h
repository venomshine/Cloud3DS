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

/* Archivos de texto por líneas, para verlos y editarlos en la consola.
 * Al guardar se conserva lo que tenía el archivo: saltos de Windows (CRLF) o
 * no, el BOM y si acababa en salto de línea. Solo se edita UTF-8 (y ASCII);
 * lo demás se abre en solo lectura para no estropearlo. */

#define TXT_EDIT_MAX (512 * 1024)      /* más grande: solo lectura */
#define TXT_VIEW_MAX (2 * 1024 * 1024) /* lo que se lee como mucho para verlo */

enum { TXT_RW, TXT_RO_BIG, TXT_RO_ENC, TXT_RO_BIN };
enum { TXT_OK = 0, TXT_ERR_IO = -1, TXT_ERR_MEM = -2 };

typedef struct {
	int i, oldn, newn;  /* las líneas [i, i+oldn) pasaron a ser newn líneas */
	char *oldt, *newt;  /* su texto antes y después (NULL: ninguna línea) */
} TxtOp;

typedef struct {
	char **ln;
	int n, cap;
	bool crlf, bom, final_nl;
	bool dirty;
	int ro;            /* TXT_RW o por qué es de solo lectura */
	bool cut;          /* solo se leyó el principio (archivo enorme) */
	uint64_t size;     /* al abrirlo, para avisar si otro lo cambió */
	TxtOp *ops;        /* deshacer: ops[0, pos) hechos, ops[pos, nops) para rehacer */
	int nops, pos;
	int saved;         /* pos al guardar (-1: ya no se puede volver a lo guardado) */
} TxtDoc;

int  txt_load(TxtDoc *d, const char *path);  /* ruta de la SD: «/notas.txt» */
int  txt_new(TxtDoc *d);                     /* documento vacío */
bool txt_set(TxtDoc *d, int i, const char *s);    /* cambia la línea i; si s trae saltos, se parte */
bool txt_replace(TxtDoc *d, int i, int count, const char *s); /* cambia count líneas desde la i por s */
char *txt_join(const TxtDoc *d, int i, int count);  /* esas líneas unidas con \n (malloc) */
bool txt_insert(TxtDoc *d, int at, const char *s); /* línea nueva en la posición at */
void txt_remove(TxtDoc *d, int i);
bool txt_move(TxtDoc *d, int i, int dir);  /* sube (-1) o baja (+1) la línea i */
int  txt_undo(TxtDoc *d);                  /* devuelve la línea cambiada o -1 */
int  txt_redo(TxtDoc *d);
bool txt_can_undo(const TxtDoc *d);
bool txt_can_redo(const TxtDoc *d);
int  txt_save(TxtDoc *d, const char *path);  /* escribe aparte y luego reemplaza: nunca queda a medias */
void txt_free(TxtDoc *d);
