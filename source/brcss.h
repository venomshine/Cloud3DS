/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* CSS del navegador: las hojas (la del navegador, <style> y <link> de la página) y el estilo
 * calculado de cada elemento, con libcss sobre el árbol de libdom. */

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct BrCss BrCss;

BrCss *brcss_new(const char *base_url);                       /* con la hoja por defecto */
void   brcss_add(BrCss *c, const char *css, size_t n, const char *url); /* una hoja de la página */
/* cuando ya están todas: se traduce su CSS moderno (cssprep) y libcss las lee. width: el ancho de
 * la página, para @media */
void   brcss_finish(BrCss *c, dom_document *doc, float width);
int    brcss_sheets(const BrCss *c);                          /* cuántas de la página */
void   brcss_free(BrCss *c);

/* el estilo de un elemento ya combinado con el de su padre (NULL en la raíz); se suelta con
 * css_computed_style_destroy. vw: el ancho de la página, para vw/% de la ventana */
css_computed_style *brcss_style(BrCss *c, dom_node *el, const css_computed_style *parent, float vw);
/* lo mismo, con las variables de los style="" de sus antepasados (penv, «--a:x;»): env_out, las que pasan a
 * sus hijos si su style="" pone alguna (malloc; NULL: las mismas) */
css_computed_style *brcss_style_env(BrCss *c, dom_node *el, const css_computed_style *parent, float vw, const char *penv, char **env_out);

/* de la última llamada a brcss_style: el texto de ::before y ::after, y si ::after cierra los
 * flotantes (el «clearfix» de siempre) */
typedef struct {
	bool after_clear;
	char before[48], after[48];
	css_computed_style *st[2]; /* su estilo (::before, ::after), ya con el del elemento; quien lo toma lo suelta */
	bool icon[2];              /* su texto era un icono de una fuente propia (no se puede pintar: ocupa 1em) */
} BrcssPseudo;
extern BrcssPseudo brcss_ps;

/* una longitud del estilo, en px */
float brcss_px(const css_computed_style *s, css_fixed len, css_unit unit, float vw);

/* width: calc(…) con % (libcss lo deja sin resolver): sus px y su %; false si no es eso */
bool brcss_width_calc(const css_computed_style *s, float vw, float *px, float *pct);

/* olvidar los estilos que libcss guardó en el árbol (tras cambiarlo) */
void brcss_forget(dom_node *root);

/* temas por ámbitos («[data-color-mode=dark]», «.dark»…): en cuál está un elemento (el de su padre si sus
 * atributos no lo cambian; -1: ninguno) y el color que toma ahí uno de la página (css_color; tipo: 1 fondo,
 * 2 texto, 3 borde) */
int brcss_scope_of(BrCss *c, dom_node *el, int parent);
uint32_t brcss_scope_col(const BrCss *c, int scope, const css_computed_style *cs, uint32_t col, int kind);
