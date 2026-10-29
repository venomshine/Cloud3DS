/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <stdbool.h>

/* Colores de código para el editor de texto, línea a línea. Es sencillo a
 * propósito: palabras clave, textos entre comillas, comentarios, números y
 * etiquetas. Los comentarios de varias líneas se siguen con un estado que pasa
 * de una línea a la siguiente. */

enum { HL_NONE, HL_C, HL_PY, HL_LUA, HL_SH, HL_INI, HL_SQL, HL_HTML, HL_CSS, HL_JSON, HL_MD };
enum { HC_TEXT, HC_KEY, HC_STR, HC_COM, HC_NUM, HC_TAG, HC_ATTR, HC_HEAD };

typedef struct {
	int start, len;
	unsigned char c; /* HC_* */
} HlSpan;

int hl_lang(const char *ext); /* por la extensión, sin el punto; HL_NONE si es texto normal */
/* parte la línea en tramos de color. *state entra con el de la línea anterior (0 al principio)
 * y sale con el de esta. out puede ser NULL para solo seguir el estado. Devuelve los tramos. */
int hl_line(int lang, const char *s, int *state, HlSpan *out, int max);
