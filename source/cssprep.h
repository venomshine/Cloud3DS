/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* CSS moderno a CSS que libcss entiende. libcss es de la época de CSS 2.1/3 y tira reglas enteras
 * (o bloques enteros) con lo que no conoce; las webs de hoy están llenas de eso. Aquí, antes de
 * dárselo:
 * - var(--x): se sustituye por su valor (las variables de :root/html/body, con el tema claro)
 * - @layer y @supports: se quita el envoltorio; @media: se resuelve para un PC (ancho fijo, tema
 *   claro, ratón) y se queda solo lo que vale
 * - :is()/:where(): se desdoblan; selectores con lo que libcss no entiende (:has(), ::-webkit-…)
 *   se quitan de la lista en vez de perder la regla entera
 * - propiedades lógicas (margin-inline, inset…) a las de siempre; clamp()/min()/max() a un valor
 * - lo que el navegador no usa (@font-face, @keyframes, @container…) se descarta: menos memoria */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CssPrep CssPrep;

/* el ancho de la página en px (para @media) y los atributos de <html> y <body> («nombre=valor»
 * separados por \n), para saber qué reglas definen las variables de la página */
CssPrep *cssprep_new(float width, const char *html_attrs, const char *body_attrs);
void cssprep_free(CssPrep *p);

void cssprep_vars(CssPrep *p, const char *css, size_t n);              /* 1.ª pasada: las variables */
char *cssprep_rewrite(CssPrep *p, const char *css, size_t n, size_t *out_n); /* 2.ª: malloc (o NULL) */
char *cssprep_inline(CssPrep *p, const char *decls, size_t n, size_t *out_n); /* style="…" */

/* los ámbitos de tema (tras la 1.ª pasada; cuántos cambian colores): cuáles tocan a un elemento por sus
 * atributos («nombre=valor
»; -1: ninguno) y el color que toma ahí uno de la página (tipo: 0 cualquiera,
 * 1 fondo, 2 texto, 3 borde; col en ARGB) */
int cssprep_scopes(CssPrep *p);
int cssprep_scope_match(CssPrep *p, const char *attrs, int parent);
bool cssprep_scope_col(CssPrep *p, int combo, uint32_t *col, int kind, int vid); /* vid: de orphans/widows */

/* modo oscuro: (prefers-color-scheme: dark) vale, y las webs que tienen tema oscuro lo usan */
void cssprep_set_dark(bool dark);

/* ¿vale esta lista de medios (atributo media o @media) en la consola? */
bool cssprep_media_ok(const char *q, float width);
