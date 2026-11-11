/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* Maquetación de páginas web: como un navegador de escritorio, siguiendo las reglas de CSS.
 *  1. Con el árbol de libdom y el estilo de cada elemento (libcss) monta un árbol de cajas.
 *  2. Las coloca: bloques (con sus márgenes que se juntan), texto en líneas (alineado, alrededor de
 *     los flotantes), flex, rejilla, tablas y posiciones (relative, absolute).
 *  3. Entrega lo que se pinta (fondos, palabras, imágenes y controles) ya en su sitio, en px de la
 *     página, por las funciones de BrLay. Luego suelta todo: no queda nada en memoria. */

#include "brcss.h"

#include <dom/dom.h>
#include <libcss/libcss.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	float px;           /* tamaño de la letra */
	uint32_t col;       /* color (ABGR, como la interfaz) */
	int16_t link, clk;  /* enlace y «se toca por JavaScript» (-1 no) */
	uint8_t bold, under;
	uint8_t fit;        /* imágenes: object-fit (0 cover, 1 contain, 2 fill) */
	dom_node *node;     /* el elemento del que sale */
} BrlText;

/* una imagen de fondo (background-image) en la caja x0,y0-x1,y1 */
enum { BRL_BG_AUTO, BRL_BG_COVER, BRL_BG_CONTAIN, BRL_BG_FILL, BRL_BG_WIDTH };
typedef struct {
	const char *url;     /* ya completa (respecto a su hoja) */
	float x0, y0, x1, y1;
	uint8_t size;        /* BRL_BG_* */
	float sw;            /* BRL_BG_WIDTH: el ancho */
	float px, py;        /* background-position: en px o en % (pxp/pyp) */
	uint8_t pxp, pyp;
	uint8_t rep;         /* se repite (x 1, y 2) */
} BrlBg;

typedef struct {
	BrCss *css;
	float vw, vh;       /* la ventana (px) */
	bool light;         /* modo ligero: las imágenes no ocupan sitio */
	void *u;
	/* medir texto (px de página) con una letra de px de tamaño */
	float (*text_w)(void *u, const char *s, int n, float px, bool bold);
	float (*line_h)(void *u, float px); /* el alto de una línea de la letra (sin interlineado) */
	/* al montar el árbol (-1: no hay) */
	int (*link)(void *u, dom_node *a, const char *href);
	int (*clk)(void *u, dom_node *n);
	/* una imagen: su número, y su tamaño si ya se sabe (0 si no) */
	int (*img)(void *u, dom_node *n, const char *src, float *iw, float *ih);
	/* un <svg> de la página: su número y su tamaño (o -1) */
	int (*svg)(void *u, dom_node *n, const css_computed_style *cs, float *w, float *h);
	/* un control de formulario: su número y su tamaño (o -1) */
	int (*wdg)(void *u, dom_node *n, const char *tag, const css_computed_style *cs, float px, float *w, float *h);
	/* lo que se pinta, en orden (lo de después, encima) */
	void (*bg)(void *u, float x0, float y0, float x1, float y1, uint32_t col, const float bw[4], const uint32_t bc[4], float radius);
	void (*text)(void *u, float x, float y, float w, float h, const char *s, int n, const BrlText *t);
	void (*box)(void *u, float x, float y, float w, float h, int img, int wdg, const BrlText *t);
	void (*bgimg)(void *u, const BrlBg *b, const BrlText *t); /* (puede ser NULL) */
	/* dónde queda cada elemento (caja con borde, en px de página; los en línea, por cada trozo de su texto), para
	 * las medidas que pide el JavaScript (getBoundingClientRect, offsetWidth…). (puede ser NULL) */
	void (*geom)(void *u, dom_node *n, float x, float y, float w, float h, bool block);
	/* un icono de una fuente propia de la página: la imagen de su glifo (cp) en ese color (ABGR), o -1
	 * (puede ser NULL) */
	int (*glyph)(void *u, const char *font_url, uint32_t cp, uint32_t col);
	/* parar: la página se deja o no queda memoria */
	bool (*stop)(void *u);
	bool frames;         /* los <iframe> ocupan su hueco (el navegador pinta su página dentro) */
} BrLay;

/* maqueta la página; devuelve su alto. page_bg: el fondo de la página (de <html> o <body>; 0 si no) */
float brl_run(BrLay *L, dom_document *doc, uint32_t *page_bg);
