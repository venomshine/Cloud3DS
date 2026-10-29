/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"
#include "fsx.h"

/* Buscar por nombre en una carpeta y todas sus subcarpetas. Sin distinguir
 * mayúsculas ni acentos («pokemon» encuentra «Pokémon»). «Nintendo 3DS» (todo
 * cifrado) y la papelera se saltan salvo que se busque dentro de ellas. */

#define SEARCH_DEPTH 24

typedef bool (*SearchCb)(const char *dir, const FsxEntry *e, void *u); /* false = parar */

void search_fold(const char *in, char *out, size_t n); /* minúsculas y sin acentos */
bool search_match(const char *name, const char *qfold);
/* devuelve cuántas carpetas recorrió; *cancel (si no es NULL) la para, y
 * max_ms (si no es 0) la corta y lo dice en *timed_out */
int  search_walk(const char *root, const char *q, volatile bool *cancel, u32 max_ms, bool *timed_out, SearchCb cb, void *u);
