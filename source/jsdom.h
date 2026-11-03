/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* JavaScript de las páginas con QuickJS-ng sobre el árbol de libdom: los <script> se ejecutan en
 * orden (los externos se bajan), con document, window, temporizadores, fetch… Lo que cambien en el
 * árbol se ve al colocar la página. */

#include <dom/dom.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	/* bajar algo (scripts, fetch, XMLHttpRequest): 0 bien; *out con malloc (se libera fuera) */
	int (*fetch)(const char *url, const char *method, const char *body, size_t blen, char **out, size_t *n, long *code, char *final_url, size_t fl);
	void (*log)(const char *msg);
	const char *url;      /* la de la página */
	const char *ua;       /* navigator.userAgent */
	float width;          /* window.innerWidth */
	unsigned budget_ms;   /* tiempo máximo para todo el JavaScript de la página */
} JsHost;

typedef struct {
	int scripts, errors;  /* ejecutados y con error */
	unsigned ms;          /* lo que tardó */
	char navigate[1024];  /* la página pidió ir a otra dirección (location.href = …) */
} JsResult;

typedef struct JsPage JsPage;

/* ejecuta los scripts de la página y deja su JavaScript vivo para los clics (NULL si no se pudo) */
JsPage *jsdom_open(dom_document *doc, const JsHost *host, JsResult *res);
/* un clic en un elemento: sus eventos (también onclick="…"), lo que dispare y sus temporizadores.
 * *changed: el árbol cambió (hay que volver a colocar la página); devuelve false si la página
 * canceló lo que haría el navegador (preventDefault: no seguir el enlace). Se puede llamar desde
 * otro hilo, nunca a la vez que otra función de esta página. */
bool jsdom_click(JsPage *p, dom_node *target, bool *changed, JsResult *res);
void jsdom_close(JsPage *p);
