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
	/* headers: las que pide la página («Nombre: valor» por línea, o NULL); ctype: el Content-Type de la respuesta */
	int (*fetch)(const char *url, const char *method, const char *headers, const char *body, size_t blen, char **out, size_t *n, long *code,
	             char *final_url, size_t fl, char *ctype, size_t cl);
	void (*log)(const char *msg);
	const char *url;      /* la de la página */
	const char *ua;       /* navigator.userAgent */
	float width;          /* window.innerWidth */
	unsigned budget_ms;   /* tiempo máximo para todo el JavaScript de la página */
	volatile bool *cancel; /* si se pone a true, el JavaScript se para en cuanto pueda */
	size_t (*cookie_get)(const char *url, char *out, size_t n); /* document.cookie */
	void (*cookie_set)(const char *url, const char *set_cookie);
	size_t mem_limit;     /* memoria máxima para el JavaScript (0: la de siempre) */
	bool dark;            /* (prefers-color-scheme: dark) */
	/* dónde quedó un elemento en la página (x, y, ancho, alto en px; false: no se colocó). NULL: la página entera.
	 * (puede ser NULL: sin medidas) */
	bool (*rect)(dom_node *n, float r[4]);
} JsHost;

typedef struct {
	int scripts, errors;  /* ejecutados y con error */
	unsigned ms;          /* lo que tardó */
	char navigate[1024];  /* la página pidió ir a otra dirección (location.href = …) */
	bool oom;             /* se quedó sin memoria: su JavaScript ya no sigue */
	size_t mem;           /* memoria que ocupa su JavaScript */
	bool changed;         /* (jsdom_open) tocó el árbol: hay que volver a colocar la página */
} JsResult;

typedef struct JsPage JsPage;

/* ejecuta los scripts de la página y deja su JavaScript vivo para los clics (NULL si no se pudo) */
JsPage *jsdom_open(dom_document *doc, const JsHost *host, JsResult *res);
/* un clic en un elemento: sus eventos (también onclick="…"), lo que dispare y sus temporizadores.
 * *changed: el árbol cambió (hay que volver a colocar la página); devuelve false si la página
 * canceló lo que haría el navegador (preventDefault: no seguir el enlace). Se puede llamar desde
 * otro hilo, nunca a la vez que otra función de esta página. */
bool jsdom_click(JsPage *p, dom_node *target, bool *changed, JsResult *res);
/* lo que la página dejó para después (temporizadores, scripts que carga): un rato de ms. *changed: el
 * árbol cambió. Devuelve si aún le queda algo por hacer. Mismas reglas de hilos que jsdom_click. */
bool jsdom_idle(JsPage *p, unsigned ms, bool *changed, JsResult *res);
void jsdom_close(JsPage *p);
