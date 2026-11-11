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
/* las clases que aparecen en la página (antes de cssprep_vars): lo que exige un antepasado con una clase que no
 * está, no se aplica */
void cssprep_add_class(CssPrep *p, const char *name, size_t n);
void cssprep_classes_done(CssPrep *p);
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

/* variables por elemento (las de los style="" y las que pone una regla de clase; se heredan): los nombres
 * de las de style="", antes de reescribir; y para un elemento, las declaraciones de sus reglas que las usan,
 * ya resueltas (penv: las de sus antepasados, own: las de su style=""; «--a:x;--b:y»). malloc o NULL;
 * env_out: las que pasan a sus hijos si cambian (malloc o NULL) */
void cssprep_inline_vars(CssPrep *p, const char *decls, size_t n);
char *cssprep_env_decls(CssPrep *p, const char *tag, const char *attrs, const char *penv, const char *own, char **env_out);

/* @font-face: la dirección de la hoja que se lee ahora (antes de cssprep_vars); y de qué fuente sale una
 * familia: «dirección de su hoja\nurl» (TrueType, OpenType o WOFF), o NULL */
void cssprep_set_base(CssPrep *p, const char *url);
const char *cssprep_font(const CssPrep *p, const char *family);

/* modo oscuro: (prefers-color-scheme: dark) vale, y las webs que tienen tema oscuro lo usan */
void cssprep_set_dark(bool dark);

/* ¿vale esta lista de medios (atributo media o @media) en la consola? */
bool cssprep_media_ok(const char *q, float width);
