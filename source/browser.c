/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Navegador ligero (prototipo).
 *  1. Un hilo baja la página (httpc, como las descargas) o la lee de la SD y la convierte en un
 *     árbol DOM con hubbub + libdom (de NetSurf, MIT).
 *  2. El hilo principal recorre el árbol y lo maqueta: cada palabra es un trozo con su tamaño,
 *     color y enlace, colocado en líneas que caben en la pantalla de abajo.
 *  3. Se dibujan solo las líneas que se ven; tocar un trozo con enlace abre esa página. */
#include "browser.h"
#include "brcss.h"
#include "cssprep.h"
#include "brfav.h"
#include "core/node.h" /* libdom por dentro: hijos y hermanos sin referencias */
#include "dl.h"
#include "gfx.h"
#include "icons.h"
#include "img.h"
#include "svg.h"
#include "jsdom.h"
#include "ui.h"
#include "util.h"

#include <dom/bindings/hubbub/parser.h>
#include <dom/dom.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <malloc.h>

#define MAX_PAGE  (6 * 1024 * 1024)
#define HIST      16
#define HDR_H     30.0f
#define SC_TEXT   0.46f
/* cómo se presenta: en Móvil, como el navegador de la 3DS (las webs dan su versión sencilla); en PC,
 * como uno de escritorio */
#define UA_MOB "Mozilla/5.0 (Nintendo 3DS; U; ; es) Version/1.7630.EU Cloud3DS"
#define UA_PC  "Mozilla/5.0 (X11; Linux x86_64; rv:115.0) Gecko/20100101 Firefox/115.0 Cloud3DS"
/* Google solo busca con navegadores que conoce: al de la 3DS le dice que se actualice */
#define UA_GOOGLE_MOB "Mozilla/5.0 (Android 14; Mobile; rv:128.0) Gecko/128.0 Firefox/128.0"
#define VW_PC     1024.0f  /* ancho de la página: siempre de PC (muchas webs pasan a escritorio en 1012-1024) */
#define VW_MOB    400.0f   /* ancho «de móvil» (las webs que lo piden con <meta viewport>) */
#define ZOOM_MAX  2.0f
/* el ancho de la página y su margen (cambian con PC / móvil) */
static float PW = VW_PC - 32, PADP = 16;
#define PAGE_W PW
#define PAD_X  PADP
/* colores de la página: como una web, fondo claro */
#define PG_BG    col_hex(0xFFFFFF)
#define PG_TEXT  col_hex(0x202122)
#define PG_HEAD  col_hex(0x000000)
#define PG_MUTED col_hex(0x72777D)
#define PG_LINK  col_hex(0x3366CC)
#define PG_CODE  col_hex(0x8B2252)
#define PG_RULE  col_hex(0xC8CCD1)

typedef struct {
	int line;
	float x, w;
	u32 off;       /* en B.text */
	u16 n;
	float sc;
	u32 col;
	s16 link;      /* -1 sin enlace */
	u8 under;      /* subrayado (enlaces) */
	u8 bold;       /* negrita: se pinta dos veces, medio píxel corrida */
	s16 img;       /* imagen que ocupa este trozo (-1 texto) */
	s16 wdg;       /* control de formulario (-1 ninguno) */
	dom_node *node; /* el elemento del que sale (para los clics del JavaScript) */
	s16 clk;       /* se puede tocar por su JavaScript (onclick, role=button…): su número, o -1 */
} Frag;

/* controles de formulario: cuadros de texto, botones, casillas y listas */
enum { WG_TEXT, WG_PASS, WG_AREA, WG_BUTTON, WG_CHECK, WG_RADIO, WG_SELECT };
typedef struct {
	dom_node *node;   /* el elemento (vive lo que el documento) */
	dom_node *form;   /* su <form> (o NULL) */
	u8 type;
	char label[96];   /* lo que se ve: texto del botón, placeholder u opción */
} Wdg;

/* lo que el usuario escribió o marcó (sobrevive a recolocar la página; se pierde al cambiar de página) */
typedef struct {
	dom_node *node;
	char *value;
	int checked;      /* -1 sin tocar */
} FVal;

/* una imagen de la página: se baja y se decodifica en su hilo; la textura la crea el principal */
enum { IM_WAIT, IM_RGBA, IM_TEX, IM_FAIL };
typedef struct {
	char *url;
	float w, h;            /* tamaño en la página (px) */
	float y;               /* dónde está (para bajar antes lo que se ve) */
	bool sized;            /* el tamaño venía de la página */
	volatile int st;
	u8 *rgba;
	int tw, th, iw, ih;    /* textura y tamaño original */
	Texture tex;
} PImg;

typedef struct {
	float y, h, gap;
	bool hr;       /* <hr>: una raya en vez de texto */
	bool used;     /* tiene texto: las vacías no ocupan alto */
	u8 align;      /* 0 izquierda, 1 centro, 2 derecha */
	float left, right; /* márgenes de la línea (para alinear) */
} Line;

typedef struct {   /* fondo de color y bordes de un bloque */
	float y0, y1, x0, x1;
	u32 col;       /* fondo (0: sin fondo) */
	float bw[4];   /* bordes arriba, derecha, abajo, izquierda */
	u32 bc[4];
} BgBox;

enum { ST_IDLE, ST_LOADING, ST_PARSED, ST_LAYOUT, ST_READY, ST_ERROR, ST_ACT /* el JavaScript atiende un clic */, ST_OOM /* no cabe: el aviso */ };
/* memoria: lo que siempre se deja libre para la app, y por qué se paró una página */
#define MEM_RESERVE (4u * 1024 * 1024)
#define MEM_PER_HTML 12u   /* el árbol ocupa unas 9 (Wikipedia) a 16 veces lo que pesa el HTML */
enum { OOM_NONE, OOM_JS, OOM_PARSE, OOM_LAYOUT };

static struct {
	bool open;
	volatile int st;
	char url[1024], err[128], title[160];
	char hist[HIST][1024];
	float hist_sy[HIST];   /* dónde se estaba en cada página (para volver ahí) */
	float back_sy;         /* al volver atrás: a dónde llevar la vista al colocar la página */
	char anchor[128];      /* la #ancla de la dirección, para ir ahí al colocar la página */
	int nhist;
	Thread thr;
	dom_document *doc;   /* del hilo al principal */
	size_t bytes;
	/* maquetación */
	Frag *f;
	int nf, cf;
	Line *l;
	int nl, cl;
	char *text;
	u32 nt, ct;
	char **links;
	int nlinks, clinks;
	BgBox *bg;
	int nbg, cbg;
	int *ord;            /* los trozos ordenados por altura */
	Wdg *wg;
	int nwg, cwg;
	FVal *fv;
	int nfv, cfv;
	float bg_x0, bg_x1;
	BrCss *css;          /* las hojas de la página (las monta el hilo de carga) */
	int ncss;            /* cuántas hojas se usaron */
	u32 t_css;           /* ms que costó calcular los estilos */
	float total, off, vel;
	int hover;           /* enlace bajo el puntero */
	float t_load;
	/* vista: la página a ancho de PC (o de móvil) arriba, con zoom y puntero */
	bool page_mobile;    /* la página pide ancho de móvil (<meta name=viewport … device-width>) */
	int force;           /* 0 lo que pida la página, 1 PC, 2 móvil */
	float vw, zoom, sx, sy, vy; /* ancho, zoom (px de pantalla por px de página), desplazamiento e inercia */
	float px, py;        /* puntero, en la pantalla de arriba */
	int focus;           /* trozo con enlace marcado con la cruceta (-1 ninguno) */
	int hw;              /* control de formulario bajo el puntero */
	float tip;           /* tiempo enseñando el % de zoom */
	volatile int nel_done, nel_total; /* nodos colocados y total (para el progreso) */
	bool js;             /* ejecutar el JavaScript de las páginas */
	int js_scripts, js_errors;
	char js_nav[1024];   /* la página pidió ir a otra dirección desde su JavaScript */
	JsPage *jsp;         /* el JavaScript de la página, vivo para los clics */
	volatile bool cancel, in_js; /* B mientras carga: parar su JavaScript; está ejecutándose */
	int idle_left;       /* ms que aún puede seguir su JavaScript con la página a la vista */
	u64 idle_next, last_in; /* cuándo darle otro rato; última vez que se tocó algo */
	u64 ready_ms;        /* cuándo quedó lista la página */
	char search_q[512];  /* lo buscado (por si el buscador no responde: el siguiente) */
	int search_stage;    /* 0 Google, 1 DuckDuckGo, 2 Bing */
	int http_code;       /* lo que respondió el servidor a la página */
	/* inicio, favoritos e historial */
	bool home;           /* en la página de inicio */
	int panel;           /* con una página: la lista encima del touchpad (0 no) */
	bool came_home;      /* se llegó desde el inicio: Atrás en la primera página vuelve a él */
	int tab, lsel;       /* Favoritos o Historial; lo marcado con la cruceta */
	float lsy;           /* desplazamiento de la lista */
	bool lkeys;          /* se usa la cruceta (se ve lo marcado) */
	bool opt, opt_just, opt_confirm; /* hoja de opciones de un elemento */
	int opt_li, opt_sel;
	char logged[1024];   /* la última página apuntada en el historial */
	/* memoria */
	int oom;             /* OOM_*: la página se ve, pero sin terminar */
	u32 oom_need;        /* lo que hacía falta (aproximado, bytes) */
	char reader_url[1024]; /* esta dirección, en modo lectura (sin JavaScript, CSS ni imágenes) */
	volatile bool lay_oom; /* la colocación se paró por memoria */
	u32 page_bg;           /* el fondo de la página (de <html> o <body>; 0 = blanco) */
	bool page_bg_html;
	volatile u32 page_ver; /* cambia con lo que se ve (colocación, imágenes, formularios): repintar la caché */
	bool light;            /* modo ligero: sin imágenes ni JavaScript (se guarda en la SD) */
	bool click_wait;       /* un clic hecho mientras su JavaScript trabajaba: se hace al acabar */  /* lo buscado en Google (por si Google no responde: DuckDuckGo) */
	int idle_relayouts;  /* recolocaciones por su JavaScript de fondo (como mucho 3) */
	bool idle_dirty;     /* cambió algo más que aún no se ve: se recoloca con el próximo clic */
	int nclk;            /* cosas que se tocan por JavaScript */
	volatile int click_link; /* tras un clic con JavaScript: el enlace a seguir (-1 ninguno, -2 esperando) */
	PImg *img;
	int nimg, cimg;
	Thread img_thr;
	volatile bool img_quit;
	size_t img_px;       /* píxeles decodificados (límite de memoria) */
} B;

static void img_free_all(void);
static bool blocked_host(const char *url);
static void fv_free(void);

/* ------------------------------------------------------------ cargar (en un hilo) */
typedef struct {
	char *p;
	size_t n, cap;
} Buf;

static size_t buf_cb(char *d, size_t s, size_t m, void *u)
{
	Buf *b = u;
	size_t k = s * m;
	if (b->n + k > MAX_PAGE)
		return 0;
	if (b->n + k > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 128 * 1024;
		while (nc < b->n + k)
			nc *= 2;
		char *np = realloc(b->p, nc);
		if (!np)
			return 0;
		b->p = np, b->cap = nc;
	}
	memcpy(b->p + b->n, d, k);
	b->n += k;
	return k;
}

static bool utf8_ok(const char *p, size_t n)
{
	size_t lim = n < 256 * 1024 ? n : 256 * 1024, i = 0;
	bool multi = false;
	while (i < lim) {
		unsigned char c = p[i];
		int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (k < 0)
			return false;
		if (i + k >= lim)
			break;
		for (int j = 1; j <= k; j++)
			if ((p[i + j] & 0xC0) != 0x80)
				return false;
		multi |= k > 0;
		i += k + 1;
	}
	return multi;
}

static void resolve(const char *base, const char *href, char *out, size_t n);
static bool attr(dom_node *el, const char *name, char *out, size_t n);

/* el núcleo de los hilos del navegador: en New 3DS, el 2 (libre: la interfaz no les quita la CPU en
 * cada fotograma); en la 3DS antigua, el de la app */
static int br_core(void)
{
	static int core = -3;
	if (core == -3) {
		bool n3ds = false;
		APT_CheckNew3DS(&n3ds);
		core = n3ds ? 2 : -2;
	}
	return core;
}

/* servidores con los que httpc no puede (TLS moderno, o lo rechazan): directos a libcurl, sin
 * perder el intento con httpc en cada hoja, script o imagen */
#define NOHC_N 32
static char NOHC[NOHC_N][96];
static int NOHC_I;
static LightLock NOHC_LOCK;
static bool NOHC_INIT;

static void nohc_host(const char *url, char *h, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t l = strcspn(p, "/?#");
	snprintf(h, n, "%.*s", (int)(l < n - 1 ? l : n - 1), p);
}

static void nohc_init(void) // desde el hilo principal, al abrir el navegador
{
	if (!NOHC_INIT) {
		LightLock_Init(&NOHC_LOCK);
		NOHC_INIT = true;
	}
}

static bool nohc_has(const char *url)
{
	char h[96];
	nohc_host(url, h, sizeof h);
	bool r = false;
	LightLock_Lock(&NOHC_LOCK);
	for (int i = 0; i < NOHC_N && !r; i++)
		r = NOHC[i][0] && !strcmp(NOHC[i], h);
	LightLock_Unlock(&NOHC_LOCK);
	return r;
}

static void nohc_add(const char *url)
{
	if (nohc_has(url))
		return;
	LightLock_Lock(&NOHC_LOCK);
	nohc_host(url, NOHC[NOHC_I], sizeof NOHC[0]);
	NOHC_I = (NOHC_I + 1) % NOHC_N;
	LightLock_Unlock(&NOHC_LOCK);
}

// un archivo de la SD (ruta que empieza por / o sdmc:) o de Internet, entero en b
static int fetch_any(const char *url, const char *accept, Buf *b, long *code)
{
	*code = 0;
	if (!strncasecmp(url, "sdmc:", 5) || url[0] == '/') {
		char sp[1100];
		snprintf(sp, sizeof sp, "%s%s", url[0] == '/' ? "sdmc:" : "", url);
		FILE *fp = fopen(sp, "rb");
		if (!fp)
			return DLF_NOCONN;
		char tmp[16384];
		size_t n;
		while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0 && buf_cb(tmp, 1, n, b) == n)
			;
		fclose(fp);
		*code = 200;
		return DLF_OK;
	}
	bool skip = nohc_has(url);
	int r = skip ? DLF_NOCONN : dl_fetch(url, accept, buf_cb, NULL, b, code);
	// algunas webs rechazan el TLS antiguo de httpc (403, 429…) o no conecta: otra vez con libcurl
	if (r != DLF_OK || *code == 403 || *code == 429 || *code == 503) {
		if (!skip)
			util_log("web: httpc no pudo con %.100s (%d, HTTP %ld): libcurl", url, r, *code);
		free(b->p);
		b->p = NULL, b->n = b->cap = 0;
		r = dl_fetch_curl(url, accept, buf_cb, b, code);
		if (r == DLF_OK && *code < 400 && !skip)
			nohc_add(url);
	}
	return r;
}

static bool ci_has(const char *h, const char *nd)
{
	size_t l = strlen(nd);
	for (; *h; h++)
		if (!strncasecmp(h, nd, l))
			return true;
	return false;
}

/* las hojas de estilo de la página, en su orden: <style> y <link rel="stylesheet"> (se bajan aquí) */
#define CSS_MAX_LINKS 24
#define CSS_MAX_BYTES (2560 * 1024) /* lo que se baja; traducido (cssprep) ocupa bastante menos */
static char CSS_SEEN[CSS_MAX_LINKS][96]; /* las ya bajadas (una web enlaza la misma varias veces) */
static int CSS_NSEEN;
// un <style> o un <link rel="stylesheet">; aparte, para que la recursión de css_collect no lleve
// estos búferes en cada nivel (con páginas profundas desbordaban la pila del hilo)
static __attribute__((noinline)) void css_one(BrCss *c, dom_node *el, bool is_style, int *links, size_t *bytes)
{
	char a[256];
	if (is_style) {
		if (attr(el, "media", a, sizeof a) && !cssprep_media_ok(a, VW_PC))
			return;
		dom_string *tx = NULL;
		if (dom_node_get_text_content(el, &tx) == DOM_NO_ERR && tx) {
			size_t l = dom_string_byte_length(tx);
			if (*bytes + l <= CSS_MAX_BYTES) {
				brcss_add(c, dom_string_data(tx), l, B.url);
				*bytes += l;
			}
			dom_string_unref(tx);
		}
		return;
	}
	if (*links >= CSS_MAX_LINKS || !attr(el, "rel", a, sizeof a) || !ci_has(a, "stylesheet") || ci_has(a, "alternate"))
		return;
	if (attr(el, "media", a, sizeof a) && !cssprep_media_ok(a, VW_PC)) // print, tema oscuro…
		return;
	char *href = malloc(2048), *abs = href ? href + 1024 : NULL;
	if (!href)
		return;
	if (attr(el, "href", href, 1024) && href[0]) {
		resolve(B.url, href, abs, 1024);
		const char *key = strlen(abs) > 95 ? abs + strlen(abs) - 95 : abs; // el final: el nombre con su huella
		for (int i = 0; i < CSS_NSEEN; i++)
			if (!strcmp(CSS_SEEN[i], key)) {
				free(href);
				return;
			}
		if (CSS_NSEEN < CSS_MAX_LINKS)
			util_strlcpy(CSS_SEEN[CSS_NSEEN++], key, sizeof CSS_SEEN[0]);
		Buf cb = {0};
		long code;
		(*links)++;
		int r = fetch_any(abs, "text/css,*/*;q=0.1", &cb, &code);
		util_log("web: css %s -> %d, HTTP %ld, %u bytes", abs, r, code, (unsigned)cb.n);
		if (r == DLF_OK && code < 400 && cb.n && *bytes + cb.n <= CSS_MAX_BYTES) {
			brcss_add(c, cb.p, cb.n, abs);
			*bytes += cb.n;
		}
		free(cb.p);
	}
	free(href);
}

static void css_collect(BrCss *c, dom_node *n, int depth, int *links, size_t *bytes)
{
	if (depth > 80)
		return;
	dom_node *ch = NULL;
	if (dom_node_get_first_child(n, &ch) != DOM_NO_ERR)
		return;
	while (ch) {
		dom_node_type t;
		if (dom_node_get_node_type(ch, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
			dom_string *nm = NULL;
			char tag[8] = "";
			if (dom_node_get_node_name(ch, &nm) == DOM_NO_ERR && nm) {
				size_t l = dom_string_byte_length(nm);
				snprintf(tag, sizeof tag, "%.*s", (int)(l < 7 ? l : 7), dom_string_data(nm));
				dom_string_unref(nm);
			}
			if (!strcasecmp(tag, "style") || !strcasecmp(tag, "link"))
				css_one(c, ch, !strcasecmp(tag, "style"), links, bytes);
			else if (strcasecmp(tag, "script") && strcasecmp(tag, "svg"))
				css_collect(c, ch, depth + 1, links, bytes);
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(ch, &nx);
		dom_node_unref(ch);
		ch = nx;
	}
}

/* ------------------------------------------------------------ cookies (de esta sesión del navegador) */
typedef struct {
	char dom[96], path[96], name[96], *val;
	bool host_only;
} Ck;
static Ck *CK;
static int NCK, CCK;
static LightLock CK_LOCK; /* las usan los hilos de carga, imágenes y JavaScript */
static bool CK_INIT;

static void ck_host(const char *url, char *host, size_t hn, char *path, size_t pn)
{
	const char *h = strstr(url, "://");
	h = h ? h + 3 : url;
	size_t l = strcspn(h, "/?#:");
	snprintf(host, hn, "%.*s", (int)l, h);
	for (char *p = host; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	const char *p = h + strcspn(h, "/?#");
	if (*p == '/')
		snprintf(path, pn, "%.*s", (int)strcspn(p, "?#"), p);
	else
		snprintf(path, pn, "/");
}

static bool ck_domain_ok(const char *host, const Ck *c)
{
	size_t hl = strlen(host), dl = strlen(c->dom);
	if (c->host_only)
		return !strcmp(host, c->dom);
	return hl >= dl && !strcmp(host + hl - dl, c->dom) && (hl == dl || host[hl - dl - 1] == '.');
}

static size_t ck_get(const char *url, char *out, size_t n)
{
	if (!CK_INIT || strncasecmp(url, "http", 4))
		return 0;
	char host[96], path[512];
	ck_host(url, host, sizeof host, path, sizeof path);
	size_t o = 0;
	out[0] = 0;
	LightLock_Lock(&CK_LOCK);
	for (int i = 0; i < NCK; i++)
		if (ck_domain_ok(host, &CK[i]) && !strncmp(path, CK[i].path, strlen(CK[i].path)))
			o += snprintf(out + o, o < n ? n - o : 0, "%s%s=%s", o ? "; " : "", CK[i].name, CK[i].val);
	LightLock_Unlock(&CK_LOCK);
	return o < n ? o : 0;
}

// Set-Cookie: nombre=valor; Domain=…; Path=…; Max-Age=0 o Expires pasado (se borra)
static void ck_set(const char *url, const char *sc)
{
	if (!CK_INIT)
		return;
	char host[96], path[512];
	ck_host(url, host, sizeof host, path, sizeof path);
	const char *eq = strchr(sc, '='), *semi = strchr(sc, ';');
	if (!eq || (semi && semi < eq))
		return;
	Ck c = {0};
	const char *nm = sc;
	while (*nm == ' ')
		nm++;
	snprintf(c.name, sizeof c.name, "%.*s", (int)(eq - nm), nm);
	size_t vl = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
	c.val = malloc(vl + 1);
	if (!c.val)
		return;
	memcpy(c.val, eq + 1, vl);
	c.val[vl] = 0;
	util_strlcpy(c.dom, host, sizeof c.dom);
	c.host_only = true;
	char *ls = strrchr(path, '/');
	snprintf(c.path, sizeof c.path, "%.*s", ls && ls != path ? (int)(ls - path) : 1, path);
	bool del = false;
	for (const char *a = semi; a && *a;) {
		a++;
		while (*a == ' ')
			a++;
		const char *e = a + strcspn(a, ";");
		if (!strncasecmp(a, "domain=", 7)) {
			const char *d = a + 7;
			while (*d == '.')
				d++;
			char dm[96];
			snprintf(dm, sizeof dm, "%.*s", (int)(e - d), d);
			for (char *p = dm; *p; p++)
				*p = (char)tolower((unsigned char)*p);
			size_t hl = strlen(host), dl = strlen(dm);
			if (dl && hl >= dl && !strcmp(host + hl - dl, dm)) // solo su dominio o uno de arriba
				util_strlcpy(c.dom, dm, sizeof c.dom), c.host_only = false;
		} else if (!strncasecmp(a, "path=", 5))
			snprintf(c.path, sizeof c.path, "%.*s", (int)(e - a - 5), a + 5);
		else if (!strncasecmp(a, "max-age=", 8) && atoi(a + 8) <= 0)
			del = true;
		else if (!strncasecmp(a, "expires=", 8) && (strstr(a, "1970") || strstr(a, "Thu, 01 Jan")))
			del = true;
		a = e;
	}
	LightLock_Lock(&CK_LOCK);
	for (int i = 0; i < NCK; i++)
		if (!strcmp(CK[i].name, c.name) && !strcmp(CK[i].dom, c.dom) && !strcmp(CK[i].path, c.path)) {
			free(CK[i].val);
			CK[i] = CK[--NCK];
			break;
		}
	if (!del && NCK < 400) {
		if (NCK == CCK) {
			int nc = CCK ? CCK * 2 : 32;
			Ck *nk = realloc(CK, nc * sizeof *nk);
			if (nk)
				CK = nk, CCK = nc;
		}
		if (NCK < CCK) {
			CK[NCK++] = c;
			c.val = NULL;
		}
	}
	LightLock_Unlock(&CK_LOCK);
	free(c.val);
}

/* memoria libre para malloc (aproximada: sin contar la fragmentación) */
static u32 mem_free(void)
{
	struct mallinfo mi = mallinfo();
	u32 hs = envGetHeapSize(), used = (u32)mi.uordblks;
	return hs > used ? hs - used : 0;
}

static bool is_google(const char *url)
{
	char host[128], path[8];
	ck_host(url, host, sizeof host, path, sizeof path);
	return strstr(host, "google.") != NULL;
}

static void browser_thread_net(void) // cada hilo del navegador: su User-Agent y las cookies
{
	if (!CK_INIT) {
		LightLock_Init(&CK_LOCK);
		CK_INIT = true;
	}
	dl_set_thread_ua(UA_PC);
	dl_set_thread_cookies(ck_get, ck_set);
}

/* ------------------------------------------------------------ JavaScript de la página */
static void js_log(const char *m)
{
	util_log("%s", m);
}

/* anuncios y rastreo: ni se bajan ni se ejecutan (en una 3DS, solo gastan segundos y memoria) */
static bool blocked_host(const char *url)
{
	static const char *const BL[] = {"googlesyndication.com", "doubleclick.net", "googletagmanager.com", "google-analytics.com", "googleadservices.com",
	                                 "adservice.google.", "cloudflareinsights.com", "facebook.net", "connect.facebook", "scorecardresearch.com", "quantserve.com",
	                                 "amazon-adsystem.com", "adnxs.com", "criteo.", "taboola.com", "outbrain.com", "hotjar.com", "clarity.ms",
	                                 "newrelic.com", "nr-data.net", "segment.io", "segment.com", "mixpanel.com", "pubmatic.com", "rubiconproject.com",
	                                 "moatads.com", "chartbeat.", "optimizely.com", "adsafeprotected.com", "2mdn.net", "yieldmo.com", NULL};
	const char *h = strstr(url, "://");
	if (!h)
		return false;
	h += 3;
	size_t hl = strcspn(h, "/?#:");
	for (int i = 0; BL[i]; i++) {
		size_t l = strlen(BL[i]);
		for (size_t k = 0; k + l <= hl; k++)
			if (!strncasecmp(h + k, BL[i], l))
				return true;
	}
	return false;
}

static int js_fetch(const char *url, const char *method, const char *body, size_t blen, char **out, size_t *n, long *code, char *fin, size_t fl)
{
	if (blocked_host(url)) {
		util_log("web: bloqueado %.80s", url);
		return -1;
	}
	Buf b = {0};
	int r;
	*code = 0;
	if (!strncasecmp(url, "file:", 5)) { // de una página de la SD: es un archivo de la SD
		url += 5;
		while (url[0] == '/' && url[1] == '/')
			url++;
	}
	if (strcasecmp(method, "GET") && strstr(url, "://")) { // POST y demás
		static const char *const H[] = {"Content-Type: application/x-www-form-urlencoded; charset=UTF-8", NULL};
		r = dl_request(strcasecmp(method, "PUT") ? HTTPC_METHOD_POST : HTTPC_METHOD_PUT, url, H, body, blen, buf_cb, &b, code);
		util_strlcpy(fin, url, fl);
	} else {
		r = fetch_any(url, "*/*", &b, code);
		util_strlcpy(fin, strstr(url, "://") && dl_last_url()[0] ? dl_last_url() : url, fl);
	}
	if (r != DLF_OK) {
		free(b.p);
		return -1;
	}
	*out = b.p, *n = b.n;
	return 0;
}

// cuántos nodos tiene el árbol (para enseñar el progreso al colocar la página)
static int count_nodes(dom_node *n)
{
	int k = 0;
	dom_node *c = NULL;
	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return 0;
	while (c) {
		k += 1 + count_nodes(c);
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
	return k;
}

static void load_thread(void *arg)
{
	(void)arg;
	browser_thread_net();
	Buf b = {0};
	if (!strncasecmp(B.url, "sdmc:", 5) || B.url[0] == '/') {
		char sp[1100];
		snprintf(sp, sizeof sp, "%s%s", B.url[0] == '/' ? "sdmc:" : "", B.url);
		FILE *fp = fopen(sp, "rb");
		if (!fp) {
			snprintf(B.err, sizeof B.err, "No se pudo abrir el archivo");
			B.st = ST_ERROR;
			return;
		}
		char tmp[16384];
		size_t n;
		while ((n = fread(tmp, 1, sizeof tmp, fp)) > 0 && buf_cb(tmp, 1, n, &b) == n)
			;
		fclose(fp);
	} else {
		long code = 0;
		int r = fetch_any(B.url, "text/html,application/xhtml+xml,*/*;q=0.8", &b, &code);
		B.http_code = (int)code;
		if (r != DLF_OK || code >= 400) {
			if (r == DLF_OK)
				snprintf(B.err, sizeof B.err, "El servidor respondió %ld", code);
			else
				snprintf(B.err, sizeof B.err, r == DLF_NOCONN ? "No se pudo conectar (o comprobar el certificado)" : "Se cortó la descarga");
			free(b.p);
			B.st = ST_ERROR;
			return;
		}
		if (dl_last_url()[0] && strcmp(dl_last_url(), B.url)) // tras redirigir: los enlaces son relativos a la final
			util_strlcpy(B.url, dl_last_url(), sizeof B.url);
	}
	B.bytes = b.n;
	B.oom = OOM_NONE;
	bool reader = !strcmp(B.reader_url, B.url);
	// ¿cabe? el árbol ocupa unas 12 veces el HTML: si ni eso cabe, ni se intenta (el aviso)
	if (!reader && (u64)b.n * MEM_PER_HTML + MEM_RESERVE > mem_free()) { // en modo lectura: lo que quepa
		B.oom_need = (u32)((u64)b.n * MEM_PER_HTML > 0xFFFFFFFFu ? 0xFFFFFFFFu : b.n * MEM_PER_HTML);
		util_log("web: no cabe: %u KB de HTML, %u KB libres", (unsigned)(b.n / 1024), (unsigned)(mem_free() / 1024));
		free(b.p);
		B.st = ST_OOM;
		return;
	}
	// ¿pide ancho de móvil? (<meta name="viewport" content="width=device-width…">), en los primeros 64 KB
	B.page_mobile = false;
	if (b.p) {
		size_t lim = b.n < 65536 ? b.n : 65536;
		for (size_t i = 0; i + 8 < lim && !B.page_mobile; i++)
			if (!strncasecmp(b.p + i, "viewport", 8))
				for (size_t k = i; k < lim && k < i + 300; k++)
					if (!strncasecmp(b.p + k, "device-width", 12)) {
						B.page_mobile = true;
						break;
					}
	}
	// muchas páginas (Wikipedia) dicen «UTF-8» demasiado abajo para que hubbub lo vea: si el texto es
	// UTF-8 válido, se le dice; si no, que lo detecte él
	dom_hubbub_parser_params params = {.enc = utf8_ok(b.p, b.n) ? "UTF-8" : NULL, .fix_enc = true, .enable_script = false};
	dom_hubbub_parser *parser = NULL;
	dom_document *doc = NULL;
	if (dom_hubbub_parser_create(&params, &parser, &doc) != DOM_HUBBUB_OK || !parser) {
		free(b.p);
		snprintf(B.err, sizeof B.err, "Sin memoria para leer la página");
		B.st = ST_ERROR;
		return;
	}
	// por trozos, mirando la memoria: si se acaba, se queda con lo leído hasta ahí
	for (size_t off = 0; off < b.n || !b.n; off += 32768) {
		size_t k = b.n - off < 32768 ? b.n - off : 32768;
		dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)(b.p ? b.p + off : ""), k);
		if (!b.n)
			break;
		if (off + k < b.n && mem_free() < 2 * MEM_RESERVE) {
			B.oom = OOM_PARSE;
			B.oom_need = (u32)(b.n * MEM_PER_HTML);
			util_log("web: memoria llena leyendo la página (%u de %u KB)", (unsigned)((off + k) / 1024), (unsigned)(b.n / 1024));
			reader = true; // lo leído se ve sin JavaScript ni CSS (no queda memoria para ellos)
			break;
		}
	}
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	free(b.p);
	// su JavaScript, antes del CSS: lo que añada o cambie (también hojas de estilo) ya cuenta
	B.js_nav[0] = 0;
	// el JavaScript, con lo que quede (dejando sitio para colocar la página)
	u32 fr = mem_free(), lay = (u32)(b.n * 4) + MEM_RESERVE;
	size_t js_lim = fr > lay + 2u * 1024 * 1024 ? fr - lay : 0;
	if (js_lim > 24u * 1024 * 1024)
		js_lim = 24u * 1024 * 1024;
	if (doc && B.js && !reader && !B.light && !js_lim && B.oom == OOM_NONE)
		B.oom = OOM_JS, B.oom_need = lay + 2u * 1024 * 1024;
	if (doc && B.js && !reader && !B.light && js_lim) { // en modo ligero, sin JavaScript
		JsHost h = {.mem_limit = js_lim, .fetch = js_fetch, .log = js_log, .url = B.url, .ua = UA_PC, .width = VW_PC, .budget_ms = 20000, .cancel = &B.cancel,
		            .cookie_get = ck_get, .cookie_set = ck_set};
		B.in_js = true;
		JsResult jr;
		B.jsp = jsdom_open(doc, &h, &jr);
		B.in_js = false;
		B.js_scripts = jr.scripts, B.js_errors = jr.errors;
		util_log("web: JavaScript: %d scripts, %d con error, %u ms", jr.scripts, jr.errors, jr.ms);
		if (jr.navigate[0])
			util_strlcpy(B.js_nav, jr.navigate, sizeof B.js_nav);
		if (jr.oom) { // sin memoria: se suelta su JavaScript (la página se ve tal como quedó)
			util_log("web: el JavaScript se quedó sin memoria (%u KB)", (unsigned)(jr.mem / 1024));
			if (B.oom == OOM_NONE)
				B.oom = OOM_JS, B.oom_need = (u32)(jr.mem * 2);
			jsdom_close(B.jsp);
			B.jsp = NULL;
		}
	}
	// el CSS de la página (se baja y se lee aquí, en el hilo, para no parar la pantalla)
	BrCss *css = doc && !reader ? brcss_new(B.url) : NULL;
	if (css) {
		int links = 0;
		size_t bytes = 0;
		CSS_NSEEN = 0;
		css_collect(css, (dom_node *)doc, 0, &links, &bytes);
		brcss_finish(css, doc, VW_PC); // su CSS moderno, traducido para libcss
		B.ncss = brcss_sheets(css);
		util_log("web: %d hojas CSS, %u bytes", B.ncss, (unsigned)bytes);
	}
	B.css = css;
	B.doc = doc;
	B.nel_total = doc ? count_nodes((dom_node *)doc) : 0;
	B.st = ST_PARSED;
}

static void start_load(const char *url)
{
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp); // antes que el documento: tiene sus nodos
	B.jsp = NULL;
	if (B.doc) {
		dom_node_unref(B.doc);
		B.doc = NULL;
	}
	brcss_free(B.css);
	B.css = NULL;
	B.ncss = 0;
	img_free_all();
	fv_free();
	util_strlcpy(B.url, url, sizeof B.url);
	B.click_link = -1;
	B.click_wait = false;
	B.cancel = false;
	B.err[0] = 0;
	B.title[0] = 0;
	B.st = ST_LOADING;
	B.t_load = 0;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thr = threadCreate(load_thread, NULL, 512 * 1024, prio + 1, br_core(), false); // hubbub y libdom anidan mucho en páginas grandes
	if (!B.thr) {
		snprintf(B.err, sizeof B.err, "No se pudo empezar a cargar");
		B.st = ST_ERROR;
	}
}

/* ------------------------------------------------------------ enlaces relativos */
static void resolve(const char *base, const char *href, char *out, size_t n)
{
	while (*href == ' ' || *href == '\n' || *href == '\t')
		href++;
	if (href[0] == '#') { // la misma página, otra parte
		snprintf(out, n, "%.*s%s", (int)strcspn(base, "#"), base, href);
		return;
	}
	if (href[0] == '?') { // la misma página con otra consulta
		snprintf(out, n, "%.*s%s", (int)strcspn(base, "?#"), base, href);
		return;
	}
	if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8)) {
		util_strlcpy(out, href, n);
		return;
	}
	const char *sch = strstr(base, "://");
	if (!sch && href[0] == '/' && href[1] == '/') { // //servidor/ruta desde un archivo de la SD
		snprintf(out, n, "https:%s", href);
		return;
	}
	if (!sch) { // archivo de la SD: relativo a su carpeta
		char d[1024];
		util_strlcpy(d, base, sizeof d);
		char *sl = strrchr(d, '/');
		if (href[0] == '/')
			util_strlcpy(out, href, n);
		else {
			if (sl)
				sl[1] = 0;
			snprintf(out, n, "%s%s", d, href);
		}
		return;
	}
	if (href[0] == '/' && href[1] == '/') { // //servidor/ruta
		snprintf(out, n, "%.*s:%s", (int)(sch - base), base, href);
		return;
	}
	const char *hostend = strchr(sch + 3, '/');
	size_t hl = hostend ? (size_t)(hostend - base) : strlen(base);
	if (href[0] == '/') {
		snprintf(out, n, "%.*s%s", (int)hl, base, href);
		return;
	}
	// relativo a la carpeta de la página (sin ?consulta ni #)
	char d[1024];
	snprintf(d, sizeof d, "%s", base);
	d[strcspn(d, "?#")] = 0;
	char *sl = strrchr(d + hl, '/');
	if (sl)
		sl[1] = 0;
	else
		strcat(d, "/");
	while (!strncmp(href, "./", 2))
		href += 2;
	while (!strncmp(href, "../", 3)) {
		href += 3;
		size_t l = strlen(d);
		if (l > hl + 1) {
			d[l - 1] = 0;
			char *p = strrchr(d + hl, '/');
			if (p)
				p[1] = 0;
		}
	}
	snprintf(out, n, "%s%s", d, href);
}

/* ------------------------------------------------------------ maquetar */
typedef struct {
	float sc, indent, right; /* tamaño de letra, sangría y borde derecho (px de página) */
	u32 col;
	int link;
	bool pre, under, skip, bold, hidden;
	bool fixw;     /* el ancho ya lo decidió quien lo coloca (columna, flotante): no se aplica otra vez */
	u8 align;
	bool list;     /* <li> con viñeta (list-style no es none) */
	int *olnum;    /* contador de la <ol> de dentro */
	int *cells;    /* celdas de la fila (separadores) */
	const css_computed_style *cs; /* el estilo calculado del padre (con CSS) */
	dom_node *el;  /* el elemento que se está colocando */
	int clk;       /* dentro de algo que se toca por JavaScript (-1 no) */
} St;

static float L_x;          /* posición en la línea actual */
static bool L_has, L_space;
static float L_gap;        /* hueco antes de la próxima línea */
static float L_indent;
static float L_right = 1e9; /* borde derecho de la línea (con CSS: márgenes, anchos) */
static u8 L_align;
static float L_pb;          /* donde acaba lo de antes de la línea en curso (su y = L_pb + su hueco) */
static bool L_marker;      /* en la línea solo está la viñeta de un <li> */
#define RIGHT (L_right < PAGE_W ? L_right : PAGE_W)
/* flotantes (float: left/right): zonas ocupadas; las líneas que pasan a su altura se estrechan */
typedef struct {
	float x0, x1, y0, y1;
	u8 side; /* 1 izquierda, 2 derecha */
} Fl;
static Fl FL[64];
static int NFL;

// los bordes de una línea en y, descontando los flotantes que haya a esa altura
static void bounds_at(float y, float h, float *l, float *r)
{
	float lo = L_indent, ro = RIGHT;
	*l = lo, *r = ro;
	for (int i = 0; i < NFL; i++) {
		const Fl *f = &FL[i];
		if (y >= f->y1 || y + h <= f->y0 || f->x1 <= lo - 1 || f->x0 >= ro + 1)
			continue;
		if (f->side == 1)
			*l = fmaxf(*l, f->x1 + 8);
		else
			*r = fminf(*r, f->x0 - 8);
	}
	if (*r - *l < 40) // no cabe nada al lado: se usa todo (mejor encima que ilegible)
		*l = lo, *r = ro;
}

static void line_bounds(Line *l)
{
	bounds_at(l->y, text_lineh(SC_TEXT), &l->left, &l->right);
}
#define CUR_R (B.nl ? B.l[B.nl - 1].right : RIGHT)
#define CUR_L (B.nl ? B.l[B.nl - 1].left : L_indent)

static Line *line_new(void)
{
	if (B.nl == B.cl) {
		int nc = B.cl ? B.cl * 2 : 256;
		Line *nl = realloc(B.l, nc * sizeof *nl);
		if (!nl)
			return NULL;
		B.l = nl, B.cl = nc;
	}
	if (B.nl) {
		Line *c = &B.l[B.nl - 1];
		if (!c->used && !c->hr) { // la en curso está vacía: se aprovecha (su hueco, el mayor)
			c->gap = fmaxf(c->gap, L_gap);
			c->y = L_pb + c->gap;
			c->align = L_align, c->left = L_indent, c->right = RIGHT;
			line_bounds(c);
			L_gap = 0;
			L_x = c->left;
			L_has = false;
			L_space = false;
			L_marker = false;
			return c;
		}
		L_pb = c->y + c->h; // se cierra la en curso
	}
	Line *l = &B.l[B.nl++];
	memset(l, 0, sizeof *l);
	l->h = text_lineh(SC_TEXT);
	l->gap = L_gap;
	l->y = L_pb + L_gap;
	l->align = L_align;
	l->left = L_indent;
	l->right = RIGHT;
	line_bounds(l);
	L_gap = 0;
	L_x = l->left;
	L_has = false;
	L_space = false;
	L_marker = false;
	return l;
}

static void block(float gap)
{
	if (L_marker) // el primer bloque de un <li> va en la línea de su viñeta
		return;
	if (L_has)
		line_new();
	if (B.nl && !L_has && !B.l[B.nl - 1].hr) { // la línea actual está vacía: el hueco va delante de ella
		if (gap > B.l[B.nl - 1].gap) {
			B.l[B.nl - 1].gap = gap;
			B.l[B.nl - 1].y = L_pb + gap;
		}
	} else if (gap > L_gap)
		L_gap = gap;
}

static void put(const char *s, int n, const St *st, float w)
{
	if (!B.nl || (!L_has && B.l[B.nl - 1].hr))
		line_new();
	if (B.nf == B.cf) {
		int nc = B.cf ? B.cf * 2 : 1024;
		Frag *nf = realloc(B.f, nc * sizeof *nf);
		if (!nf)
			return;
		B.f = nf, B.cf = nc;
	}
	if (B.nt + n + 1 > B.ct) {
		u32 nc = B.ct ? B.ct * 2 : 64 * 1024;
		while (nc < B.nt + n + 1)
			nc *= 2;
		char *nt = realloc(B.text, nc);
		if (!nt)
			return;
		B.text = nt, B.ct = nc;
	}
	memcpy(B.text + B.nt, s, n);
	Frag *f = &B.f[B.nf++];
	f->line = B.nl - 1;
	f->x = L_x, f->w = w;
	f->off = B.nt, f->n = (u16)n;
	f->sc = st->sc, f->col = st->col, f->link = (s16)st->link, f->under = st->under, f->bold = st->bold;
	f->img = -1;
	f->wdg = -1;
	f->node = st->el;
	f->clk = (s16)st->clk;
	B.nt += n;
	Line *l = &B.l[B.nl - 1];
	l->used = true;
	float lh = text_lineh(st->sc);
	if (lh > l->h)
		l->h = lh;
	L_x += w;
	L_has = true;
}

static void word(const char *s, int n, const St *st)
{
	if (n <= 0)
		return;
	float w = text_width_n(s, n, st->sc), sp = 0;
	if (L_has && L_space)
		sp = text_width(" ", st->sc);
	if (!B.nl || (L_has && L_x + sp + w > CUR_R)) {
		line_new();
		sp = 0;
	}
	if (!L_has)
		L_x = CUR_L;
	// una palabra más ancha que la pantalla: a trozos
	while (w > CUR_R - CUR_L && n > 1) {
		int k = n;
		while (k > 1 && text_width_n(s, k, st->sc) > CUR_R - L_x)
			k--;
		while (k > 1 && (s[k] & 0xC0) == 0x80) // sin partir un carácter UTF-8
			k--;
		put(s, k, st, text_width_n(s, k, st->sc));
		line_new();
		s += k, n -= k;
		w = text_width_n(s, n, st->sc);
	}
	L_x += sp;
	put(s, n, st, w);
	L_space = false;
	L_marker = false;
}

// una imagen en la línea: un trozo sin texto de w × h
static void put_box(float w, float h, const St *st, int img)
{
	if (!B.nl)
		line_new();
	float sp = L_has && L_space ? text_width(" ", st->sc) : 0;
	if (L_has && L_x + sp + w > CUR_R)
		line_new(), sp = 0;
	if (!L_has)
		L_x = CUR_L;
	L_x += sp;
	int nf0 = B.nf;
	put("", 0, st, w);
	if (B.nf == nf0)
		return;
	Frag *f = &B.f[B.nf - 1];
	f->img = (s16)img;
	f->under = 0;
	Line *l = &B.l[f->line];
	if (h > l->h)
		l->h = h;
	L_space = false;
	L_marker = false;
}

static void text_run(const char *s, size_t n, const St *st)
{
	if (st->hidden) // visibility: hidden
		return;
	size_t i = 0;
	if (st->pre) { // se respetan los espacios y los saltos de línea
		while (i < n) {
			size_t j = i;
			while (j < n && s[j] != '\n')
				j++;
			if (j > i)
				word(s + i, (int)(j - i), st);
			if (j < n) {
				line_new();
				L_has = false;
			}
			i = j + 1;
		}
		return;
	}
	while (i < n) {
		unsigned char c = s[i];
		if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f') {
			L_space = true;
			i++;
			continue;
		}
		size_t j = i;
		while (j < n && s[j] != ' ' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r' && s[j] != '\f')
			j++;
		word(s + i, (int)(j - i), st);
		i = j;
	}
}

static bool attr(dom_node *el, const char *name, char *out, size_t n)
{
	dom_string *k = NULL, *v = NULL;
	out[0] = 0;
	if (dom_string_create((const uint8_t *)name, strlen(name), &k) != DOM_NO_ERR)
		return false;
	bool ok = dom_element_get_attribute((dom_element *)el, k, &v) == DOM_NO_ERR && v;
	if (ok) {
		size_t l = dom_string_byte_length(v);
		snprintf(out, n, "%.*s", (int)(l < n - 1 ? l : n - 1), dom_string_data(v));
		dom_string_unref(v);
	}
	dom_string_unref(k);
	return ok;
}

static int link_add(const char *href)
{
	if (B.nlinks == B.clinks) {
		int nc = B.clinks ? B.clinks * 2 : 64;
		char **nl = realloc(B.links, nc * sizeof *nl);
		if (!nl)
			return -1;
		B.links = nl, B.clinks = nc;
	}
	char abs[1024];
	resolve(B.url, href, abs, sizeof abs);
	B.links[B.nlinks] = strdup(abs);
	return B.links[B.nlinks] ? B.nlinks++ : -1;
}

static bool is(const char *t, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(t, *list))
			return true;
	return false;
}

static void walk(dom_node *node, St st);

/* modo lectura sin CSS: lo que casi siempre son menús, desplegables o avisos de edición */
static bool skip_class(const char *cls)
{
	static const char *const BAD[] = {"dropdown", "menu", "navbox", "noprint", "sidebar", "editsection", "jump-link",
	                                  "printfooter", "catlinks", "mw-indicator", "cookie", "banner", NULL};
	char low[256];
	size_t i = 0;
	for (; cls[i] && i < sizeof low - 1; i++)
		low[i] = (char)tolower((unsigned char)cls[i]);
	low[i] = 0;
	for (const char *const *b = BAD; *b; b++)
		if (strstr(low, *b))
			return true;
	return false;
}

static void children(dom_node *node, const St *st)
{
	dom_node *c = NULL;
	if (dom_node_get_first_child(node, &c) != DOM_NO_ERR)
		return;
	while (c) {
		walk(c, *st);
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
}

/* ------------------------------------------------------------ imágenes */
#define IMG_MAX     400                 /* imágenes por página */
#define IMG_BUDGET  (5 * 1024 * 1024)   /* píxeles decodificados en total (~20 MB) */

static int img_find_or_add(const char *url)
{
	for (int i = 0; i < B.nimg; i++)
		if (!strcmp(B.img[i].url, url))
			return i;
	if (B.nimg >= IMG_MAX)
		return -1;
	if (B.nimg == B.cimg) {
		int nc = B.cimg ? B.cimg * 2 : 32;
		PImg *ni = realloc(B.img, nc * sizeof *ni);
		if (!ni)
			return -1;
		B.img = ni, B.cimg = nc;
	}
	PImg *im = &B.img[B.nimg];
	memset(im, 0, sizeof *im);
	im->url = strdup(url);
	if (!im->url)
		return -1;
	im->st = IM_WAIT;
	return B.nimg++;
}

static void img_stop(void)
{
	if (B.img_thr) {
		B.img_quit = true;
		threadJoin(B.img_thr, U64_MAX);
		threadFree(B.img_thr);
		B.img_thr = NULL;
	}
	B.img_quit = false;
}

static void img_free_all(void)
{
	img_stop();
	for (int i = 0; i < B.nimg; i++) {
		free(B.img[i].url);
		free(B.img[i].rgba);
		tex_free(&B.img[i].tex);
	}
	free(B.img);
	B.img = NULL;
	B.nimg = B.cimg = 0;
	B.img_px = 0;
}

static int fetch_any(const char *url, const char *accept, Buf *b, long *code);

// baja y decodifica, primero las que están más cerca de lo que se ve
static void img_thread(void *arg)
{
	(void)arg;
	browser_thread_net();
	for (;;) {
		if (B.img_quit)
			return;
		int best = -1;
		float bd = 1e30f, vy = B.sy;
		for (int i = 0; i < B.nimg; i++)
			if (B.img[i].st == IM_WAIT) {
				float d = fabsf(B.img[i].y - vy);
				if (d < bd)
					bd = d, best = i;
			}
		if (best < 0)
			return;
		PImg *im = &B.img[best];
		Buf b = {0};
		long code = 0;
		int r = blocked_host(im->url) ? DLF_FAIL : fetch_any(im->url, "image/png,image/jpeg,image/gif,image/*;q=0.5", &b, &code);
		int mw = (int)fminf(512, fmaxf(16, im->w * 1.5f)), mh = (int)fminf(512, fmaxf(16, im->h * 1.5f));
		u8 *px = NULL;
		int w = 0, h = 0;
		bool svg = false;
		for (size_t i = 0; b.p && i < b.n && i < 512 && !svg; i++) // por su contenido, no por su nombre
			svg = !strncmp(b.p + i, "<svg", 4);
		bool ok = r == DLF_OK && code < 400 && b.n && B.img_px < IMG_BUDGET &&
		          mem_free() > 2 * MEM_RESERVE + (u32)b.n * 2 + (u32)(mw * mh * 4) &&
		          (svg ? svg_render(b.p, b.n, mw, mh, &px, &w, &h) : img_mem_fit((const u8 *)b.p, b.n, mw, mh, &px, &w, &h) == IMG_OK);
		if (ok) {
			im->rgba = px, im->tw = w, im->th = h, im->iw = w, im->ih = h;
			B.img_px += (size_t)w * h;
			im->st = IM_RGBA;
		} else
			im->st = IM_FAIL;
		free(b.p);
	}
}

static void img_start(void)
{
	img_stop();
	if (!strcmp(B.reader_url, B.url) || B.light) // modo lectura o ligero: sin imágenes
		return;
	bool any = false;
	for (int i = 0; i < B.nimg; i++)
		any |= B.img[i].st == IM_WAIT;
	if (!any)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.img_thr = threadCreate(img_thread, NULL, 64 * 1024, prio + 2, br_core(), false);
}

/* ------------------------------------------------------------ con CSS */
static float SC16 = SC_TEXT; /* tamaño de letra de 16 px */

static u32 css_col(css_color c)
{
	return RGBA((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
}

// una longitud en px de página (los % sobre «base»)
static float len_px(const css_computed_style *cs, css_fixed len, css_unit u, float base)
{
	if (u == CSS_UNIT_PCT)
		return base * FIXTOFLT(len) / 100;
	return brcss_px(cs, len, u, B.vw);
}

// escondido «para lectores de pantalla» o de otras formas que un navegador no enseña:
// position:absolute con clip o de 1 px, letra de tamaño 0, sangría enorme hacia fuera
static bool css_invisible(const css_computed_style *cs)
{
	css_fixed v;
	css_unit u;
	if (css_computed_position(cs) == CSS_POSITION_ABSOLUTE) {
		css_computed_clip_rect r;
		if (css_computed_clip(cs, &r) == CSS_CLIP_RECT)
			return true;
		if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) <= 2)
			return true;
		if (css_computed_height(cs, &v, &u) == CSS_HEIGHT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) <= 2)
			return true;
	}
	css_computed_font_size(cs, &v, &u);
	if (brcss_px(cs, v, u, B.vw) < 1)
		return true;
	css_fixed op;
	if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET && FIXTOFLT(op) < 0.05f) // transparente del todo
		return true;
	if (css_computed_text_indent(cs, &v, &u) == CSS_TEXT_INDENT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) < -500)
		return true;
	return false;
}

static bool is_block_display(uint8_t d)
{
	return d != CSS_DISPLAY_INLINE && d != CSS_DISPLAY_INLINE_BLOCK && d != CSS_DISPLAY_INLINE_FLEX && d != CSS_DISPLAY_INLINE_GRID &&
	       d != CSS_DISPLAY_INLINE_TABLE && d != CSS_DISPLAY_TABLE_CELL;
}

// la línea vacía en curso toma la sangría, el borde y la alineación de ahora
static void line_ctx(void)
{
	if (B.nl && !L_has) {
		Line *l = &B.l[B.nl - 1];
		l->align = L_align, l->left = L_indent, l->right = RIGHT;
		line_bounds(l);
		L_x = l->left;
	}
}

static void bg_add(int l0, int l1, float x0, float x1, u32 col)
{
	if (B.nbg == B.cbg) {
		int nc = B.cbg ? B.cbg * 2 : 64;
		BgBox *nb = realloc(B.bg, nc * sizeof *nb);
		if (!nb)
			return;
		B.bg = nb, B.cbg = nc;
	}
	B.bg[B.nbg++] = (BgBox){B.l[l0].y - 4, B.l[l1].y + B.l[l1].h + 4, x0, x1, col, {0}, {0}};
}

static void children(dom_node *node, const St *st);
static int link_add(const char *href);
static bool attr(dom_node *el, const char *name, char *out, size_t n);

/* ------------------------------------------------------------ formularios */
#define NI(n) ((dom_node_internal *)(n))

static void tag_of(dom_node *n, char *tag, size_t sz)
{
	tag[0] = 0;
	dom_string *nm = NULL;
	if (dom_node_get_node_name(n, &nm) != DOM_NO_ERR || !nm)
		return;
	size_t l = dom_string_byte_length(nm), i = 0;
	for (; i < l && i < sz - 1; i++)
		tag[i] = (char)tolower((unsigned char)dom_string_data(nm)[i]);
	tag[i] = 0;
	dom_string_unref(nm);
}


static FVal *fv_get(dom_node *n, bool make)
{
	for (int i = 0; i < B.nfv; i++)
		if (B.fv[i].node == n)
			return &B.fv[i];
	if (!make)
		return NULL;
	if (B.nfv == B.cfv) {
		int nc = B.cfv ? B.cfv * 2 : 16;
		FVal *nv = realloc(B.fv, nc * sizeof *nv);
		if (!nv)
			return NULL;
		B.fv = nv, B.cfv = nc;
	}
	FVal *v = &B.fv[B.nfv++];
	v->node = n, v->value = NULL, v->checked = -1;
	return v;
}

static void fv_free(void)
{
	for (int i = 0; i < B.nfv; i++)
		free(B.fv[i].value);
	free(B.fv);
	B.fv = NULL;
	B.nfv = B.cfv = 0;
}

static void text_of(dom_node *n, char *out, size_t sz) // su texto, con los espacios juntos
{
	out[0] = 0;
	dom_string *t = NULL;
	if (dom_node_get_text_content(n, &t) != DOM_NO_ERR || !t)
		return;
	const char *p = dom_string_data(t);
	size_t l = dom_string_byte_length(t), k = 0;
	bool sp = true;
	for (size_t i = 0; i < l && k < sz - 1; i++) {
		bool w = isspace((unsigned char)p[i]);
		if (w && !sp)
			out[k++] = ' ';
		else if (!w)
			out[k++] = p[i];
		sp = w;
	}
	while (k && out[k - 1] == ' ')
		k--;
	out[k] = 0;
	dom_string_unref(t);
}

static bool has_attr(dom_node *n, const char *a)
{
	char v[8];
	return attr(n, a, v, sizeof v);
}

// la opción elegida de un <select>: su texto y su valor (la marcada, o la primera)
static void select_pick(dom_node *sel, int want, char *label, size_t ls, char *value, size_t vs, int *count)
{
	int k = 0, chosen = -1;
	label[0] = value[0] = 0;
	for (int pass = 0; pass < 2 && chosen < 0; pass++) {
		k = 0;
		for (dom_node_internal *c = NI(sel)->first_child; c; c = c->next) {
			dom_node_internal *opts[2] = {c, NULL};
			if (c->type != DOM_ELEMENT_NODE)
				continue;
			char tag[12];
			tag_of((dom_node *)c, tag, sizeof tag);
			if (!strcmp(tag, "optgroup"))
				opts[0] = c->first_child;
			for (dom_node_internal *o = opts[0]; o; o = !strcmp(tag, "optgroup") ? o->next : NULL) {
				if (o->type != DOM_ELEMENT_NODE)
					continue;
				char ot[12];
				tag_of((dom_node *)o, ot, sizeof ot);
				if (strcmp(ot, "option"))
					continue;
				bool pick = want >= 0 ? k == want : pass == 0 ? has_attr((dom_node *)o, "selected") : k == 0;
				if (pick && chosen < 0) {
					chosen = k;
					text_of((dom_node *)o, label, ls);
					if (!attr((dom_node *)o, "value", value, vs))
						util_strlcpy(value, label, vs);
				}
				k++;
			}
		}
		if (want >= 0)
			break;
	}
	if (count)
		*count = k;
}

static void url_esc(const char *in, char *out, size_t n)
{
	static const char hx[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *in && o + 4 < n; in++) {
		unsigned char c = *in;
		if (isalnum(c) || strchr("-_.~", c))
			out[o++] = c;
		else
			out[o++] = '%', out[o++] = hx[c >> 4], out[o++] = hx[c & 15];
	}
	out[o] = 0;
}

static void go(const char *url, bool push);

// enviar un formulario (GET): los campos con nombre, como haría un navegador
static void form_submit(dom_node *form, dom_node *submitter)
{
	char *q = calloc(1, 8192), act[1024], tmp[1024], esc[2048];
	if (!q)
		return;
	size_t ql = 0;
	// recorrido de todo el formulario
	dom_node_internal *stack[64];
	int sp = 0;
	for (dom_node_internal *c = NI(form)->first_child; c || sp;) {
		if (!c) {
			c = stack[--sp]->next;
			continue;
		}
		if (c->type == DOM_ELEMENT_NODE) {
			char tag[12], name[256], type[24], val[1024];
			tag_of((dom_node *)c, tag, sizeof tag);
			bool field = !strcmp(tag, "input") || !strcmp(tag, "select") || !strcmp(tag, "textarea") || !strcmp(tag, "button");
			if (field && attr((dom_node *)c, "name", name, sizeof name) && name[0] && !has_attr((dom_node *)c, "disabled")) {
				bool add = true;
				val[0] = 0;
				if (!attr((dom_node *)c, "type", type, sizeof type))
					util_strlcpy(type, !strcmp(tag, "button") ? "submit" : "text", sizeof type);
				for (char *p = type; *p; p++)
					*p = (char)tolower((unsigned char)*p);
				FVal *v = fv_get((dom_node *)c, false);
				if (!strcmp(tag, "select")) {
					char lab[2];
					select_pick((dom_node *)c, v && v->value ? atoi(v->value) : -1, lab, sizeof lab, val, sizeof val, NULL);
				} else if (!strcmp(tag, "textarea")) {
					if (v && v->value)
						util_strlcpy(val, v->value, sizeof val);
					else
						text_of((dom_node *)c, val, sizeof val);
				} else if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
					bool on = v && v->checked >= 0 ? v->checked : has_attr((dom_node *)c, "checked");
					add = on;
					if (!attr((dom_node *)c, "value", val, sizeof val))
						util_strlcpy(val, "on", sizeof val);
				} else if (!strcmp(type, "submit") || !strcmp(type, "image") || !strcmp(type, "button") || !strcmp(type, "reset")) {
					add = (dom_node *)c == submitter && strcmp(type, "reset");
					attr((dom_node *)c, "value", val, sizeof val);
				} else if (!strcmp(type, "file"))
					add = false;
				else if (v && v->value)
					util_strlcpy(val, v->value, sizeof val);
				else
					attr((dom_node *)c, "value", val, sizeof val);
				if (add) {
					url_esc(name, esc, sizeof esc);
					ql += snprintf(q + ql, 8192 - ql, "%s%s=", ql ? "&" : "", esc);
					url_esc(val, esc, sizeof esc);
					ql += snprintf(q + ql, 8192 - ql, "%s", esc);
					if (ql >= 8000)
						break;
				}
			}
			if (c->first_child && strcmp(tag, "select") && strcmp(tag, "textarea") && sp < 64) {
				stack[sp++] = c;
				c = c->first_child;
				continue;
			}
		}
		c = c->next;
	}
	if (!attr(form, "action", tmp, sizeof tmp) || !tmp[0])
		util_strlcpy(tmp, B.url, sizeof tmp);
	resolve(B.url, tmp, act, sizeof act);
	act[strcspn(act, "?#")] = 0;
	char *url = malloc(1024 + 8192);
	if (url) {
		snprintf(url, 1024 + 8192, "%s?%s", act, q);
		util_log("web: formulario -> %s", url);
		char *u2 = malloc(1024);
		if (u2) { // B.url guarda 1024: si la dirección es más larga, se corta (raro en búsquedas)
			util_strlcpy(u2, url, 1024);
			go(u2, true);
			free(u2);
		}
		free(url);
	}
	free(q);
}

// un control de formulario: ocupa su caja en la línea
static void widget(dom_node *node, const char *tag, const St *st, const css_computed_style *cs)
{
	char type[24] = "text";
	if (!strcmp(tag, "input") && attr(node, "type", type, sizeof type))
		for (char *p = type; *p; p++)
			*p = (char)tolower((unsigned char)*p);
	if (!strcmp(tag, "button"))
		util_strlcpy(type, "submit", sizeof type);
	Wdg w = {.node = node};
	if (!strcmp(tag, "textarea"))
		w.type = WG_AREA;
	else if (!strcmp(tag, "select"))
		w.type = WG_SELECT;
	else if (!strcmp(type, "password"))
		w.type = WG_PASS;
	else if (!strcmp(type, "checkbox"))
		w.type = WG_CHECK;
	else if (!strcmp(type, "radio"))
		w.type = WG_RADIO;
	else if (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "reset") || !strcmp(type, "image"))
		w.type = WG_BUTTON;
	else if (!strcmp(type, "hidden") || !strcmp(type, "file") || !strcmp(type, "range") || !strcmp(type, "color"))
		return;
	else
		w.type = WG_TEXT;
	FVal *v = fv_get(node, false);
	if (w.type == WG_BUTTON) {
		if (!strcmp(tag, "button"))
			text_of(node, w.label, sizeof w.label);
		else if (!attr(node, "value", w.label, sizeof w.label) && !attr(node, "alt", w.label, sizeof w.label))
			util_strlcpy(w.label, !strcmp(type, "reset") ? "Borrar" : "Enviar", sizeof w.label);
	} else if (w.type == WG_SELECT) {
		char val[256];
		select_pick(node, v && v->value ? atoi(v->value) : -1, w.label, sizeof w.label, val, sizeof val, NULL);
	} else if (w.type == WG_TEXT || w.type == WG_PASS || w.type == WG_AREA) {
		if (v && v->value)
			util_strlcpy(w.label, v->value, sizeof w.label);
		else if (!attr(node, "value", w.label, sizeof w.label) || !w.label[0])
			w.label[0] = 0;
	}
	lwc_string *fn;
	if (lwc_intern_string("form", 4, &fn) == lwc_error_ok) {
		dom_element_named_ancestor_node((dom_element *)node, fn, (dom_element **)&w.form);
		if (w.form)
			dom_node_unref(w.form); // sigue vivo: está en el árbol
		lwc_string_unref(fn);
	}
	float lh = text_lineh(st->sc), bw, bh = lh * 1.5f;
	css_fixed cv;
	css_unit cu;
	char a[16];
	float avail = RIGHT - L_indent;
	if (w.type == WG_CHECK || w.type == WG_RADIO)
		bw = bh = lh;
	else if (css_computed_width(cs, &cv, &cu) == CSS_WIDTH_SET)
		bw = len_px(cs, cv, cu, avail);
	else if (w.type == WG_BUTTON || w.type == WG_SELECT)
		bw = text_width(w.label[0] ? w.label : "Enviar", st->sc) + lh * (w.type == WG_SELECT ? 2.2f : 1.4f);
	else if (w.type == WG_AREA)
		bw = (attr(node, "cols", a, sizeof a) ? atoi(a) : 30) * text_width("n", st->sc) + 12;
	else
		bw = attr(node, "size", a, sizeof a) && atoi(a) > 0 ? atoi(a) * text_width("n", st->sc) + 12 : 180;
	if (w.type == WG_AREA)
		bh = (attr(node, "rows", a, sizeof a) && atoi(a) > 0 ? atoi(a) : 2) * lh + 8;
	bw = clampf(bw, 12, avail > 40 ? avail : 400);
	if (B.nwg == B.cwg) {
		int nc = B.cwg ? B.cwg * 2 : 16;
		Wdg *nw = realloc(B.wg, nc * sizeof *nw);
		if (!nw)
			return;
		B.wg = nw, B.cwg = nc;
	}
	B.wg[B.nwg] = w;
	int nf0 = B.nf;
	put_box(bw, bh, st, -1);
	if (B.nf > nf0) {
		B.f[B.nf - 1].wdg = (s16)B.nwg;
		B.nwg++;
	}
}

static void walk_css(dom_node *node, const char *tag, St st, const css_computed_style *cs, uint8_t d);
static bool css_invisible(const css_computed_style *cs);
static bool is(const char *t, const char *const *list);
static float measure(dom_node *n, float sc, float cap, int depth);
static void line_at(float y);
static float cur_bottom(void);
static const char *const NEVER_TAGS[] = {"script", "style", "noscript", "template", "svg", "iframe", "object", "embed", "canvas", "video", "audio", "head", NULL};


/* lo que mide el contenido sin partir líneas (aproximado, sin CSS: para repartir el ancho de una fila) */
static float measure(dom_node *n, float sc, float cap, int depth)
{
	static const char *const BLK[] = {"div", "p", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "table", "tr", "section", "article", "header",
	                                  "footer", "nav", "aside", "main", "form", "dl", "dd", "dt", "figure", "blockquote", "pre", "br", NULL};
	if (depth > 30)
		return 0;
	float sum = 0, mx = 0;
	for (dom_node_internal *c = NI(n)->first_child; c && sum < cap && mx < cap; c = c->next) {
		if (c->type == DOM_TEXT_NODE) {
			dom_string *d = NULL;
			if (dom_characterdata_get_data((dom_characterdata *)c, &d) == DOM_NO_ERR && d) {
				const char *t = dom_string_data(d);
				size_t l = dom_string_byte_length(d), k = 0;
				char buf[256];
				bool sp = false;
				for (size_t i = 0; i < l && k < sizeof buf - 1; i++) { // espacios juntos, uno
					char ch = t[i];
					bool w = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
					if (w) {
						if (!sp && k)
							buf[k++] = ' ';
						sp = true;
					} else
						buf[k++] = ch, sp = false;
				}
				if (k)
					sum += text_width_n(buf, (int)k, sc) * (l > sizeof buf ? (float)l / sizeof buf : 1);
				dom_string_unref(d);
			}
		} else if (c->type == DOM_ELEMENT_NODE) {
			char tag[16];
			tag_of((dom_node *)c, tag, sizeof tag);
			if (is(tag, NEVER_TAGS))
				continue;
			if (!strcmp(tag, "img")) { // su ancho; si no se va a ver (SVG), lo que ocupa su texto alternativo
				char v[160];
				float w = attr((dom_node *)c, "width", v, sizeof v) ? (float)atof(v) : 0;
				if (attr((dom_node *)c, "src", v, sizeof v) && strstr(v, ".svg"))
					w = 0;
				if (w <= 0 && attr((dom_node *)c, "alt", v, sizeof v) && v[0])
					w = text_width(v, sc) + text_width("[]", sc);
				sum += w > 0 ? w : 40;
				continue;
			}
			float m = measure((dom_node *)c, sc, cap, depth + 1);
			if (is(tag, BLK)) {
				mx = fmaxf(mx, fmaxf(sum, m));
				sum = 0;
			} else
				sum += m + 4;
		}
	}
	return fminf(cap, fmaxf(mx, sum));
}

// la palabra más ancha (lo mínimo que puede medir una columna sin partir palabras)
static float min_word(dom_node *n, float sc, int depth)
{
	if (depth > 30)
		return 0;
	float mx = 0;
	for (dom_node_internal *c = NI(n)->first_child; c; c = c->next) {
		if (c->type == DOM_TEXT_NODE) {
			dom_string *d = NULL;
			if (dom_characterdata_get_data((dom_characterdata *)c, &d) == DOM_NO_ERR && d) {
				const char *t = dom_string_data(d);
				size_t l = dom_string_byte_length(d);
				for (size_t i = 0; i < l && i < 2000;) {
					while (i < l && isspace((unsigned char)t[i]))
						i++;
					size_t j = i;
					while (j < l && !isspace((unsigned char)t[j]))
						j++;
					if (j > i)
						mx = fmaxf(mx, text_width_n(t + i, (int)(j - i), sc));
					i = j;
				}
				dom_string_unref(d);
			}
		} else if (c->type == DOM_ELEMENT_NODE) {
			char tag[16];
			tag_of((dom_node *)c, tag, sizeof tag);
			if (is(tag, NEVER_TAGS))
				continue;
			if (!strcmp(tag, "img")) {
				char v[160];
				float w = attr((dom_node *)c, "width", v, sizeof v) ? (float)atof(v) : 0;
				if (attr((dom_node *)c, "src", v, sizeof v) && strstr(v, ".svg"))
					w = 0;
				if (w <= 0 && attr((dom_node *)c, "alt", v, sizeof v) && v[0])
					w = text_width(v, sc) + text_width("[]", sc);
				mx = fmaxf(mx, w);
			} else
				mx = fmaxf(mx, min_word((dom_node *)c, sc, depth + 1));
		}
	}
	return mx;
}

// dónde acaba lo colocado hasta ahora
static float cur_bottom(void)
{
	if (!B.nl)
		return L_pb;
	const Line *l = &B.l[B.nl - 1];
	return (l->used || l->hr) ? fmaxf(L_pb, l->y + l->h) : L_pb;
}

// que la línea en curso sea una vacía que empieza en y
static void line_at(float y)
{
	if (!B.nl || L_has || B.l[B.nl - 1].hr)
		line_new();
	Line *l = &B.l[B.nl - 1];
	L_pb = y;
	l->gap = 0;
	l->y = y;
	l->align = L_align, l->left = L_indent, l->right = RIGHT;
	line_bounds(l);
	L_x = l->left;
	L_gap = 0;
}

/* hijos uno al lado de otro: flex en fila, celdas de una fila de tabla, rejilla. El ancho de cada uno
 * sale de su CSS (width, flex-basis) o de lo que mide; lo que sobra se reparte con flex-grow y, si
 * no cabe, se encoge (o pasa a otra fila si la página deja partir) */
#define MAX_COLS 48
static void row_children(dom_node *node, St st, bool wrap, float gapx)
{
	struct {
		dom_node *n;
		css_computed_style *cs;
		char tag[16];
		uint8_t d;
		float w, grow, minw;
		bool fixed;
	} c[MAX_COLS];
	int n = 0;
	float avail = st.right - st.indent;
	dom_node_internal *rest = NULL; // lo que no cabe en MAX_COLS sigue debajo, normal
	for (dom_node_internal *ch = NI(node)->first_child; ch; ch = ch->next) {
		if (n == MAX_COLS) {
			rest = ch;
			break;
		}
		if (ch->type == DOM_TEXT_NODE) {
			dom_string *d = NULL;
			bool blank = true;
			if (dom_characterdata_get_data((dom_characterdata *)ch, &d) == DOM_NO_ERR && d) {
				for (size_t i = 0; i < dom_string_byte_length(d) && blank; i++)
					blank = isspace((unsigned char)dom_string_data(d)[i]);
				if (!blank) {
					c[n] = (typeof(c[0])){.n = (dom_node *)ch, .cs = NULL, .w = 0, .grow = 0};
					c[n].tag[0] = 0;
					c[n].w = fminf(avail, text_width_n(dom_string_data(d), (int)fminf(dom_string_byte_length(d), 200), st.sc) + 4);
					n++;
				}
				dom_string_unref(d);
			}
			continue;
		}
		if (ch->type != DOM_ELEMENT_NODE)
			continue;
		char tag[16];
		tag_of((dom_node *)ch, tag, sizeof tag);
		if (is(tag, NEVER_TAGS))
			continue;
		css_computed_style *cs = brcss_style(B.css, (dom_node *)ch, st.cs, B.vw);
		if (!cs)
			continue;
		uint8_t d = css_computed_display(cs, false);
		if (d == CSS_DISPLAY_NONE || css_computed_position(cs) == CSS_POSITION_FIXED || css_invisible(cs)) {
			css_computed_style_destroy(cs);
			continue;
		}
		css_fixed v;
		css_unit u;
		float w = 0;
		bool fixed = false;
		if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET) {
			w = len_px(cs, v, u, avail);
			fixed = true;
		} else if (css_computed_flex_basis(cs, &v, &u) == CSS_FLEX_BASIS_SET) {
			w = len_px(cs, v, u, avail); // flex: 1 es base 0: todo el ancho sale de flex-grow
			fixed = true;
		}
		css_fixed fl;
		css_computed_font_size(cs, &v, &u);
		float sc = clampf(SC16 * brcss_px(cs, v, u, B.vw) / 16, 0.15f, 2.4f);
		if (!fixed) {
			float ex = 0;
			if (css_computed_padding_left(cs, &v, &u) == CSS_PADDING_SET)
				ex += len_px(cs, v, u, avail);
			if (css_computed_padding_right(cs, &v, &u) == CSS_PADDING_SET)
				ex += len_px(cs, v, u, avail);
			if (css_computed_margin_left(cs, &v, &u) == CSS_MARGIN_SET)
				ex += len_px(cs, v, u, avail);
			if (css_computed_margin_right(cs, &v, &u) == CSS_MARGIN_SET)
				ex += len_px(cs, v, u, avail);
			w = measure((dom_node *)ch, sc, avail, 0) + clampf(ex, 0, 80) + 2;
		}
		c[n].minw = fixed ? 0 : fminf(min_word((dom_node *)ch, sc, 0) + 10, avail * 0.6f);
		c[n].n = (dom_node *)ch, c[n].cs = cs, c[n].d = d;
		util_strlcpy(c[n].tag, tag, sizeof c[n].tag);
		c[n].w = clampf(w, 0, avail);
		c[n].grow = css_computed_flex_grow(cs, &fl) == CSS_FLEX_GROW_SET ? FIXTOFLT(fl) : 0;
		c[n].fixed = fixed;
		n++;
	}
	// a filas (si se puede partir) y en cada una: repartir lo que sobra o encoger
	float top = (L_has || !B.nl) ? cur_bottom() : B.l[B.nl - 1].y;
	int i0 = 0;
	float o_ind = L_indent, o_right = L_right;
	while (i0 < n) {
		int i1 = i0;
		float tot = 0, grow = 0;
		while (i1 < n && (i1 == i0 || !wrap || tot + gapx + c[i1].w <= avail)) {
			tot += (i1 > i0 ? gapx : 0) + c[i1].w;
			grow += c[i1].grow;
			i1++;
		}
		float extra = avail - tot;
		if (extra > 0 && grow > 0)
			for (int i = i0; i < i1; i++)
				c[i].w += extra * c[i].grow / grow;
		else if (extra < 0) { // no cabe: se encoge lo que no tiene ancho fijo (y si no, todo)
			float flex = 0;
			for (int i = i0; i < i1; i++)
				flex += c[i].fixed ? 0 : c[i].w;
			float need = -extra;
			for (int i = i0; i < i1; i++) {
				float share = flex > 0 ? (c[i].fixed ? 0 : c[i].w / flex) : c[i].w / tot;
				c[i].w = fmaxf(fmaxf(24, c[i].minw), c[i].w - need * share);
			}
		}
		for (int i = i0; i < i1; i++)
			c[i].w = fmaxf(c[i].w, 16);
		float x = st.indent, maxb = top;
		int nbg0 = B.nbg;
		for (int i = i0; i < i1; i++) {
			L_indent = x;
			L_right = x + c[i].w;
			L_align = st.align;
			line_at(top);
			St cs = st;
			cs.indent = x, cs.right = x + c[i].w, cs.cells = NULL, cs.fixw = true;
			if (c[i].cs)
				walk_css(c[i].n, c[i].tag, cs, c[i].cs, c[i].d);
			else
				walk(c[i].n, cs);
			maxb = fmaxf(maxb, cur_bottom());
			x += c[i].w + gapx;
		}
		// align-items: stretch: los fondos de las columnas llegan hasta la más alta
		for (int k = nbg0; k < B.nbg; k++)
			if (B.bg[k].y0 <= top + 8)
				B.bg[k].y1 = fmaxf(B.bg[k].y1, maxb + 4);
		L_indent = o_ind, L_right = o_right;
		line_at(maxb);
		top = maxb;
		i0 = i1;
	}
	for (int i = 0; i < n; i++)
		if (c[i].cs)
			css_computed_style_destroy(c[i].cs);
	for (dom_node_internal *ch = rest; ch; ch = ch->next) // lo que no cupo en la fila
		walk((dom_node *)ch, st);
}

// un flotante: se coloca a su lado en su ancho y deja la zona ocupada para lo que siga
static bool place_float(dom_node *node, const char *tag, St st, const css_computed_style *cs, uint8_t side)
{
	float avail = st.right - st.indent, w = 0;
	css_fixed v;
	css_unit u;
	if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET)
		w = len_px(cs, v, u, avail);
	else if (!strcmp(tag, "img")) {
		char a[16];
		w = attr(node, "width", a, sizeof a) ? (float)atof(a) : 0;
	} else {
		css_computed_font_size(cs, &v, &u);
		w = measure(node, clampf(SC16 * brcss_px(cs, v, u, B.vw) / 16, 0.15f, 2.4f), avail, 0) + 16;
	}
	if (w <= 0 || w > avail * 0.7f || NFL >= 64) // casi todo el ancho: mejor en su sitio, sin flotar
		return false;
	float top = B.nl ? B.l[B.nl - 1].y : L_pb;
	if (L_has) // a la altura de la línea en curso (que se cierra)
		top = B.l[B.nl - 1].y;
	// al lado de los flotantes que ya haya a esa altura; si no cabe, debajo del que acabe antes
	float o_ind = L_indent, o_right = L_right;
	u8 o_align = L_align;
	L_indent = st.indent, L_right = st.right;
	float l = st.indent, r = st.right;
	for (int tries = 0; tries < 8; tries++) {
		float lo = st.indent, ro = st.right, next = 1e30f;
		for (int i = 0; i < NFL; i++) {
			const Fl *f = &FL[i];
			if (top >= f->y1 || top + 1 <= f->y0 || f->x1 <= st.indent - 1 || f->x0 >= st.right + 1)
				continue;
			if (f->side == 1)
				lo = fmaxf(lo, f->x1 + 4);
			else
				ro = fminf(ro, f->x0 - 4);
			next = fminf(next, f->y1);
		}
		l = lo, r = ro;
		if (r - l >= w - 1 || next > 1e29f)
			break;
		top = next;
	}
	float x = side == 1 ? l : r - w;
	St fs = st;
	fs.indent = x, fs.right = x + w, fs.fixw = true;
	L_indent = x, L_right = x + w;
	line_at(top);
	walk_css(node, tag, fs, cs, CSS_DISPLAY_BLOCK);
	float bot = cur_bottom();
	FL[NFL++] = (Fl){x, x + w, top, bot, side};
	L_indent = o_ind, L_right = o_right, L_align = o_align;
	line_at(top);
	return true;
}

/* un elemento con su estilo calculado: la página decide qué se ve, colores, tamaños, márgenes */
static void walk_css(dom_node *node, const char *tag, St st, const css_computed_style *cs, uint8_t d)
{
	char a[256];
	st.cs = cs;
	st.el = node;
	if (st.link < 0 && (attr(node, "onclick", a, sizeof a) || (attr(node, "role", a, sizeof a) && (!strcmp(a, "button") || !strcmp(a, "tab") || !strcmp(a, "link")))))
		st.clk = B.nclk++;
	css_color c;
	if (css_computed_color(cs, &c) == CSS_COLOR_COLOR)
		st.col = css_col(c) | 0xFF000000;
	css_fixed len;
	css_unit u;
	css_computed_font_size(cs, &len, &u);
	float fpx = brcss_px(cs, len, u, B.vw);
	st.sc = clampf(SC16 * fpx / 16, 0.15f, 2.4f);
	uint8_t fw = css_computed_font_weight(cs);
	st.bold = fw == CSS_FONT_WEIGHT_BOLD || fw == CSS_FONT_WEIGHT_BOLDER || fw >= CSS_FONT_WEIGHT_600;
	st.under = (css_computed_text_decoration(cs) & CSS_TEXT_DECORATION_UNDERLINE) != 0;
	uint8_t ws = css_computed_white_space(cs);
	st.pre = ws == CSS_WHITE_SPACE_PRE || ws == CSS_WHITE_SPACE_PRE_WRAP;
	uint8_t ta = css_computed_text_align(cs);
	st.align = (ta == CSS_TEXT_ALIGN_CENTER || ta == CSS_TEXT_ALIGN_LIBCSS_CENTER) ? 1 : (ta == CSS_TEXT_ALIGN_RIGHT || ta == CSS_TEXT_ALIGN_LIBCSS_RIGHT) ? 2 : 0;
	st.hidden = css_computed_visibility(cs) == CSS_VISIBILITY_HIDDEN;

	if (!strcmp(tag, "br")) {
		if (L_has)
			line_new();
		else
			L_gap += text_lineh(st.sc);
		return;
	}
	if (!strcmp(tag, "hr")) {
		block(4);
		Line *l = line_new();
		if (l) {
			l->hr = true;
			l->h = 6;
		}
		L_gap = 4;
		return;
	}
	if (!strcmp(tag, "input") || !strcmp(tag, "button") || !strcmp(tag, "select") || !strcmp(tag, "textarea")) {
		widget(node, tag, &st, cs);
		return;
	}
	if (!strcmp(tag, "img") && B.light) { // modo ligero: la imagen no ocupa sitio; si dice qué es, su texto
		char alt[160];
		if (attr(node, "alt", alt, sizeof alt) && alt[0] && st.link >= 0)
			text_run(alt, strlen(alt), &st);
		return;
	}
	if (!strcmp(tag, "img")) {
		char src[1024], abs[1024], v[32];
		bool ok = attr(node, "src", src, sizeof src) && src[0] && strncmp(src, "data:", 5);
		if (!ok) // las que se cargan «perezosas» guardan la dirección en data-src
			ok = attr(node, "data-src", src, sizeof src) && src[0];
		if (ok) {
			float w = 0, h = 0;
			if (attr(node, "width", v, sizeof v))
				w = (float)atof(v);
			if (attr(node, "height", v, sizeof v))
				h = (float)atof(v);
			css_fixed cv;
			css_unit cu;
			float avail = RIGHT - L_indent;
			if (css_computed_width(cs, &cv, &cu) == CSS_WIDTH_SET)
				w = len_px(cs, cv, cu, avail);
			if (css_computed_height(cs, &cv, &cu) == CSS_HEIGHT_SET && cu != CSS_UNIT_PCT)
				h = len_px(cs, cv, cu, avail);
			resolve(B.url, src, abs, sizeof abs);
			int k = img_find_or_add(abs);
			if (k >= 0) {
				PImg *im = &B.img[k];
				if (w <= 0 && h <= 0 && im->iw) // ya se sabe su tamaño (se bajó antes)
					w = im->iw, h = im->ih;
				else if (w > 0 && h <= 0)
					h = im->iw ? w * im->ih / im->iw : w * 0.75f;
				else if (h > 0 && w <= 0)
					w = im->ih ? h * im->iw / im->ih : h * 1.33f;
				im->sized = w > 0 && h > 0;
				if (!im->sized)
					w = 120, h = 90;
				if (w > avail && avail > 20) // max-width: 100% casi siempre
					h = h * avail / w, w = avail;
				if (w >= 1 && h >= 1) {
					im->w = w, im->h = h;
					put_box(w, h, &st, k);
					return;
				}
			}
		}
		char alt[120];
		attr(node, "alt", alt, sizeof alt);
		if (!alt[0])
			return; // sin texto: decorativa
		char tx[140];
		snprintf(tx, sizeof tx, "[%s]", alt);
		St is = st;
		if (is.link < 0)
			is.col = PG_MUTED;
		L_space = true;
		text_run(tx, strlen(tx), &is);
		L_space = true;
		return;
	}

	bool blk = is_block_display(d);
	float o_ind = L_indent, o_right = L_right;
	u8 o_align = L_align;
	float mb = 0;
	int l0 = -1;
	u32 bgc = 0;
	float brw[4] = {0};
	u32 brc[4] = {0};
	bool has_border = false;
	if (blk) {
		float avail = st.right - st.indent;
		css_fixed v;
		css_unit vu;
		float mt = 0, ml = 0, mr = 0, pt = 0, pl = 0, pr = 0, pb = 0;
		if (css_computed_margin_top(cs, &v, &vu) == CSS_MARGIN_SET)
			mt = len_px(cs, v, vu, avail);
		if (css_computed_margin_bottom(cs, &v, &vu) == CSS_MARGIN_SET)
			mb = len_px(cs, v, vu, avail);
		bool aml = css_computed_margin_left(cs, &v, &vu) == CSS_MARGIN_AUTO;
		if (!aml)
			ml = len_px(cs, v, vu, avail);
		bool amr = css_computed_margin_right(cs, &v, &vu) == CSS_MARGIN_AUTO;
		if (!amr)
			mr = len_px(cs, v, vu, avail);
		if (css_computed_padding_top(cs, &v, &vu) == CSS_PADDING_SET)
			pt = len_px(cs, v, vu, avail);
		if (css_computed_padding_bottom(cs, &v, &vu) == CSS_PADDING_SET)
			pb = len_px(cs, v, vu, avail);
		if (css_computed_padding_left(cs, &v, &vu) == CSS_PADDING_SET)
			pl = len_px(cs, v, vu, avail);
		if (css_computed_padding_right(cs, &v, &vu) == CSS_PADDING_SET)
			pr = len_px(cs, v, vu, avail);
		// los márgenes negativos y enormes de algunos diseños no tienen sentido sin su posición exacta
		ml = clampf(ml, 0, avail * 0.3f), mr = clampf(mr, 0, avail * 0.3f);
		pl = clampf(pl, 0, avail * 0.2f), pr = clampf(pr, 0, avail * 0.2f);
		mt = clampf(mt, 0, 60), mb = clampf(mb, 0, 60) + clampf(pb, 0, 40);
		// ancho: width / max-width, centrado con margin: auto
		float w = avail - ml - mr;
		if (st.fixw) // columna o flotante: su ancho ya es el de fuera
			ml = mr = 0, w = avail, aml = amr = false;
		else {
			if (css_computed_width(cs, &v, &vu) == CSS_WIDTH_SET && d != CSS_DISPLAY_TABLE_ROW)
				w = fminf(w, len_px(cs, v, vu, avail));
			if (css_computed_max_width(cs, &v, &vu) == CSS_MAX_WIDTH_SET)
				w = fminf(w, len_px(cs, v, vu, avail));
		}
		st.fixw = false;
		w = fmaxf(w, 40);
		float x0 = st.indent + ml;
		if (aml && amr)
			x0 = st.indent + (avail - w) / 2;
		else if (aml)
			x0 = st.right - mr - w;
		float bx0 = x0, bx1 = x0 + w;
		st.indent = x0 + pl;
		st.right = x0 + w - pr;
		if (css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR && (c >> 24) > 0x20 && (c & 0xFFFFFF) != 0xFFFFFF && strcmp(tag, "body") && strcmp(tag, "html"))
			bgc = css_col(c) | 0xFF000000;
		// el fondo de <html> (o si no tiene, el de <body>) pinta toda la página, como en los navegadores
		if ((!strcmp(tag, "html") || (!strcmp(tag, "body") && !B.page_bg_html)) && css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR &&
		    (c >> 24) > 0x20) {
			B.page_bg = css_col(c) | 0xFF000000;
			B.page_bg_html |= !strcmp(tag, "html");
		}
		// bordes: ancho y color de cada lado
		{
			uint8_t (*wf[4])(const css_computed_style *, css_fixed *, css_unit *) = {css_computed_border_top_width, css_computed_border_right_width,
			                                                                            css_computed_border_bottom_width, css_computed_border_left_width};
			uint8_t (*sf[4])(const css_computed_style *) = {css_computed_border_top_style, css_computed_border_right_style, css_computed_border_bottom_style,
			                                                css_computed_border_left_style};
			uint8_t (*cf[4])(const css_computed_style *, css_color *) = {css_computed_border_top_color, css_computed_border_right_color,
			                                                               css_computed_border_bottom_color, css_computed_border_left_color};
			for (int k = 0; k < 4; k++) {
				brw[k] = 0;
				uint8_t bs = sf[k](cs);
				if (bs == CSS_BORDER_STYLE_NONE || bs == CSS_BORDER_STYLE_HIDDEN || bs == 0)
					continue;
				css_fixed bv;
				css_unit bu;
				uint8_t wt = wf[k](cs, &bv, &bu);
				brw[k] = wt == CSS_BORDER_WIDTH_THIN ? 1 : wt == CSS_BORDER_WIDTH_THICK ? 5 : wt == CSS_BORDER_WIDTH_WIDTH ? clampf(len_px(cs, bv, bu, avail), 0, 20) : 3;
				css_color bcol;
				brc[k] = cf[k](cs, &bcol) == CSS_BORDER_COLOR_COLOR ? css_col(bcol) : st.col;
				if (((brc[k] >> 24) & 0xFF) < 0x20)
					brw[k] = 0;
				has_border |= brw[k] > 0;
			}
			st.indent += brw[3], st.right -= brw[1]; // el contenido va por dentro del borde
			pt += brw[0], mb += brw[2];
		}
		block(mt + clampf(pt, 0, 40));
		L_indent = st.indent;
		L_right = st.right;
		L_align = st.align;
		line_ctx();
		uint8_t clr = css_computed_clear(cs); // clear: debajo de los flotantes de ese lado
		if (clr == CSS_CLEAR_LEFT || clr == CSS_CLEAR_RIGHT || clr == CSS_CLEAR_BOTH) {
			float fy = 0;
			for (int i = 0; i < NFL; i++)
				if (clr == CSS_CLEAR_BOTH || (clr == CSS_CLEAR_LEFT) == (FL[i].side == 1))
					fy = fmaxf(fy, FL[i].y1);
			if (B.nl && !L_has && B.l[B.nl - 1].y < fy)
				line_at(fy);
		}
		if (bgc || has_border) {
			if (!B.nl || L_has)
				line_new();
			l0 = B.nl - 1; // la línea (vacía) en la que empieza el bloque
			B.bg_x0 = bx0, B.bg_x1 = bx1;
		}
	}
	int ol = 0, cells = 0;
	if (!strcmp(tag, "ol") || !strcmp(tag, "ul"))
		st.olnum = !strcmp(tag, "ol") ? &ol : NULL;
	if (d == CSS_DISPLAY_TABLE_ROW)
		st.cells = &cells;
	int li_frag = -1;
	if (d == CSS_DISPLAY_LIST_ITEM && css_computed_list_style_type(cs) != CSS_LIST_STYLE_TYPE_NONE) {
		char mk[16];
		St ms = st;
		ms.col = PG_MUTED, ms.link = -1, ms.under = false, ms.bold = false;
		if (st.olnum)
			snprintf(mk, sizeof mk, "%d.", ++*st.olnum);
		else
			util_strlcpy(mk, "•", sizeof mk);
		float mw = text_width("00.", ms.sc) + 2;
		if (!B.nl || L_has)
			line_new();
		L_x = st.indent - mw > 0 ? st.indent - mw : st.indent;
		li_frag = B.nf;
		put(mk, strlen(mk), &ms, text_width(mk, ms.sc));
		if (st.indent - mw <= 0) { // sin sitio a la izquierda: la viñeta dentro
			st.indent += mw;
			L_indent = st.indent;
		}
		L_x = st.indent;
		L_has = true;
		L_space = false;
		L_marker = true;
	}
	if (d == CSS_DISPLAY_TABLE_CELL && st.cells && (*st.cells)++) { // separador entre celdas
		St ss = st;
		ss.col = PG_MUTED, ss.link = -1, ss.under = false;
		L_space = true;
		word("·", strlen("·"), &ss);
		L_space = true;
	}
	if (!strcmp(tag, "a") && attr(node, "href", a, sizeof a) && a[0] && strncasecmp(a, "javascript:", 11))
		st.link = link_add(a); // también #ancla: lleva a esa parte de la página
	float bx0 = B.bg_x0, bx1 = B.bg_x1;
	float il = 0, ir = 0; // en línea: márgenes y rellenos a los lados (menús hechos con enlaces seguidos)
	uint8_t fd = css_computed_flex_direction(cs);
	bool row = blk && ((d == CSS_DISPLAY_FLEX && (fd == CSS_FLEX_DIRECTION_ROW || fd == CSS_FLEX_DIRECTION_ROW_REVERSE || fd == CSS_FLEX_DIRECTION_INHERIT)) ||
	                   d == CSS_DISPLAY_TABLE_ROW || d == CSS_DISPLAY_GRID);
	float gapx = 0;
	if (row) {
		css_fixed v;
		css_unit vu;
		if (css_computed_column_gap(cs, &v, &vu) == CSS_COLUMN_GAP_SET)
			gapx = clampf(len_px(cs, v, vu, st.right - st.indent), 0, 60);
		if (d == CSS_DISPLAY_TABLE_ROW)
			gapx = fmaxf(gapx, 6);
	}
	if (!blk) {
		css_fixed v;
		css_unit vu;
		if (css_computed_margin_left(cs, &v, &vu) == CSS_MARGIN_SET)
			il += len_px(cs, v, vu, 0);
		if (css_computed_padding_left(cs, &v, &vu) == CSS_PADDING_SET)
			il += len_px(cs, v, vu, 0);
		if (css_computed_margin_right(cs, &v, &vu) == CSS_MARGIN_SET)
			ir += len_px(cs, v, vu, 0);
		if (css_computed_padding_right(cs, &v, &vu) == CSS_PADDING_SET)
			ir += len_px(cs, v, vu, 0);
		il = clampf(il, 0, 60), ir = clampf(ir, 0, 60);
		if (il > 0 && L_has)
			L_x += il;
	}
	if (row)
		row_children(node, st, d == CSS_DISPLAY_GRID || css_computed_flex_wrap(cs) == CSS_FLEX_WRAP_WRAP, gapx);
	else
		children(node, &st);
	if (!blk && ir > 0 && L_has)
		L_x += ir;
	if (li_frag >= 0 && B.nf == li_frag + 1) { // el <li> no tenía nada visible: fuera su viñeta
		B.nf--;
		B.nt = B.f[B.nf].off;
		B.l[B.f[B.nf].line].used = false;
		L_has = false;
		L_marker = false;
		if (st.olnum)
			--*st.olnum;
	}
	if (blk) {
		int l1 = L_has ? B.nl - 1 : B.nl - 2; // la última línea con algo
		if ((bgc || has_border) && l0 >= 0 && l1 >= l0) {
			bg_add(l0, l1, bx0, bx1, bgc);
			if (B.nbg)
				for (int k = 0; k < 4; k++)
					B.bg[B.nbg - 1].bw[k] = brw[k], B.bg[B.nbg - 1].bc[k] = brc[k];
		}
		block(mb);
		L_indent = o_ind, L_right = o_right, L_align = o_align;
		line_ctx();
	}
}

static void walk(dom_node *node, St st)
{
	if (B.lay_oom)
		return;
	if ((++B.nel_done & 127) == 0 && mem_free() < MEM_RESERVE) { // la memoria se acaba: hasta aquí
		B.lay_oom = true;
		return;
	}
	dom_node_type t;
	if (dom_node_get_node_type(node, &t) != DOM_NO_ERR)
		return;
	if (t == DOM_TEXT_NODE || t == DOM_CDATA_SECTION_NODE) {
		dom_string *d = NULL;
		if (dom_characterdata_get_data((dom_characterdata *)node, &d) == DOM_NO_ERR && d) {
			text_run(dom_string_data(d), dom_string_byte_length(d), &st);
			dom_string_unref(d);
		}
		return;
	}
	if (t == DOM_DOCUMENT_NODE) {
		children(node, &st);
		return;
	}
	if (t != DOM_ELEMENT_NODE)
		return;
	dom_string *nm = NULL;
	if (dom_node_get_node_name(node, &nm) != DOM_NO_ERR || !nm)
		return;
	char tag[16];
	size_t tl = dom_string_byte_length(nm);
	snprintf(tag, sizeof tag, "%.*s", (int)(tl < 15 ? tl : 15), dom_string_data(nm));
	dom_string_unref(nm);
	for (char *p = tag; *p; p++)
		*p = (char)tolower((unsigned char)*p);

	if (!strcmp(tag, "title") && !B.title[0]) {
		dom_string *tx = NULL;
		if (dom_node_get_text_content(node, &tx) == DOM_NO_ERR && tx) {
			size_t l = dom_string_byte_length(tx);
			snprintf(B.title, sizeof B.title, "%.*s", (int)(l < sizeof B.title - 1 ? l : sizeof B.title - 1), dom_string_data(tx));
			dom_string_unref(tx);
		}
		return;
	}
	if (B.css && !strcmp(tag, "head")) { // solo por el <title>
		children(node, &st);
		return;
	}
	if (B.css) { // con CSS: lo decide el estilo de la página
		static const char *const NEVER[] = {"script", "style", "noscript", "template", "svg", "iframe", "object", "embed", "canvas", "video", "audio", NULL};
		if (is(tag, NEVER))
			return;
		css_computed_style *cs = brcss_style(B.css, node, st.cs, B.vw);
		if (cs) {
			uint8_t d = css_computed_display(cs, st.cs == NULL);
			// lo oculto y lo que flota fijo encima de la página (avisos de cookies, barras pegadas)
			uint8_t fl = css_computed_float(cs), pos = css_computed_position(cs);
			if (d != CSS_DISPLAY_NONE && pos != CSS_POSITION_FIXED && !css_invisible(cs)) {
				bool done = false;
				if ((fl == CSS_FLOAT_LEFT || fl == CSS_FLOAT_RIGHT) && pos != CSS_POSITION_ABSOLUTE)
					done = place_float(node, tag, st, cs, fl == CSS_FLOAT_LEFT ? 1 : 2);
				if (!done)
					walk_css(node, tag, st, cs, d);
			}
			css_computed_style_destroy(cs);
			return;
		}
	}
	static const char *const SKIP[] = {"script", "style", "noscript", "template", "svg", "iframe", "button", "input", "select",
	                                   "textarea", "object", "embed", "canvas", "video", "audio", "map", "nav", NULL};
	if (is(tag, SKIP))
		return;
	if (!strcmp(tag, "head")) { // solo para el <title>
		children(node, &st);
		return;
	}
	char a[256];
	if (attr(node, "role", a, sizeof a) && (!strcmp(a, "navigation") || !strcmp(a, "search") || !strcmp(a, "banner")))
		return;
	if (attr(node, "class", a, sizeof a) && skip_class(a)) // menús y desplegables que la página oculta con CSS
		return;
	if (attr(node, "hidden", a, sizeof a) || (attr(node, "aria-hidden", a, sizeof a) && !strcmp(a, "true")))
		return;
	if (attr(node, "style", a, sizeof a)) {
		char *p = a, *q = a;
		for (; *p; p++)
			if (*p != ' ')
				*q++ = (char)tolower((unsigned char)*p);
		*q = 0;
		if (strstr(a, "display:none"))
			return;
	}

	static const char *const BLOCK[] = {"p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "ul",
	                                    "ol", "dl", "dt", "dd", "figure", "figcaption", "form", "address", "details",
	                                    "summary", "table", "caption", "center", "body", "html", NULL};
	int ol = 0, cells = 0;
	float gap = 0;
	if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
		static const float HS[6] = {0.74f, 0.64f, 0.56f, 0.52f, 0.50f, 0.48f};
		st.sc = HS[tag[1] - '1'];
		st.col = PG_HEAD;
		gap = 10;
	} else if (!strcmp(tag, "p"))
		gap = 7;
	else if (!strcmp(tag, "blockquote")) {
		gap = 6;
		st.indent += 12;
		st.col = PG_TEXT;
	} else if (!strcmp(tag, "pre")) {
		gap = 6;
		st.pre = true;
		st.col = PG_CODE;
		st.sc = 0.40f;
	} else if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
		gap = 4;
		st.indent += 6;
		st.olnum = !strcmp(tag, "ol") ? &ol : NULL;
	} else if (!strcmp(tag, "li")) {
		gap = 2;
	} else if (!strcmp(tag, "dd"))
		st.indent += 14;
	else if (!strcmp(tag, "tr")) {
		gap = 3;
		st.cells = &cells;
	} else if (!strcmp(tag, "table"))
		gap = 6;
	else if (is(tag, BLOCK))
		gap = 2;

	bool blk = gap > 0 || is(tag, BLOCK) || !strcmp(tag, "li") || !strcmp(tag, "tr");
	if (blk) {
		block(gap);
		L_indent = st.indent;
	}
	if (!strcmp(tag, "br")) {
		if (L_has)
			line_new();
		else
			L_gap += text_lineh(st.sc);
		return;
	}
	if (!strcmp(tag, "hr")) {
		block(4);
		Line *l = line_new();
		if (l) {
			l->hr = true;
			l->h = 6;
		}
		L_gap = 4;
		return;
	}
	int li_frag = -1;
	if (!strcmp(tag, "li")) { // la viñeta o el número, colgando a la izquierda
		char mk[16];
		St ms = st;
		ms.col = PG_MUTED;
		ms.link = -1, ms.under = false;
		if (st.olnum)
			snprintf(mk, sizeof mk, "%d.", ++*st.olnum);
		else
			util_strlcpy(mk, "•", sizeof mk);
		L_indent = st.indent;
		if (!B.nl || L_has)
			line_new();
		L_x = st.indent;
		li_frag = B.nf;
		put(mk, strlen(mk), &ms, text_width(mk, ms.sc));
		st.indent += text_width("00.", ms.sc) + 2;
		L_x = st.indent;
		L_indent = st.indent;
		L_has = true;
		L_space = false;
		L_marker = true;
	}
	if ((!strcmp(tag, "td") || !strcmp(tag, "th")) && st.cells) {
		if ((*st.cells)++) { // separador entre celdas
			St ss = st;
			ss.col = PG_MUTED, ss.link = -1, ss.under = false;
			L_space = true;
			word("·", strlen("·"), &ss);
			L_space = true;
		}
		if (!strcmp(tag, "th"))
			st.col = PG_HEAD;
	}
	if (!strcmp(tag, "a") && attr(node, "href", a, sizeof a) && a[0] && a[0] != '#' && strncasecmp(a, "javascript:", 11)) {
		st.link = link_add(a);
		st.col = PG_LINK;
		st.under = true;
	} else if (!strcmp(tag, "code") || !strcmp(tag, "kbd") || !strcmp(tag, "samp"))
		st.col = PG_CODE;
	else if (!strcmp(tag, "small") || !strcmp(tag, "sup") || !strcmp(tag, "sub"))
		st.sc = st.sc * 0.82f;
	else if (!strcmp(tag, "img")) {
		char alt[120];
		attr(node, "alt", alt, sizeof alt);
		char tx[140];
		snprintf(tx, sizeof tx, "[%s]", alt[0] ? alt : "imagen");
		St is = st;
		if (is.link < 0)
			is.col = PG_MUTED;
		L_space = true;
		text_run(tx, strlen(tx), &is);
		L_space = true;
		return;
	}
	children(node, &st);
	if (li_frag >= 0 && B.nf == li_frag + 1) { // el <li> no tenía nada visible: fuera su viñeta
		B.nf--;
		B.nt = B.f[B.nf].off;
		B.l[B.f[B.nf].line].used = false;
		L_has = false;
		L_marker = false;
		if (st.olnum)
			--*st.olnum;
	}
	if (blk) {
		L_indent = st.indent;
		block(gap);
		// al salir, la sangría vuelve a la de fuera (la pone el padre al seguir)
	}
}

/* el primer elemento <tag> (en anchura limitada a 40 niveles); hay que soltarlo con dom_node_unref */
static dom_node *find_tag(dom_node *n, const char *tag, int depth)
{
	if (depth > 40)
		return NULL;
	dom_node *c = NULL;
	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return NULL;
	while (c) {
		dom_node_type t;
		if (dom_node_get_node_type(c, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
			dom_string *nm = NULL;
			if (dom_node_get_node_name(c, &nm) == DOM_NO_ERR && nm) {
				bool eq = dom_string_byte_length(nm) == strlen(tag) && !strncasecmp(dom_string_data(nm), tag, strlen(tag));
				dom_string_unref(nm);
				if (eq)
					return c;
			}
			dom_node *r = find_tag(c, tag, depth + 1);
			if (r) {
				dom_node_unref(c);
				return r;
			}
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
	return NULL;
}

static void layout_free(void)
{
	free(B.f), B.f = NULL, B.nf = B.cf = 0;
	free(B.l), B.l = NULL, B.nl = B.cl = 0;
	free(B.text), B.text = NULL, B.nt = B.ct = 0;
	free(B.bg), B.bg = NULL, B.nbg = B.cbg = 0;
	free(B.ord), B.ord = NULL;
	free(B.wg), B.wg = NULL, B.nwg = B.cwg = 0;
	for (int i = 0; i < B.nlinks; i++)
		free(B.links[i]);
	free(B.links), B.links = NULL, B.nlinks = B.clinks = 0;
}

static int ord_cmp(const void *a, const void *b)
{
	const Frag *fa = &B.f[*(const int *)a], *fb = &B.f[*(const int *)b];
	float ya = B.l[fa->line].y, yb = B.l[fb->line].y;
	if (ya != yb)
		return ya < yb ? -1 : 1;
	return *(const int *)a - *(const int *)b;
}

static void layout(void)
{
	img_stop(); // su hilo lee la lista de imágenes, que aquí puede crecer
	layout_free();
	bool mob = false; // siempre de PC: las versiones de móvil a 400 px se veían peor
	B.vw = mob ? VW_MOB : VW_PC;
	PADP = mob ? 8 : 16;
	PW = B.vw - 2 * PADP;
	L_x = L_indent = L_gap = 0;
	L_right = 1e9;
	L_align = 0;
	L_pb = 6;
	NFL = 0;
	L_has = L_space = false;
	SC16 = SC_TEXT * 19.2f / text_lineh(SC_TEXT); // 16 px de letra = 19,2 px de línea
	St st = {.sc = SC_TEXT, .col = PG_TEXT, .link = -1, .right = PAGE_W, .clk = -1};
	B.nclk = 0;
	u64 t0 = util_ms();
	B.nel_done = 0;
	B.lay_oom = false;
	B.page_bg = 0, B.page_bg_html = false;
	// modo lectura: si la página marca su contenido (<main> o <article>), solo eso (más el <title>)
	// sin CSS, modo lectura: si la página marca su contenido (<main> o <article>), solo eso
	dom_node *content = B.css ? NULL : find_tag((dom_node *)B.doc, "main", 0);
	if (!content && !B.css)
		content = find_tag((dom_node *)B.doc, "article", 0);
	if (content) {
		dom_node *head = find_tag((dom_node *)B.doc, "head", 0);
		if (head) {
			walk(head, st);
			dom_node_unref(head);
		}
		walk(content, st);
		dom_node_unref(content);
	} else
		walk((dom_node *)B.doc, st);
	if (L_has)
		line_new();
	B.t_css = (u32)(util_ms() - t0);
	B.page_ver++;
	// alinear (text-align): cada línea se corre dentro de sus márgenes
	for (int i = 0, f = 0; i < B.nl; i++) {
		int f0 = f;
		float end = B.l[i].left;
		while (f < B.nf && B.f[f].line == i) {
			end = fmaxf(end, B.f[f].x + B.f[f].w);
			f++;
		}
		if (!B.l[i].align || f == f0)
			continue;
		float free_w = B.l[i].right - end;
		float dx = B.l[i].align == 1 ? free_w / 2 : free_w;
		if (dx > 0)
			for (int k = f0; k < f; k++)
				B.f[k].x += dx;
	}
	float y = 6; // las y ya se pusieron al crear cada línea; el alto total es lo más bajo
	for (int i = 0; i < B.nl; i++) {
		if (!B.l[i].used && !B.l[i].hr)
			B.l[i].h = 0;
		y = fmaxf(y, B.l[i].y + B.l[i].h);
	}
	B.total = y + 20;
	// orden para pintar y tocar: por altura (con columnas, el orden del documento ya no lo es)
	free(B.ord);
	B.ord = malloc(sizeof *B.ord * (B.nf ? B.nf : 1));
	if (B.ord) {
		for (int i = 0; i < B.nf; i++)
			B.ord[i] = i;
		qsort(B.ord, B.nf, sizeof *B.ord, ord_cmp);
	}
	for (int i = 0; i < B.nf; i++) // dónde está cada imagen (las de cerca se bajan antes)
		if (B.f[i].img >= 0 && B.f[i].img < B.nimg)
			B.img[B.f[i].img].y = B.l[B.f[i].line].y;
	B.off = B.vel = 0;
	B.zoom = TOP_W / B.vw; // entera a lo ancho; para leer, zoom
	B.sx = B.sy = B.vy = 0;
	B.px = TOP_W / 2, B.py = SCR_H / 2;
	B.focus = -1;
	B.tip = 2.5f;
	util_log("web: %s -> %u bytes, %d hojas CSS, %d líneas, %d trozos, %d enlaces, maquetar %lu ms", B.url, (unsigned)B.bytes, B.ncss, B.nl, B.nf,
	         B.nlinks, (unsigned long)B.t_css);
}

/* ------------------------------------------------------------ uso */
// un enlace a un archivo (no a una página): se descarga, como en cualquier navegador
static bool is_file_url(const char *url)
{
	static const char *const EXT[] = {"cia", "3dsx", "zip", "7z", "rar", "tar", "gz", "bin", "firm", "nds", "gba", "gb", "gbc", "smc", "sfc", "nes",
	                                  "iso", "cxi", "cci", "3ds", "apk", "exe", "mp3", "ogg", "wav", "mp4", "pdf", "txt", "json", NULL};
	if (!strstr(url, "://"))
		return false;
	size_t l = strcspn(url, "?#");
	const char *dot = NULL;
	for (size_t i = l; i > 0 && url[i - 1] != '/'; i--)
		if (url[i - 1] == '.') {
			dot = url + i;
			break;
		}
	if (!dot)
		return false;
	for (int k = 0; EXT[k]; k++)
		if ((size_t)(url + l - dot) == strlen(EXT[k]) && !strncasecmp(dot, EXT[k], strlen(EXT[k])))
			return true;
	return false;
}

static void scroll_to_anchor(const char *id);

/* lo escrito en la barra: una dirección, o si no lo parece, una búsqueda en Google */
static bool looks_like_address(const char *t)
{
	if (strstr(t, "://") || t[0] == '/' || !strncasecmp(t, "localhost", 9))
		return true;
	if (strchr(t, ' ') || !strchr(t, '.'))
		return false;
	const char *dot = strrchr(t, '.'), *end = dot + 1; // lo de tras el último punto: com, es, org… o una IP
	size_t n = strcspn(end, "/:?#");
	if (n < 2) {
		bool digits = n > 0;
		for (size_t i = 0; i < n; i++)
			digits = digits && isdigit((unsigned char)end[i]);
		return digits;
	}
	for (size_t i = 0; i < n; i++)
		if (!isalnum((unsigned char)end[i]))
			return false;
	return dot != t;
}

static void address_to_url(const char *t, char *out, size_t n)
{
	while (*t == ' ')
		t++;
	if (!looks_like_address(t)) {
		char q[900];
		url_esc(t, q, sizeof q);
		snprintf(out, n, "https://html.duckduckgo.com/html/?q=%s", q); // Google no deja buscar a este navegador (ver go)
		util_strlcpy(B.search_q, t, sizeof B.search_q);
		B.search_stage = 1;
	} else if (!strstr(t, "://") && t[0] != '/')
		snprintf(out, n, "https://%s", t);
	else
		util_strlcpy(out, t, n);
}

static bool is_search_host(const char *url)
{
	char host[128], path[8];
	ck_host(url, host, sizeof host, path, sizeof path);
	return strstr(host, "google.") || strstr(host, "duckduckgo.com") || strstr(host, "bing.com");
}

// el valor de un parámetro de la dirección (?q=…), ya decodificado
static bool url_param(const char *url, const char *name, char *out, size_t n)
{
	const char *q = strchr(url, '?');
	size_t nl = strlen(name);
	for (const char *p = q; p && *p; p = strchr(p + 1, '&')) {
		const char *k = p + 1;
		if (!strncmp(k, name, nl) && k[nl] == '=') {
			const char *v = k + nl + 1;
			size_t j = 0;
			for (; *v && *v != '&' && *v != '#' && j + 1 < n; v++) {
				if (*v == '+')
					out[j++] = ' ';
				else if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) {
					char hx[3] = {v[1], v[2], 0};
					out[j++] = (char)strtol(hx, NULL, 16);
					v += 2;
				} else
					out[j++] = *v;
			}
			out[j] = 0;
			return true;
		}
	}
	return false;
}

/* buscadores que no funcionan con este navegador, a sus versiones que sí:
 * - Google: su comprobación con JavaScript detecta que no es Chrome ni Firefox y responde 429 a la
 *   búsqueda (visto en la consola: siempre, con httpc y con libcurl). Se busca lo mismo en DuckDuckGo.
 * - duckduckgo.com: su página normal es una aplicación que necesita partes de JavaScript que el
 *   motor no tiene; html.duckduckgo.com es la versión sin JavaScript. */
static bool search_rewrite(const char *url, char *out, size_t n)
{
	char host[128], path[256], q[512];
	ck_host(url, host, sizeof host, path, sizeof path);
	bool google = strstr(host, "google.") != NULL, ddg = !strcmp(host, "duckduckgo.com") || !strcmp(host, "www.duckduckgo.com");
	if (google && !strcmp(path, "/search") && url_param(url, "q", q, sizeof q) && q[0]) {
		char e[900];
		url_esc(q, e, sizeof e);
		snprintf(out, n, "https://html.duckduckgo.com/html/?q=%s", e);
		util_strlcpy(B.search_q, q, sizeof B.search_q);
		B.search_stage = 1;
		toast_icon("Google no deja buscar a este navegador: resultados de DuckDuckGo", IC_SEARCH, T.accent);
		return true;
	}
	if (ddg && (!strcmp(path, "/") || !strcmp(path, "/search"))) {
		if (url_param(url, "q", q, sizeof q) && q[0]) {
			char e[900];
			url_esc(q, e, sizeof e);
			snprintf(out, n, "https://html.duckduckgo.com/html/?q=%s", e);
			util_strlcpy(B.search_q, q, sizeof B.search_q);
			B.search_stage = 1;
		} else
			snprintf(out, n, "https://html.duckduckgo.com/html/");
		return true;
	}
	return false;
}

static void go(const char *url, bool push)
{
	char rw[1024];
	if (search_rewrite(url, rw, sizeof rw))
		url = rw;
	if (!is_search_host(url))
		B.search_q[0] = 0;
	const char *hash = strchr(url, '#');
	size_t base_n = hash ? (size_t)(hash - url) : strlen(url);
	size_t cur_n = strcspn(B.url, "#");
	if (hash && B.st == ST_READY && base_n == cur_n && !strncmp(url, B.url, base_n)) { // en esta misma página
		scroll_to_anchor(hash + 1);
		return;
	}
	if (hash)
		snprintf(B.anchor, sizeof B.anchor, "%s", hash + 1);
	if (is_file_url(url)) {
		size_t l = strcspn(url, "?#");
		bool cia = l > 4 && !strncasecmp(url + l - 4, ".cia", 4);
		if (dl_add(url, cia ? "/cias" : "/Descargas", "", cia ? DLA_INSTALL | DLA_STREAM : DLA_NONE, "Navegador", NULL) == 0)
			toast_icon(cia ? "Descargando e instalando: lo verás en Actividad" : "Descargando en /Descargas: lo verás en Actividad", IC_DOWNLOAD, T.accent);
		else
			toast_icon("No se pudo descargar ese enlace", IC_CLOSE, T.danger);
		return;
	}
	if (push && B.url[0] && B.st != ST_IDLE) {
		if (B.nhist == HIST) {
			memmove(B.hist[0], B.hist[1], sizeof B.hist[0] * (HIST - 1));
			B.nhist--;
		}
		B.hist_sy[B.nhist] = B.sy;
		util_strlcpy(B.hist[B.nhist++], B.url, sizeof B.hist[0]);
	}
	start_load(url);
}

#define LIGHT_PATH APP_DIR "/navegador_ligero"
static void light_load(void)
{
	FILE *f = fopen(LIGHT_PATH, "r");
	B.light = f != NULL;
	if (f)
		fclose(f);
}

static void light_save(void)
{
	if (B.light) {
		FILE *f = fopen(LIGHT_PATH, "w");
		if (f)
			fputs("1\n", f), fclose(f);
	} else
		remove(LIGHT_PATH);
}

void br_open_url(const char *url)
{
	light_load();
	nohc_init();
	B.js = true;
	B.open = true;
	B.home = false;
	B.panel = 0;
	B.came_home = false;
	B.opt = false;
	B.nhist = 0;
	B.force = 0;
	B.st = ST_IDLE;
	go(url, false);
}

void br_open_file(const char *sd_path)
{
	br_open_url(sd_path);
}

bool br_is_open(void)
{
	return B.open;
}

static void close_br(void)
{
	B.open = false;
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp); // antes que el documento: tiene sus nodos
	B.jsp = NULL;
	if (B.doc) {
		dom_node_unref(B.doc);
		B.doc = NULL;
	}
	brcss_free(B.css);
	B.css = NULL;
	img_free_all();
	fv_free();
	layout_free();
	B.st = ST_IDLE;
}

void br_exit(void)
{
	close_br();
}

static void home_enter(void);
static void back(void)
{
	if (B.nhist) {
		--B.nhist;
		float sy = B.hist_sy[B.nhist];
		start_load(B.hist[B.nhist]);
		B.back_sy = sy;
	}
	else if (B.came_home)
		home_enter();
	else
		close_br();
}

/* ------------------------------------------------------------ vista */
#define TS_TITLE 0.58f
#define TS_SUB   0.40f
static void text_draw_center_clip(const char *t, float cx, float y, float maxw, float sc, u32 c)
{
	float w = fminf(text_width(t, sc), maxw);
	text_draw_clip(t, cx - w / 2, y, maxw, sc, c);
}

static float view_w(void) { return TOP_W / B.zoom; }
static float view_h(void) { return SCR_H / B.zoom; }

static void clamp_view(void);

// lleva la vista al elemento con ese id (o <a name>): su primer trozo colocado
void scroll_to_anchor(const char *id)
{
	if (!id[0] || !strcasecmp(id, "top")) { // «#» o «#top»: arriba del todo
		B.sy = 0, B.vy = 0;
		return;
	}
	if (!B.doc)
		return;
	char dec[128];
	size_t k = 0;
	for (size_t i = 0; id[i] && k < sizeof dec - 1; i++) { // %20 y demás
		if (id[i] == '%' && isxdigit((unsigned char)id[i + 1]) && isxdigit((unsigned char)id[i + 2])) {
			char hx[3] = {id[i + 1], id[i + 2], 0};
			dec[k++] = (char)strtol(hx, NULL, 16);
			i += 2;
		} else
			dec[k++] = id[i];
	}
	dec[k] = 0;
	dom_string *ds = NULL;
	dom_element *el = NULL;
	if (dom_string_create((const uint8_t *)dec, k, &ds) != DOM_NO_ERR)
		return;
	dom_document_get_element_by_id(B.doc, ds, &el);
	dom_string_unref(ds);
	if (!el)
		return;
	float best = -1;
	for (int i = 0; i < B.nf && best < 0; i++)
		for (dom_node_internal *n = (dom_node_internal *)B.f[i].node; n; n = n->parent)
			if ((dom_node *)n == (dom_node *)el) {
				best = B.l[B.f[i].line].y;
				break;
			}
	dom_node_unref(el);
	if (best >= 0) {
		B.sy = best - 8;
		B.vy = 0;
		clamp_view();
	}
}

static void clamp_view(void)
{
	B.sx = clampf(B.sx, 0, fmaxf(0, B.vw - view_w()));
	B.sy = clampf(B.sy, 0, fmaxf(0, B.total - view_h()));
}

// zoom manteniendo quieto lo que hay bajo el puntero
static void zoom_by(float k)
{
	float min = TOP_W / B.vw;
	float nz = clampf(B.zoom * k, min, ZOOM_MAX);
	if (fabsf(nz - B.zoom) < 0.001f)
		return;
	float qx = B.sx + B.px / B.zoom, qy = B.sy + B.py / B.zoom;
	B.zoom = nz;
	B.sx = qx - B.px / B.zoom;
	B.sy = qy - B.py / B.zoom;
	clamp_view();
	B.tip = 1.5f;
}

// el rectángulo de un trozo, en px de página
static void frag_rect(const Frag *f, float *x, float *y, float *w, float *h)
{
	const Line *l = &B.l[f->line];
	*h = text_lineh(f->sc);
	*x = PAD_X + f->x, *y = l->y + (l->h - *h), *w = f->w;
}

// la posición en B.ord del primer trozo que puede verse desde y (B.ord va por altura de línea;
// se empieza un poco antes por si hay líneas muy altas, como imágenes grandes)
static int first_frag(float y)
{
	if (!B.ord)
		return B.nf;
	y -= 400;
	int lo = 0, hi = B.nf;
	while (lo < hi) {
		int m = (lo + hi) / 2;
		if (B.l[B.f[B.ord[m]].line].y < y)
			lo = m + 1;
		else
			hi = m;
	}
	return lo;
}

// el enlace bajo el puntero (o -1)
static int link_at_pointer(void)
{
	float qx = B.sx + B.px / B.zoom, qy = B.sy + B.py / B.zoom, pad = 3 / B.zoom;
	for (int k = first_frag(qy - 60); k < B.nf; k++) {
		const Frag *f = &B.f[B.ord[k]];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		if (B.l[f->line].y > qy + pad)
			break;
		if (f->link >= 0 && qx >= x - pad && qx <= x + w + pad && qy >= y - pad && qy <= y + h + pad)
			return f->link;
	}
	return -1;
}

// el trozo bajo el puntero (o -1): para saber a qué elemento se le hace clic
static int frag_at_pointer(void)
{
	float qx = B.sx + B.px / B.zoom, qy = B.sy + B.py / B.zoom, pad = 3 / B.zoom;
	for (int k = first_frag(qy - 60); k < B.nf; k++) {
		const Frag *f = &B.f[B.ord[k]];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		if (B.l[f->line].y > qy + pad)
			break;
		if (qx >= x - pad && qx <= x + w + pad && qy >= B.l[f->line].y - pad && qy <= B.l[f->line].y + B.l[f->line].h + pad)
			return B.ord[k];
	}
	return -1;
}

// el control de formulario bajo el puntero (o -1)
static int wdg_at_pointer(void)
{
	float qx = B.sx + B.px / B.zoom, qy = B.sy + B.py / B.zoom, pad = 3 / B.zoom;
	for (int k = first_frag(qy - 60); k < B.nf; k++) {
		const Frag *f = &B.f[B.ord[k]];
		if (B.l[f->line].y > qy + pad)
			break;
		if (f->wdg < 0)
			continue;
		float x = PAD_X + f->x, y = B.l[f->line].y + B.l[f->line].h - f->w * 0, w = f->w;
		const Line *l = &B.l[f->line];
		(void)y;
		if (qx >= x - pad && qx <= x + w + pad && qy >= l->y - pad && qy <= l->y + l->h + pad)
			return f->wdg;
	}
	return -1;
}

// tocar un control: escribir, marcar, elegir o enviar
static void wdg_activate(int k)
{
	if (k < 0 || k >= B.nwg)
		return;
	B.page_ver++; // su valor cambia: la caché de la página se repinta
	Wdg *w = &B.wg[k];
	FVal *v = fv_get(w->node, true);
	if (!v)
		return;
	if (w->type == WG_TEXT || w->type == WG_PASS || w->type == WG_AREA) {
		char hint[96] = "", out[1024];
		if (!attr(w->node, "placeholder", hint, sizeof hint) && !attr(w->node, "aria-label", hint, sizeof hint))
			util_strlcpy(hint, "Escribe aquí", sizeof hint);
		if (kb_input(hint, v->value ? v->value : w->label, out, sizeof out, w->type == WG_AREA, w->type == WG_PASS)) {
			free(v->value);
			v->value = strdup(out);
			util_strlcpy(w->label, out, sizeof w->label);
			if (w->type != WG_AREA && w->form) // como pulsar Intro: se envía (una búsqueda, por ejemplo)
				form_submit(w->form, NULL);
		}
	} else if (w->type == WG_CHECK) {
		bool on = v->checked >= 0 ? v->checked : has_attr(w->node, "checked");
		v->checked = !on;
	} else if (w->type == WG_RADIO) {
		char name[128] = "", n2[128];
		attr(w->node, "name", name, sizeof name);
		for (int i = 0; i < B.nwg; i++) // las del mismo grupo se desmarcan
			if (B.wg[i].type == WG_RADIO && B.wg[i].form == w->form && attr(B.wg[i].node, "name", n2, sizeof n2) && !strcmp(n2, name)) {
				FVal *o = fv_get(B.wg[i].node, true);
				if (o)
					o->checked = 0;
			}
		v = fv_get(w->node, true);
		if (v)
			v->checked = 1;
	} else if (w->type == WG_SELECT) { // la siguiente opción
		char lab[96], val[256];
		int n = 0, cur = v->value ? atoi(v->value) : -1;
		if (cur < 0) { // la que estaba marcada
			select_pick(w->node, -1, lab, sizeof lab, val, sizeof val, &n);
			for (int i = 0; i < n; i++) {
				char l2[96], v2[256];
				select_pick(w->node, i, l2, sizeof l2, v2, sizeof v2, NULL);
				if (!strcmp(l2, lab)) {
					cur = i;
					break;
				}
			}
		} else
			select_pick(w->node, 0, lab, sizeof lab, val, sizeof val, &n);
		int nx = n ? (cur + 1) % n : 0;
		char num[12];
		snprintf(num, sizeof num, "%d", nx);
		free(v->value);
		v->value = strdup(num);
		select_pick(w->node, nx, w->label, sizeof w->label, val, sizeof val, NULL);
	} else if (w->type == WG_BUTTON) {
		char type[16] = "submit";
		attr(w->node, "type", type, sizeof type);
		if (w->form && strcasecmp(type, "button") && strcasecmp(type, "reset"))
			form_submit(w->form, w->node);
	}
}

// cruceta ◀ ▶: al enlace anterior o siguiente; el puntero se pone encima y la vista lo sigue
static void focus_step(int dir)
{
	int i = B.focus;
	if (i < 0) { // el primero que se vea
		int k = first_frag(B.sy + 400);
		i = k < B.nf ? B.ord[k] : B.nf;
		if (dir < 0)
			i--;
	} else
		i += dir;
#define FKEY(f) ((f).wdg >= 0 ? 100000 + (f).wdg : (f).link >= 0 ? (f).link : (f).clk >= 0 ? 200000 + (f).clk : -1)
	int cur = B.focus >= 0 ? FKEY(B.f[B.focus]) : -2;
	for (; i >= 0 && i < B.nf; i += dir)
		if (FKEY(B.f[i]) >= 0 && FKEY(B.f[i]) != cur)
			break;
	if (i < 0 || i >= B.nf)
		return;
	if (dir < 0) // al principio de ese enlace, no a su última palabra
		while (i > 0 && FKEY(B.f[i - 1]) == FKEY(B.f[i]))
			i--;
	B.focus = i;
	float x, y, w, h;
	frag_rect(&B.f[i], &x, &y, &w, &h);
	if (y < B.sy || y + h > B.sy + view_h())
		B.sy = y - view_h() / 3;
	if (x < B.sx || x + w > B.sx + view_w())
		B.sx = x - 20 / B.zoom;
	clamp_view();
	B.px = (x + fminf(w, 40) / 2 - B.sx) * B.zoom;
	B.py = (y + h / 2 - B.sy) * B.zoom;
}

static void draw_pointer(float x, float y, bool on_link)
{
	if (on_link) { // una mano: un círculo con borde
		draw_circle(x, y, 7, col_alpha(PG_LINK, 0.25f));
		draw_ring(x, y, 7, 1.5f, PG_LINK);
		draw_circle(x, y, 2, PG_LINK);
		return;
	}
	// flecha
	draw_tri(x + 1, y + 1, x + 1, y + 17, x + 12, y + 12, col_alpha(0xFF000000, 0.35f));
	draw_tri(x, y, x, y + 16, x + 11, y + 11, 0xFF000000);
	draw_tri(x + 1.5f, y + 3.5f, x + 1.5f, y + 13, x + 8, y + 10, 0xFFFFFFFF);
}

static const char *host_of(const char *u, char *out, size_t n)
{
	const char *h = strstr(u, "://");
	if (!h) { // un archivo de la SD: su nombre
		const char *sl = strrchr(u, '/');
		snprintf(out, n, "%s", sl ? sl + 1 : u);
		return out;
	}
	h += 3;
	size_t l = strcspn(h, "/?#");
	snprintf(out, n, "%.*s", (int)(l < n - 1 ? l : n - 1), h);
	return out;
}

/* la página en un rectángulo de VW x VH desde (B.sx, B.sy): mode 0 todo, 1 sin resaltados (para la
 * caché), 2 solo los resaltados (el enlace del puntero, lo marcado), encima de la caché */
static void page_paint(float VW, float VH, int mode, int hov)
{
	const float z = B.zoom;
	if (mode != 2) {
	draw_rect(0, 0, VW, VH, B.page_bg ? B.page_bg : PG_BG);
	// reglas <hr>
	for (int i = 0; i < B.nl; i++) {
		const Line *l = &B.l[i];
		if (!l->hr)
			continue;
		float y = (l->y + l->h / 2 - B.sy) * z;
		if (y >= 0 && y < VH)
			draw_rect((PAD_X - B.sx) * z, y, PAGE_W * z, fmaxf(1, z), PG_RULE);
	}
	// fondos de color de los bloques
	for (int i = 0; i < B.nbg; i++) {
		const BgBox *bb = &B.bg[i];
		float Y0 = (bb->y0 - B.sy) * z, Y1 = (bb->y1 - B.sy) * z;
		if (Y1 < 0 || Y0 > VH)
			continue;
		float X0 = (PAD_X + bb->x0 - B.sx) * z, Wb = (bb->x1 - bb->x0) * z;
		if (bb->col)
			draw_rect(X0, Y0, Wb, Y1 - Y0, bb->col);
		float t;
		if ((t = bb->bw[0] * z) > 0) draw_rect(X0, Y0, Wb, fmaxf(t, 1), bb->bc[0]);
		if ((t = bb->bw[2] * z) > 0) draw_rect(X0, Y1 - fmaxf(t, 1), Wb, fmaxf(t, 1), bb->bc[2]);
		if ((t = bb->bw[3] * z) > 0) draw_rect(X0, Y0, fmaxf(t, 1), Y1 - Y0, bb->bc[3]);
		if ((t = bb->bw[1] * z) > 0) draw_rect(X0 + Wb - fmaxf(t, 1), Y0, fmaxf(t, 1), Y1 - Y0, bb->bc[1]);
	}
	}
	for (int k = first_frag(B.sy); k < B.nf; k++) {
		int i = B.ord[k];
		const Frag *f = &B.f[i];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		float X = (x - B.sx) * z, Y = (y - B.sy) * z, Wd = w * z, H = h * z;
		if ((B.l[f->line].y - B.sy) * z > VH)
			break;
		if (X > VW || X + Wd < 0 || Y + H < 0)
			continue;
		if (mode != 1 && ((f->link >= 0 && (f->link == hov || i == B.focus)) || (f->clk >= 0 && B.focus >= 0 && f->clk == B.f[B.focus].clk)))
			draw_rect(X - 1, Y, Wd + 2, H, col_alpha(PG_LINK, 0.18f));
		if (f->wdg >= 0 && f->wdg < B.nwg) { // control de formulario
			const Wdg *w = &B.wg[f->wdg];
			const Line *l = &B.l[f->line];
			float bh = (w->type == WG_CHECK || w->type == WG_RADIO) ? text_lineh(f->sc) : (w->type == WG_AREA ? l->h : text_lineh(f->sc) * 1.5f);
			float by = (l->y + l->h - bh - B.sy) * z, BH = bh * z;
			u32 border = col_hex(0xA2A9B1);
			FVal *v = fv_get(w->node, false);
			if (mode == 2) {
			} else if (w->type == WG_CHECK || w->type == WG_RADIO) {
				bool on = v && v->checked >= 0 ? v->checked : has_attr(w->node, "checked");
				float r = BH / 2;
				if (w->type == WG_RADIO) {
					draw_circle(X + r, by + r, r, border);
					draw_circle(X + r, by + r, r - fmaxf(1, z), col_hex(0xFFFFFF));
					if (on)
						draw_circle(X + r, by + r, r * 0.5f, PG_LINK);
				} else {
					draw_rect(X, by, BH, BH, on ? PG_LINK : border);
					if (!on)
						draw_rect(X + fmaxf(1, z), by + fmaxf(1, z), BH - 2 * fmaxf(1, z), BH - 2 * fmaxf(1, z), col_hex(0xFFFFFF));
					else
						draw_icon(IC_CHECK, X + r, by + r, BH * 0.8f, 0xFFFFFFFF);
				}
			} else {
				bool btn = w->type == WG_BUTTON;
				draw_rrect(X, by, Wd, BH, fminf(4 * z, BH / 2), border);
				draw_rrect(X + fmaxf(1, z * 0.8f), by + fmaxf(1, z * 0.8f), Wd - 2 * fmaxf(1, z * 0.8f), BH - 2 * fmaxf(1, z * 0.8f), fminf(3 * z, BH / 2),
				           btn ? col_hex(0xF8F9FA) : col_hex(0xFFFFFF));
				char shown[96];
				const char *t = w->label;
				u32 tc = PG_TEXT;
				if (w->type == WG_PASS && t[0]) {
					size_t n = strlen(t);
					memset(shown, '*', n < 95 ? n : 95);
					shown[n < 95 ? n : 95] = 0;
					t = shown;
				}
				if (!t[0] && !btn && w->type != WG_SELECT) { // vacío: lo que la página sugiere
					if (attr(w->node, "placeholder", shown, sizeof shown) || attr(w->node, "aria-label", shown, sizeof shown))
						t = shown, tc = PG_MUTED;
				}
				float sc = f->sc * z, pad = 4 * z;
				if (sc >= 0.17f && t[0]) {
					float ty = by + (fminf(BH, text_lineh(f->sc) * 1.5f * z) - text_lineh(sc)) / 2;
					if (btn)
						text_draw_center_clip(t, X + Wd / 2, ty, Wd - 2 * pad, sc, tc);
					else
						text_draw_clip(t, X + pad, ty, Wd - 2 * pad - (w->type == WG_SELECT ? 10 * z : 0), sc, tc);
				}
				if (w->type == WG_SELECT) {
					float ax = X + Wd - 9 * z, ay = by + BH / 2;
					draw_tri(ax - 4 * z, ay - 2 * z, ax + 4 * z, ay - 2 * z, ax, ay + 3 * z, PG_MUTED);
				}
			}
			if (mode != 1 && f->wdg == B.hw)
				draw_rect(X, by, Wd, BH, col_alpha(PG_LINK, 0.18f));
			continue;
		}
		if (f->img >= 0 && f->img < B.nimg) { // imagen: encajada en su sitio sin deformarla
			PImg *im = &B.img[f->img];
			float bw = Wd, bh = im->h * z, by = (B.l[f->line].y + B.l[f->line].h - im->h - B.sy) * z;
			if (mode == 2) {
			} else if (im->st == IM_TEX && im->tex.ok) {
				float k = fminf(bw / im->tw, bh / im->th), dw = im->tw * k, dh = im->th * k;
				draw_texture(&im->tex, X + (bw - dw) / 2, by + (bh - dh) / 2, dw, dh, 1);
			} else
				draw_rect(X, by, bw, bh, im->st == IM_FAIL ? col_hex(0xF2F2F2) : col_hex(0xEAECF0));
			if (mode != 1 && f->link >= 0 && (f->link == hov || i == B.focus))
				draw_rect(X, by, bw, bh, col_alpha(PG_LINK, 0.25f));
			continue;
		}
		if (mode == 2)
			continue;
		float sc = f->sc * z;
		if (sc < 0.17f) // tan pequeño que no se leería: una raya del color del texto (como una vista previa)
			draw_rect(X, Y + H * 0.35f, Wd, fmaxf(1, H * 0.35f), col_alpha(f->col, 0.55f));
		else {
			text_draw_n(B.text + f->off, f->n, X, Y, sc, f->col);
			if (f->bold) // la fuente no tiene negrita: otra vez, un poco a la derecha
				text_draw_n(B.text + f->off, f->n, X + fmaxf(0.5f, z * 0.6f), Y, sc, f->col);
			if (f->under)
				draw_rect(X, Y + H - fmaxf(1, z * 0.8f), Wd, fmaxf(1, z * 0.6f), col_alpha(f->col, 0.5f));
		}
	}
}

/* la página ya pintada en una textura algo más grande que la pantalla: mover el puntero o
 * desplazarse un poco solo mueve la textura (repintar miles de palabras en cada fotograma es lo que
 * hacía ir a tirones el touchpad). Se repinta al salirse de ella o si cambia algo */
#define PC_W 512
#define PC_H 512
static GfxRT PC;
static bool PC_tried;
static struct {
	bool valid;
	float x0, y0, zoom; /* origen en la página y zoom con que se pintó */
	u32 ver;
} PCS;


static void page_draw(int hov)
{
	const float z = B.zoom;
	if (!PC_tried) {
		PC_tried = true;
		gfx_rt_init(&PC, PC_W, PC_H);
	}
	if (!PC.ok) { // sin textura (VRAM llena): como siempre
		page_paint(TOP_W, SCR_H, 0, hov);
		return;
	}
	float mx = (PC_W - TOP_W) / 2.0f, my = (PC_H - SCR_H) / 2.0f;
	float dx = (B.sx - PCS.x0) * z, dy = (B.sy - PCS.y0) * z;
	bool ok = PCS.valid && PCS.zoom == z && PCS.ver == B.page_ver && dx >= 0 && dy >= 0 && dx + TOP_W <= PC_W && dy + SCR_H <= PC_H;
	if (!ok) {
		float sx = B.sx, sy = B.sy;
		PCS.x0 = B.sx - floorf(mx) / z, PCS.y0 = B.sy - floorf(my) / z;
		B.sx = PCS.x0, B.sy = PCS.y0;
		gfx_rt_begin(&PC, B.page_bg ? B.page_bg : PG_BG);
		bool ell = g_clip_ell;
		g_clip_ell = true; // sin recortes (las tijeras van por pantalla, no por textura)
		page_paint(PC_W, PC_H, 1, -1);
		g_clip_ell = ell;
		gfx_resume(g_top);
		B.sx = sx, B.sy = sy;
		PCS.valid = true, PCS.zoom = z, PCS.ver = B.page_ver;
		dx = (B.sx - PCS.x0) * z, dy = (B.sy - PCS.y0) * z;
	}
	gfx_rt_draw(&PC, -floorf(dx + 0.5f), -floorf(dy + 0.5f));
	page_paint(TOP_W, SCR_H, 2, hov);
}

static void home_top(void);
static void oom_top(void);
void br_top(void)
{
	if (B.home) {
		home_top();
		return;
	}
	if (B.st == ST_OOM) {
		oom_top();
		return;
	}
	if (B.st != ST_READY && B.st != ST_ACT) {
		draw_rect(0, 0, TOP_W, SCR_H, T.bg);
		const float X = 24, W = TOP_W - 48;
		char host[128];
		host_of(B.url, host, sizeof host);
		draw_icon(IC_GLOBE, TOP_W / 2, 70, 30, B.st == ST_ERROR ? T.danger : T.accent);
		text_draw_center_clip(host, TOP_W / 2, 98, W, TS_TITLE, T.text);
		if (B.st == ST_ERROR) {
			text_draw_center(B.err, TOP_W / 2, 126, TS_SUB, T.danger);
		} else {
			B.t_load += 1.0f / 60;
			draw_arc(TOP_W / 2, 140, 10, 2.5f, B.t_load * 6, B.t_load * 6 + 4.2f, T.accent);
			text_draw_center(B.st == ST_LAYOUT ? "Colocando la página…" : B.in_js ? (B.cancel ? "Parando el JavaScript…" : "Ejecutando el JavaScript de la página…") : "Cargando…",
			                 TOP_W / 2, 158, TS_SUB, T.text3);
			if (B.in_js && !B.cancel)
				text_draw_center("B: saltárselo y ver la página ya", TOP_W / 2, 200, TS_SUB, T.text3);
			if (B.st == ST_LAYOUT && B.nel_total > 0) {
				float f = clampf((float)B.nel_done / B.nel_total, 0, 1);
				draw_rrect(TOP_W / 2 - 80, 180, 160, 5, 2.5f, T.surface2);
				draw_rrect(TOP_W / 2 - 80, 180, fmaxf(5, 160 * f), 5, 2.5f, T.accent);
			}
		}
		(void)X;
		return;
	}
	// texturas de las imágenes ya decodificadas (pocas por fotograma, para no dar tirones)
	for (int i = 0, up = 0; i < B.nimg && up < 2; i++)
		if (B.img[i].st == IM_RGBA) {
			bool ok = tex_from_rgba(&B.img[i].tex, B.img[i].rgba, B.img[i].tw, B.img[i].th);
			free(B.img[i].rgba);
			B.img[i].rgba = NULL;
			B.img[i].st = ok ? IM_TEX : IM_FAIL;
			up++;
			B.page_ver++;
		}
	int hov = link_at_pointer();
	B.hover = hov;
	B.hw = wdg_at_pointer();
	page_draw(hov);
	const float z = B.zoom;
	// barra de desplazamiento fina a la derecha
	if (B.total > view_h()) {
		float bh = fmaxf(16, SCR_H * view_h() / B.total), by = (SCR_H - bh) * (B.sy / (B.total - view_h()));
		draw_rrect(TOP_W - 5, by, 3, bh, 1.5f, col_alpha(0xFF000000, 0.3f));
	}
	// arriba: la web, PC / móvil y el zoom (un momento, al cambiar)
	char host[96], chip[160];
	host_of(B.url, host, sizeof host);
	snprintf(chip, sizeof chip, "%s%s · %d %%", host, B.light ? "   Ligero" : "", (int)(z * 100 + 0.5f));
	float cw = fminf(TOP_W - 16, text_width(chip, 0.40f) + 20);
	B.tip = fmaxf(0, B.tip - 1.0f / 60);
	float ca = clampf(B.tip / 0.4f, 0, 1); // se ve al abrir y al hacer zoom, y se va sola para no tapar la página
	// franja: la página se ve sin terminar (memoria) o en modo lectura
	const char *strip = NULL;
	bool reader = !strcmp(B.reader_url, B.url);
	if (B.oom == OOM_JS)
		strip = "Memoria llena: parte de su JavaScript no se ejecutó";
	else if (B.oom)
		strip = "Memoria llena: la página se ve hasta donde cupo";
	else if (reader)
		strip = "Modo lectura: solo el texto y los enlaces";
	float cy = 6;
	if (strip) {
		u32 sc = B.oom ? T.warn : T.accent;
		draw_rect(0, 0, TOP_W, 17, 0xF01A1F2A);
		draw_rect(0, 16, TOP_W, 1, col_alpha(sc, 0.8f));
		draw_icon(B.oom ? IC_INFO : IC_BOOK, 12, 8.5f, 10, sc);
		text_draw_clip(strip, 24, 8.5f - text_lineh(0.38f) / 2, TOP_W - 32, 0.38f, 0xFFFFFFFF);
		cy = 22;
	}
	if (ca > 0.01f) {
		draw_rrect(8, cy, cw, 18, 9, col_alpha(0xFF1A1F2A, 0.85f * ca));
		text_draw_clip(chip, 18, cy + 9 - text_lineh(0.40f) / 2, cw - 20, 0.40f, col_alpha(0xFFFFFFFF, ca));
	}
	draw_pointer(B.px, B.py, hov >= 0);
}

static void go_link(int k)
{
	if (k < 0 || k >= B.nlinks)
		return;
	char url[1024];
	util_strlcpy(url, B.links[k], sizeof url);
	go(url, true);
}

// botón redondo de la barra de herramientas; true si se tocó
static bool tool_btn(float x, float y, float w, const char *label, bool on)
{
	bool p = touch_in(x, y, w, 24);
	draw_rrect(x, y, w, 24, 12, on ? T.accent : p ? T.surface2 : T.surface);
	text_draw_center(label, x + w / 2, y + 12 - text_lineh(TS_SUB) / 2, TS_SUB, on ? 0xFFFFFFFF : T.text);
	return tap(x, y, w, 24);
}

#define TP_Y   62.0f   /* touchpad */
#define TP_B   (SCR_H - 34.0f)
#define STRIP  30.0f   /* franja de desplazamiento, a la derecha */

// maquetar, en un hilo con pila grande: libcss usa unos 10 KB de pila por elemento y la del hilo
// principal no da para páginas grandes; así además la pantalla no se queda parada
// su JavaScript se quedó sin memoria: se suelta (la página sigue como está; los clics, normales)
static void js_oom_check(const JsResult *r)
{
	if (!r->oom || !B.jsp)
		return;
	util_log("web: el JavaScript se quedó sin memoria (%u KB)", (unsigned)(r->mem / 1024));
	jsdom_close(B.jsp);
	B.jsp = NULL;
	B.idle_left = 0;
	if (B.oom == OOM_NONE)
		B.oom = OOM_JS, B.oom_need = (u32)(r->mem * 2);
}

static void layout_thread(void *arg)
{
	(void)arg;
	layout();
	if (B.lay_oom) { // la memoria no dio para colocarla entera
		util_log("web: memoria llena colocando la página (%d líneas)", B.nl);
		if (B.oom == OOM_NONE)
			B.oom = OOM_LAYOUT, B.oom_need = (u32)(B.bytes * MEM_PER_HTML);
		if (B.nl < 4) { // no se ve casi nada: mejor el aviso
			B.st = ST_OOM;
			return;
		}
	}
	B.hover = -1;
	if (B.back_sy > 0) { // se vuelve atrás: donde se estaba
		B.sy = B.back_sy;
		B.back_sy = 0;
		clamp_view();
	} else if (B.anchor[0]) // dirección con #ancla
		scroll_to_anchor(B.anchor);
	B.anchor[0] = 0;
	B.idle_left = B.jsp ? 30000 : 0;
	B.idle_next = util_ms() + 600;
	B.idle_relayouts = 0;
	B.idle_dirty = false;
	B.ready_ms = util_ms();
	B.st = ST_READY;
	img_start();
}

// un clic con el JavaScript de la página: sus eventos y, si cambió algo, se vuelve a colocar
static dom_node *ACT_EL;
static int ACT_LINK;
static void act_thread(void *arg)
{
	(void)arg;
	browser_thread_net();
	JsResult r;
	bool changed = false;
	bool go_on = jsdom_click(B.jsp, ACT_EL, &changed, &r);
	js_oom_check(&r);
	if (B.idle_dirty)
		changed = true, B.idle_dirty = false;
	if (r.navigate[0])
		util_strlcpy(B.js_nav, r.navigate, sizeof B.js_nav);
	if (changed) { // se coloca otra vez, sin mover la vista
		float z = B.zoom, sx = B.sx, sy = B.sy, px = B.px, py = B.py;
		dom_node *fn = B.focus >= 0 && B.focus < B.nf ? B.f[B.focus].node : NULL;
		B.st = ST_LAYOUT;
		brcss_forget((dom_node *)B.doc);
		int keep = B.idle_left;
		layout();
		B.idle_left = keep;
		B.zoom = z, B.sx = sx, B.sy = sy, B.px = px, B.py = py;
		for (int i = 0; fn && i < B.nf; i++) // lo marcado sigue marcado
			if (B.f[i].node == fn) {
				B.focus = i;
				break;
			}
		clamp_view();
		B.tip = 0;
		img_start();
	}
	B.click_link = go_on ? ACT_LINK : -1;
	B.st = ST_READY;
}

// lo que la página dejó para después, con ella ya a la vista: un rato corto; si cambia, se recoloca
static void idle_thread(void *arg)
{
	(void)arg;
	browser_thread_net();
	JsResult r;
	bool changed = false;
	u64 t0 = util_ms();
	bool more = jsdom_idle(B.jsp, 600, &changed, &r);
	js_oom_check(&r);
	B.idle_left -= (int)(util_ms() - t0) + 1;
	if (!more)
		B.idle_left = 0;
	if (r.navigate[0])
		util_strlcpy(B.js_nav, r.navigate, sizeof B.js_nav);
	// recolocar para el JavaScript de fondo para la pantalla un momento: solo una vez, nada más
	// cargar y sin que se haya tocado nada; lo demás se aplica con el próximo clic
	if (changed && (B.idle_relayouts >= 1 || B.last_in > B.ready_ms || util_ms() - B.ready_ms > 5000))
		B.idle_dirty = true, changed = false;
	if (changed) {
		B.idle_relayouts++;
		float z = B.zoom, sx = B.sx, sy = B.sy, px = B.px, py = B.py;
		dom_node *fn = B.focus >= 0 && B.focus < B.nf ? B.f[B.focus].node : NULL;
		B.st = ST_LAYOUT;
		brcss_forget((dom_node *)B.doc);
		int keep = B.idle_left;
		layout();
		B.idle_left = keep;
		B.zoom = z, B.sx = sx, B.sy = sy, B.px = px, B.py = py;
		for (int i = 0; fn && i < B.nf; i++)
			if (B.f[i].node == fn) {
				B.focus = i;
				break;
			}
		clamp_view();
		B.tip = 0;
		img_start();
	}
	B.idle_next = util_ms() + 400;
	B.st = ST_READY;
}

static void start_action(dom_node *el, int link)
{
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	ACT_EL = el, ACT_LINK = link;
	B.click_link = -2;
	B.st = ST_ACT;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thr = threadCreate(act_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
	if (!B.thr) { // sin hilo: el clic sigue como un enlace normal
		B.st = ST_READY;
		B.click_link = link;
	}
}

/* ------------------------------------------------------------ aviso de memoria */
static bool is_new3ds(void)
{
	bool n = false;
	APT_CheckNew3DS(&n);
	return n;
}

static void oom_reader(void)
{
	util_strlcpy(B.reader_url, B.url, sizeof B.reader_url);
	char u[1024];
	util_strlcpy(u, B.url, sizeof u);
	start_load(u);
}

static void oom_top(void)
{
	draw_rect(0, 0, TOP_W, SCR_H, T.bg);
	// la dirección, arriba
	draw_rect(0, 0, TOP_W, 20, T.surface);
	char host[160];
	const char *u = strstr(B.url, "://");
	util_strlcpy(host, u ? u + 3 : B.url, sizeof host);
	g_clip_ell = true;
	text_draw_clip(host, 10, 10 - text_lineh(0.38f) / 2, TOP_W - 20, 0.38f, T.text3);
	g_clip_ell = false;
	const float LX = 18, LW = 236, QX = 266, QW = 118;
	draw_circle(LX + 14, 46, 14, col_alpha(T.warn, 0.18f));
	draw_icon(IC_INFO, LX + 14, 46, 15, T.warn);
	float y = 68;
	int n = text_wrap_lines("Esta página es demasiado grande para la memoria de la consola", LW, 0.52f, 3, LX, y, T.text, true);
	y += n * text_lineh(0.52f) + 8;
	char msg[300];
	unsigned have = (unsigned)(envGetHeapSize() / (1024 * 1024)), need = (unsigned)((B.oom_need + 1024 * 1024 - 1) / (1024 * 1024));
	if (need <= have)
		need = have + 1;
	if (is_new3ds())
		snprintf(msg, sizeof msg, "Necesitaba unos %u MB y Cloud3DS dispone de unos %u MB en esta New 3DS.", need, have);
	else
		snprintf(msg, sizeof msg, "Necesitaba unos %u MB y Cloud3DS dispone de unos %u MB en esta consola (en una New 3DS, unos 88 MB).", need, have);
	n = text_wrap_lines(msg, LW, 0.40f, 4, LX, y, T.text2, true);
	y += n * text_lineh(0.40f) + 8;
	text_wrap_lines("Se paró antes de que la consola se quedara sin memoria: la app sigue funcionando.", LW, 0.38f, 3, LX, y, T.text3, true);
	// el QR, para abrirla en el móvil
	draw_rrect(QX, 34, QW, 168, 14, T.surface);
	if (strstr(B.url, "://")) {
		draw_qr(B.url, QX + 9, 44, QW - 18, 1);
		text_draw_center("Ábrela en el móvil", QX + QW / 2, 44 + QW - 18 + 12, 0.38f, T.text2);
	} else { // un archivo de la SD: no hay enlace que abrir en el móvil
		draw_icon(IC_FILE, QX + QW / 2, 100, 30, T.text3);
		text_draw_center("Archivo de la SD", QX + QW / 2, 132, 0.38f, T.text2);
	}
}

static void oom_bottom(void)
{
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	draw_rect(0, 0, BOT_W, HDR_H, T.surface);
	draw_rect(0, HDR_H - 1, BOT_W, 1, T.divider);
	text_draw_center("No se pudo abrir del todo", BOT_W / 2, HDR_H / 2 - text_lineh(0.46f) / 2, 0.46f, T.text);
	static int sel;
	bool web = strstr(B.url, "://") != NULL;
	const char *lab[3] = {"Abrir en el móvil", "Modo lectura", "Volver"};
	const char *sub[3] = {"Escanea el QR de arriba con la cámara del móvil", "Solo el texto y los enlaces, sin ejecutar nada", B.nhist || B.came_home ? "A la página anterior" : "Cerrar el navegador"};
	IconId ic[3] = {IC_QR, IC_BOOK, IC_CHEV_LEFT};
	u32 col[3] = {T.accent, T.ok, T.text2};
	if (g_krepeat & KEY_DOWN)
		sel = (sel + 1) % 3;
	if (g_krepeat & KEY_UP)
		sel = (sel + 2) % 3;
	if (!web && sel == 0)
		sel = 1;
	int run = -1;
	for (int i = 0; i < 3; i++) {
		float y = HDR_H + 12 + i * 46;
		bool dis = i == 0 && !web;
		bool hl = sel == i || touch_in(10, y, BOT_W - 20, 42);
		if (hl && !dis)
			draw_rrect(10, y, BOT_W - 20, 42, 12, T.surface2);
		draw_rrect(20, y + 7, 28, 28, 9, col_alpha(col[i], dis ? 0.06f : 0.16f));
		draw_icon(ic[i], 34, y + 21, 14, dis ? T.text3 : col[i]);
		text_draw(lab[i], 58, y + 6, 0.46f, dis ? T.text3 : T.text);
		text_draw_clip(sub[i], 58, y + 23, BOT_W - 80, 0.36f, T.text3);
		if (!dis && tap(10, y, BOT_W - 20, 42))
			run = i, sel = i;
	}
	char keys[80];
	snprintf(keys, sizeof keys, "%s Elegir · %s Volver", GLYPH_A, GLYPH_B);
	text_draw_center(keys, BOT_W / 2, SCR_H - 16, 0.36f, T.text3);
	if (g_kdown & KEY_A)
		run = sel;
	if (g_kdown & KEY_B)
		run = 2;
	if (g_kdown & KEY_START) {
		close_br();
		g_kdown = 0;
		return;
	}
	g_kdown = 0;
	g_touch.up = false;
	if (run == 0)
		toast_icon("Escanea el QR de la pantalla de arriba", IC_QR, T.accent);
	else if (run == 1) {
		sel = 0;
		oom_reader();
	} else if (run == 2) {
		sel = 0;
		back();
	}
}

/* ------------------------------------------------------------ inicio, favoritos e historial */
#define TS_ROW 0.44f
enum { TAB_FAV = 1, TAB_HIST = 2 };
enum { OP_OPEN, OP_RENAME, OP_LEFT, OP_RIGHT, OP_UNFAV, OP_ADDFAV, OP_UNHIST, OP_CLEAR };

// la letra y el color de una página, en un cuadrado redondeado
static void site_icon(const char *title, const char *url, float x, float y, float s, bool hl)
{
	u32 c = bm_color(url);
	draw_rrect(x, y, s, s, s * 0.34f, col_alpha(c, 0.16f));
	if (hl)
		draw_rrect_outline(x - 2, y - 2, s + 4, s + 4, s * 0.34f + 2, 1.5f, T.accent);
	char l[2] = {bm_letter(title, url), 0};
	float sc = s >= 30 ? 0.62f : 0.46f;
	text_draw_center(l, x + s / 2, y + s / 2 - text_lineh(sc) / 2, sc, c);
}

// «Hoy», «Ayer» o la fecha, para agrupar el historial
static void day_label(int64_t t, char *out, size_t n)
{
	static const char *const MES[12] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
	time_t now = time(NULL), tt = (time_t)t;
	struct tm a = *gmtime(&now), b = *gmtime(&tt);
	long da = (long)(now / 86400), db = (long)(tt / 86400);
	if (da == db)
		snprintf(out, n, "HOY");
	else if (da - db == 1)
		snprintf(out, n, "AYER");
	else if (a.tm_year == b.tm_year)
		snprintf(out, n, "%d %s", b.tm_mday, MES[b.tm_mon]);
	else
		snprintf(out, n, "%d %s %d", b.tm_mday, MES[b.tm_mon], 1900 + b.tm_year);
	for (char *p = out; *p; p++)
		*p = (char)toupper((unsigned char)*p);
}

static void hist_sub(const BmHist *h, char *out, size_t n, bool ago)
{
	char host[96];
	bm_host(h->url, host, sizeof host);
	time_t now = time(NULL), tt = (time_t)h->t;
	long d = (long)(now - tt);
	if (ago && d >= 0 && d < 3600)
		snprintf(out, n, "%s · hace %ld min", host, d / 60 > 0 ? d / 60 : 1);
	else if (ago && now / 86400 != tt / 86400) {
		char dl[24];
		day_label(h->t, dl, sizeof dl);
		for (char *p = dl; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		snprintf(out, n, "%s · %s", host, dl);
	} else {
		struct tm b = *gmtime(&tt);
		snprintf(out, n, "%s · %02d:%02d", host, b.tm_hour, b.tm_min);
	}
}

// volver a la página de inicio (se suelta la página que hubiera, para tener la memoria libre)
static void home_enter(void)
{
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp);
	B.jsp = NULL;
	if (B.doc) {
		dom_node_unref(B.doc);
		B.doc = NULL;
	}
	brcss_free(B.css);
	B.css = NULL;
	img_free_all();
	fv_free();
	layout_free();
	B.st = ST_IDLE;
	B.url[0] = B.title[0] = B.err[0] = 0;
	B.nhist = 0;
	B.home = true;
	B.panel = 0;
	B.came_home = false;
	B.tab = TAB_FAV;
	B.lsel = 0;
	B.lsy = 0;
	B.opt = false;
	bm_load();
}

void br_open_home(void)
{
	light_load();
	nohc_init();
	B.js = true;
	B.open = true;
	B.force = 0;
	home_enter();
}

// ir a una página desde el inicio o desde la lista
static void open_from_list(const char *url)
{
	bool home = B.home;
	B.home = false;
	B.panel = 0;
	B.opt = false;
	if (home) {
		B.came_home = true;
		B.st = ST_IDLE;
		go(url, false);
	} else
		go(url, true);
}

// lo que se ve en la lista: favoritos (más «Añadir») o historial (con cabeceras por día)
typedef struct {
	int kind;  /* 0 favorito, 1 añadir, 2 cabecera, 3 historial */
	int idx;
	float x, y, w, h;
} LItem;
static LItem LI[BM_HIST_MAX * 2 + BM_FAV_MAX + 2];
static int NLI;

#define LS_Y0 (HDR_H + 32.0f)
#define LS_Y1 (SCR_H - 20.0f)

static void list_build(void)
{
	NLI = 0;
	if (B.tab == TAB_FAV) {
		const float cw = (BOT_W - 16) / 4.0f, ch = 60;
		int n = bm_nfav();
		for (int i = 0; i <= n; i++) {
			if (i == n && n >= BM_FAV_MAX)
				break;
			LItem *it = &LI[NLI++];
			it->kind = i < n ? 0 : 1;
			it->idx = i;
			it->x = 8 + (i % 4) * cw, it->y = 6 + (i / 4) * ch, it->w = cw, it->h = ch;
		}
	} else {
		float y = 4;
		long last = -1;
		for (int i = 0; i < bm_nhist(); i++) {
			const BmHist *h = bm_hist(i);
			long d = (long)(h->t / 86400);
			if (d != last) {
				last = d;
				LItem *it = &LI[NLI++];
				*it = (LItem){2, i, 8, y, BOT_W - 16, 18};
				y += 18;
			}
			LItem *it = &LI[NLI++];
			*it = (LItem){3, i, 8, y, BOT_W - 16, 32};
			y += 32;
		}
	}
}

static float list_h(void)
{
	float h = 0;
	for (int i = 0; i < NLI; i++)
		h = fmaxf(h, LI[i].y + LI[i].h);
	return h + 6;
}

static bool list_pickable(int i)
{
	return i >= 0 && i < NLI && LI[i].kind != 2;
}

// lleva la vista hasta lo marcado con la cruceta
static void list_show_sel(void)
{
	if (!list_pickable(B.lsel))
		return;
	const LItem *it = &LI[B.lsel];
	float vh = LS_Y1 - LS_Y0;
	if (it->y - B.lsy < 0)
		B.lsy = it->y - (B.lsel > 0 && LI[B.lsel - 1].kind == 2 ? 18 : 0);
	if (it->y + it->h - B.lsy > vh)
		B.lsy = it->y + it->h - vh;
}

static void opt_open(int li)
{
	if (!list_pickable(li) || LI[li].kind == 1)
		return;
	B.opt = true;
	B.opt_li = li;
	B.opt_sel = 0;
	B.opt_confirm = false;
	B.opt_just = true;
}

static void add_by_address(void)
{
	char nu[1024], full[1024];
	if (!kb_input("Dirección para añadir a favoritos", "https://", nu, sizeof nu, false, false) || strlen(nu) < 3 || !strcmp(nu, "https://"))
		return;
	address_to_url(nu, full, sizeof full);
	B.search_q[0] = 0;
	if (!bm_add_fav(NULL, full))
		toast_icon("Ya no caben más favoritos", IC_CLOSE, T.danger);
	else
		toast_icon("Añadido a favoritos", IC_STAR, T.warn);
}

// la hoja de opciones de un favorito o de una visita
static void opt_draw_update(void)
{
	const LItem *it = &LI[B.opt_li];
	bool fav = it->kind == 0;
	const char *title, *url;
	if (fav) {
		const BmFav *f = bm_fav(it->idx);
		if (!f) {
			B.opt = false;
			return;
		}
		title = f->title, url = f->url;
	} else {
		const BmHist *h = bm_hist(it->idx);
		if (!h) {
			B.opt = false;
			return;
		}
		title = h->title, url = h->url;
	}
	int ops[6], n = 0;
	ops[n++] = OP_OPEN;
	if (fav) {
		ops[n++] = OP_RENAME;
		if (it->idx > 0)
			ops[n++] = OP_LEFT;
		if (it->idx < bm_nfav() - 1)
			ops[n++] = OP_RIGHT;
		ops[n++] = OP_UNFAV;
	} else {
		ops[n++] = bm_find_fav(url) >= 0 ? OP_UNFAV : OP_ADDFAV;
		ops[n++] = OP_UNHIST;
		ops[n++] = OP_CLEAR;
	}
	const float X = 8, W = BOT_W - 16, RH = 26, HH = 34;
	float h = HH + n * RH + 10, y = SCR_H - 8 - h;
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, 0.9f));
	draw_rrect(X, y + 3, W, h, 14, T.shadow);
	draw_rrect(X, y, W, h, 14, T.surface);
	g_clip_ell = true;
	text_draw_clip(title, X + 12, y + 7, W - 24, TS_ROW, T.text);
	text_draw_clip(url, X + 12, y + 20, W - 24, 0.36f, T.text3);
	g_clip_ell = false;
	bool fresh = B.opt_just;
	B.opt_just = false;
	if (g_krepeat & KEY_DOWN)
		B.opt_sel = (B.opt_sel + 1) % n;
	if (g_krepeat & KEY_UP)
		B.opt_sel = (B.opt_sel + n - 1) % n;
	if (B.opt_sel >= n)
		B.opt_sel = n - 1;
	int run = -1;
	for (int i = 0; i < n; i++) {
		float ry = y + HH + i * RH;
		bool hl = B.opt_sel == i || touch_in(X + 4, ry, W - 8, RH);
		if (hl)
			draw_rrect(X + 4, ry, W - 8, RH - 2, 9, T.surface2);
		const char *lab = "";
		u32 c = T.text;
		switch (ops[i]) {
		case OP_OPEN: lab = "Abrir"; break;
		case OP_RENAME: lab = "Cambiar el nombre"; break;
		case OP_LEFT: lab = "Mover a la izquierda"; break;
		case OP_RIGHT: lab = "Mover a la derecha"; break;
		case OP_UNFAV: lab = "Quitar de favoritos", c = T.danger; break;
		case OP_ADDFAV: lab = "Añadir a favoritos"; break;
		case OP_UNHIST: lab = "Quitar del historial", c = T.danger; break;
		case OP_CLEAR: lab = B.opt_confirm ? "Toca otra vez para borrar todo" : "Borrar todo el historial", c = T.danger; break;
		}
		text_draw(lab, X + 14, ry + (RH - 2) / 2 - text_lineh(TS_ROW) / 2, TS_ROW, c);
		if (!fresh && tap(X + 4, ry, W - 8, RH))
			run = i;
	}
	if (fresh)
		return;
	if (g_kdown & KEY_A)
		run = B.opt_sel;
	if ((g_kdown & KEY_B) || (g_touch.up && !g_touch.drag && !(g_touch.sx >= X && g_touch.sx < X + W && g_touch.sy >= y && g_touch.sy < y + h))) {
		B.opt = false;
		return;
	}
	if (run < 0)
		return;
	char u[400];
	util_strlcpy(u, url, sizeof u);
	switch (ops[run]) {
	case OP_OPEN:
		open_from_list(u);
		return;
	case OP_RENAME: {
		char nn[96];
		if (kb_input("Nombre del favorito", title, nn, sizeof nn, false, false) && nn[0])
			bm_rename_fav(it->idx, nn);
		break;
	}
	case OP_LEFT:
		bm_move_fav(it->idx, it->idx - 1);
		B.lsel = B.opt_li - 1;
		break;
	case OP_RIGHT:
		bm_move_fav(it->idx, it->idx + 1);
		B.lsel = B.opt_li + 1;
		break;
	case OP_UNFAV:
		bm_del_fav(bm_find_fav(u));
		toast_icon("Quitado de favoritos", IC_STAR, T.text2);
		break;
	case OP_ADDFAV:
		if (bm_add_fav(title, u))
			toast_icon("Añadido a favoritos", IC_STAR, T.warn);
		else
			toast_icon("Ya no caben más favoritos", IC_CLOSE, T.danger);
		break;
	case OP_UNHIST:
		bm_del_hist(it->idx);
		break;
	case OP_CLEAR:
		if (!B.opt_confirm) {
			B.opt_confirm = true;
			return;
		}
		bm_clear_hist();
		toast_icon("Historial borrado", IC_TRASH, T.text2);
		break;
	}
	B.opt = false;
}

// pantalla de abajo con los favoritos o el historial: en el inicio, o encima del touchpad
static void lists_bottom(void)
{
	bm_load();
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	// cabecera: atrás, dirección (o búsqueda) y cerrar
	draw_rect(0, 0, BOT_W, HDR_H, T.surface);
	draw_rect(0, HDR_H - 1, BOT_W, 1, T.divider);
	bool can_back = !B.home;
	bool bp = touch_in(0, 0, 36, HDR_H);
	draw_circle(18, HDR_H / 2, 11, bp && can_back ? T.surface2 : T.surface);
	draw_icon(IC_CHEV_LEFT, 18, HDR_H / 2, 13, can_back ? T.text : T.text3);
	bool cp2 = touch_in(BOT_W - 36, 0, 36, HDR_H);
	draw_circle(BOT_W - 18, HDR_H / 2, 11, cp2 ? T.surface2 : T.surface);
	draw_icon(IC_CLOSE, BOT_W - 18, HDR_H / 2, 12, T.text2);
	draw_rrect(36, 5, BOT_W - 72, HDR_H - 10, (HDR_H - 10) / 2, T.surface2);
	if (B.home) {
		draw_icon(IC_SEARCH, 50, HDR_H / 2, 10, T.text3);
		text_draw_clip("Buscar en Google o escribir dirección", 60, HDR_H / 2 - text_lineh(0.40f) / 2, BOT_W - 106, 0.40f, T.text3);
	} else {
		const char *u = strstr(B.url, "://");
		text_draw_clip(u ? u + 3 : B.url, 46, HDR_H / 2 - text_lineh(0.40f) / 2, BOT_W - 92, 0.40f, T.text2);
	}
	// Favoritos | Historial
	const float SY = HDR_H + 5, SW = (BOT_W - 16) / 2;
	draw_rrect(8, SY, BOT_W - 16, 22, 11, T.surface);
	draw_rrect(8 + (B.tab == TAB_HIST ? SW : 0) + 2, SY + 2, SW - 4, 18, 9, T.surface2);
	text_draw_center("Favoritos", 8 + SW / 2, SY + 11 - text_lineh(TS_SUB) / 2, TS_SUB, B.tab == TAB_FAV ? T.text : T.text3);
	text_draw_center("Historial", 8 + SW * 1.5f, SY + 11 - text_lineh(TS_SUB) / 2, TS_SUB, B.tab == TAB_HIST ? T.text : T.text3);
	int tab = B.tab;
	if (!B.opt) {
		if (tap(8, SY, SW, 22) || (g_kdown & KEY_L))
			tab = TAB_FAV;
		if (tap(8 + SW, SY, SW, 22) || (g_kdown & KEY_R))
			tab = TAB_HIST;
	}
	if (tab != B.tab)
		B.tab = tab, B.lsel = 0, B.lsy = 0;
	list_build();
	// desplazar arrastrando
	float vh = LS_Y1 - LS_Y0, lh = list_h();
	if (!B.opt && g_touch.held && g_touch.drag && g_touch.sy >= LS_Y0 && g_touch.sy < LS_Y1)
		B.lsy -= (float)(g_touch.y - g_touch.py);
	// cruceta
	if (!B.opt && NLI) {
		int d = 0, step = B.tab == TAB_FAV ? 4 : 1;
		if (g_krepeat & KEY_DOWN)
			d = step;
		if (g_krepeat & KEY_UP)
			d = -step;
		if (B.tab == TAB_FAV && (g_krepeat & KEY_RIGHT))
			d = 1;
		if (B.tab == TAB_FAV && (g_krepeat & KEY_LEFT))
			d = -1;
		if (d) {
			B.lkeys = true;
			int s = B.lsel + d;
			while (s >= 0 && s < NLI && !list_pickable(s))
				s += d > 0 ? 1 : -1;
			if (s >= 0 && s < NLI)
				B.lsel = s;
			list_show_sel();
		}
	}
	if (g_touch.down)
		B.lkeys = false;
	if (B.lsel >= NLI)
		B.lsel = NLI - 1;
	if (!list_pickable(B.lsel)) {
		int s = B.lsel < 0 ? 0 : B.lsel;
		while (s < NLI && !list_pickable(s))
			s++;
		B.lsel = s < NLI ? s : 0;
	}
	B.lsy = clampf(B.lsy, 0, fmaxf(0, lh - vh));
	// la lista
	int open = -1, hold = -1;
	g_clip_ell = true;
	gfx_clip(0, LS_Y0, BOT_W, LS_Y1 - LS_Y0);
	for (int i = 0; i < NLI; i++) {
		const LItem *it = &LI[i];
		float y = LS_Y0 + it->y - B.lsy;
		if (y + it->h < LS_Y0 || y > LS_Y1)
			continue;
		bool sel = B.lkeys && i == B.lsel;
		bool vis_tap = !B.opt && g_touch.sy >= LS_Y0 && g_touch.sy < LS_Y1;
		if (it->kind == 0 || it->kind == 1) {
			float cx = it->x + it->w / 2, s = 34;
			bool p = vis_tap && touch_in(it->x, y, it->w, it->h);
			if (it->kind == 0) {
				const BmFav *f = bm_fav(it->idx);
				if (p)
					draw_rrect(it->x + 4, y + 1, it->w - 8, it->h - 4, 12, T.surface);
				site_icon(f->title, f->url, cx - s / 2, y + 6, s, sel);
				text_draw_center_clip(f->title, cx, y + 44, it->w - 8, 0.36f, T.text2);
			} else {
				if (p)
					draw_rrect(it->x + 4, y + 1, it->w - 8, it->h - 4, 12, T.surface);
				draw_rrect_outline(cx - s / 2, y + 6, s, s, 12, 1.2f, sel ? T.accent : T.divider);
				draw_icon(IC_PLUS, cx, y + 6 + s / 2, 14, T.text3);
				text_draw_center("Añadir", cx, y + 44, 0.36f, T.text3);
			}
		} else if (it->kind == 2) {
			char dl[32];
			day_label(bm_hist(it->idx)->t, dl, sizeof dl);
			text_draw(dl, it->x + 8, y + 5, 0.34f, T.text3);
		} else {
			const BmHist *h = bm_hist(it->idx);
			bool p = vis_tap && touch_in(it->x, y, it->w, it->h);
			if (p || sel)
				draw_rrect(it->x, y + 1, it->w, it->h - 2, 9, sel ? T.surface2 : T.surface);
			site_icon(h->title, h->url, it->x + 6, y + 6, 20, false);
			char sub[160];
			hist_sub(h, sub, sizeof sub, false);
			bool fav = bm_find_fav(h->url) >= 0;
			float tw = it->w - 40 - (fav ? 18 : 0);
			text_draw_clip(h->title, it->x + 34, y + 3, tw, TS_ROW * 0.92f, T.text);
			text_draw_clip(sub, it->x + 34, y + 17, tw, 0.34f, T.text3);
			if (fav)
				draw_icon(IC_STAR, it->x + it->w - 14, y + 16, 10, T.warn);
		}
		if (!B.opt && list_pickable(i) && g_touch.sy >= LS_Y0 && g_touch.sy < LS_Y1) {
			if (tap(it->x, y, it->w, it->h))
				open = i;
			if (long_press(it->x, y, it->w, it->h))
				hold = i;
		}
	}
	gfx_clip_off();
	g_clip_ell = false;
	if (!NLI || (B.tab == TAB_HIST && !bm_nhist()))
		text_draw_center(B.tab == TAB_HIST ? "Aún no has visitado ninguna página" : "", BOT_W / 2, LS_Y0 + 40, TS_SUB, T.text3);
	// pie
	char keys[160];
	if (B.home)
		snprintf(keys, sizeof keys, "Toca para abrir · mantén pulsado: opciones · %s Buscar", GLYPH_X);
	else
		snprintf(keys, sizeof keys, "Mantén pulsado: opciones · %s Opciones · %s Volver", GLYPH_Y, GLYPH_B);
	text_draw_center_clip(keys, BOT_W / 2, SCR_H - 15, BOT_W - 16, 0.36f, T.text3);
	if (B.opt) {
		opt_draw_update();
		g_kdown = 0;
		g_touch.up = false;
		return;
	}
	if (g_kdown & KEY_A)
		open = list_pickable(B.lsel) ? B.lsel : -1;
	if (g_kdown & KEY_Y)
		hold = list_pickable(B.lsel) ? B.lsel : -1;
	// acciones
	if (tap(BOT_W - 36, 0, 36, HDR_H) || (g_kdown & KEY_START)) {
		close_br();
		g_kdown = 0;
		return;
	}
	if ((can_back && tap(0, 0, 36, HDR_H)) || (!B.home && (g_kdown & KEY_B))) {
		B.panel = 0;
		g_kdown = 0;
		return;
	}
	if (B.home && (g_kdown & KEY_B)) {
		close_br();
		g_kdown = 0;
		return;
	}
	if (tap(36, 0, BOT_W - 72, HDR_H) || (g_kdown & KEY_X)) {
		char nu[1024], full[1024];
		if (kb_input("Buscar en Google o escribir dirección", B.home ? "" : B.url, nu, sizeof nu, false, false) && nu[0]) {
			address_to_url(nu, full, sizeof full);
			char q[512];
			util_strlcpy(q, B.search_q, sizeof q);
			open_from_list(full);
			util_strlcpy(B.search_q, q, sizeof B.search_q); // go() la borra si no es de Google
		}
		g_kdown = 0;
		return;
	}
	if (hold >= 0) {
		opt_open(hold);
		B.lsel = hold;
	} else if (open >= 0) {
		const LItem *it = &LI[open];
		if (it->kind == 1)
			add_by_address();
		else {
			char u[400];
			util_strlcpy(u, it->kind == 0 ? bm_fav(it->idx)->url : bm_hist(it->idx)->url, sizeof u);
			open_from_list(u);
		}
	}
	g_kdown = 0;
	g_touch.up = false;
}

// pantalla de arriba en el inicio
static void home_top(void)
{
	bm_load();
	draw_grad_v(0, 0, TOP_W, SCR_H, T.surface, T.bg);
	text_draw_center("Navegador", TOP_W / 2, 26, TS_TITLE, T.text);
	const float PW = 260, PX = (TOP_W - PW) / 2, PY = 56, PH = 28;
	draw_rrect(PX, PY, PW, PH, PH / 2, T.surface2);
	draw_rrect_outline(PX, PY, PW, PH, PH / 2, 1, T.divider);
	draw_icon(IC_SEARCH, PX + 16, PY + PH / 2, 11, T.text3);
	text_draw("Buscar en Google o escribir dirección", PX + 28, PY + PH / 2 - text_lineh(0.40f) / 2, 0.40f, T.text3);
	text_draw_right(GLYPH_X, PX + PW - 12, PY + PH / 2 - text_lineh(0.40f) / 2, 0.40f, T.text3);
	const float LX = (TOP_W - 300) / 2;
	int n = bm_nhist() < 4 ? bm_nhist() : 4;
	if (!n) {
		text_draw_center("Tus favoritos están abajo: toca uno para abrirlo", TOP_W / 2, 112, TS_SUB, T.text3);
		return;
	}
	text_draw("SEGUIR DONDE LO DEJASTE", LX + 6, 102, 0.34f, T.text3);
	g_clip_ell = true;
	for (int i = 0; i < n; i++) {
		const BmHist *h = bm_hist(i);
		float y = 118 + i * 30;
		site_icon(h->title, h->url, LX + 6, y + 4, 20, false);
		char sub[160];
		hist_sub(h, sub, sizeof sub, true);
		text_draw_clip(h->title, LX + 34, y + 2, 262, TS_ROW * 0.92f, T.text);
		text_draw_clip(sub, LX + 34, y + 16, 262, 0.34f, T.text3);
	}
	g_clip_ell = false;
}

void br_bottom(void)
{
	if (B.st == ST_PARSED) { // el árbol está listo (se guarda para cambiar PC / móvil sin volver a bajarla)
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		B.st = ST_LAYOUT;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		B.thr = threadCreate(layout_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
		if (!B.thr) {
			snprintf(B.err, sizeof B.err, "Sin memoria para colocar la página");
			B.st = ST_ERROR;
		}
	}
	if (g_kdown || g_touch.held || abs((int)0) > 0)
		B.last_in = util_ms();
	{
		circlePosition cp;
		hidCircleRead(&cp);
		if (abs(cp.dx) > 18 || abs(cp.dy) > 18)
			B.last_in = util_ms();
	}
	if (B.st == ST_READY && B.jsp && B.idle_left > 0 && util_ms() > B.idle_next && util_ms() - B.last_in > 800) {
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		B.st = ST_ACT;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		B.thr = threadCreate(idle_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
		if (!B.thr)
			B.st = ST_READY, B.idle_left = 0;
	}
	if (B.st == ST_READY && B.click_link >= 0) { // el clic con JavaScript dejó seguir el enlace
		int k = B.click_link;
		B.click_link = -1;
		go_link(k);
	}
	if (B.st == ST_READY && B.js_nav[0]) { // location.href = … desde la página
		char u[1024];
		util_strlcpy(u, B.js_nav, sizeof u);
		B.js_nav[0] = 0;
		// sin que se haya tocado nada desde que cargó, es una redirección: no queda en el historial
		go(u, B.last_in > B.ready_ms);
	}
	// el buscador pidió un captcha, no aceptó el navegador o no contestó: la misma búsqueda en el
	// siguiente (Google → DuckDuckGo → Bing)
	bool s_fail = false;
	if (B.search_q[0] && B.search_stage == 0 && is_google(B.url))
		s_fail = B.st == ST_ERROR || (B.st == ST_READY && (strstr(B.url, "/sorry/") || strstr(B.url, "enablejs")));
	else if (B.search_q[0] && B.search_stage == 1 && strstr(B.url, "duckduckgo.com"))
		s_fail = B.st == ST_ERROR || (B.st == ST_READY && B.http_code == 202); // 202: su página de «¿eres un robot?»
	if (s_fail) {
		char q[900], full[1024];
		url_esc(B.search_q, q, sizeof q);
		if (B.search_stage == 0) {
			snprintf(full, sizeof full, "https://html.duckduckgo.com/html/?q=%s", q);
			toast_icon("Google no respondió: resultados de DuckDuckGo", IC_SEARCH, T.accent);
		} else {
			snprintf(full, sizeof full, "https://www.bing.com/search?q=%s&setlang=es&cc=ES", q);
			toast_icon("DuckDuckGo no respondió: resultados de Bing", IC_SEARCH, T.accent);
		}
		util_log("web: %s no respondió (%d, HTTP %d): siguiente buscador", B.search_stage ? "DuckDuckGo" : "Google", B.st, B.http_code);
		B.search_stage++;
		if (B.search_stage >= 2) // el último: si falla, se queda el error
			B.search_q[0] = 0;
		char keep[512];
		util_strlcpy(keep, B.search_q, sizeof keep);
		go(full, false);
		util_strlcpy(B.search_q, keep, sizeof B.search_q);
	}
	// al historial, cuando la página lleva un rato a la vista (las que solo redirigen no se apuntan)
	if (B.st == ST_READY && !B.js_nav[0] && util_ms() - B.ready_ms > 1500 && strcmp(B.logged, B.url)) {
		util_strlcpy(B.logged, B.url, sizeof B.logged);
		if (!strstr(B.url, "/sorry/") && !strstr(B.url, "enablejs")) {
			bm_load();
			bm_visit(B.title, B.url);
		}
	}
	if (B.home || B.panel) {
		lists_bottom();
		return;
	}
	if (B.st == ST_OOM) {
		oom_bottom();
		return;
	}
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	bool ready = B.st == ST_READY;
	// --- cabecera: atrás, dirección y cerrar
	draw_rect(0, 0, BOT_W, HDR_H, T.surface);
	draw_rect(0, HDR_H - 1, BOT_W, 1, T.divider);
	bool bp = touch_in(0, 0, 36, HDR_H);
	draw_circle(18, HDR_H / 2, 11, bp ? T.surface2 : T.surface);
	draw_icon(IC_CHEV_LEFT, 18, HDR_H / 2, 13, B.nhist ? T.text : T.text3);
	bool cp2 = touch_in(BOT_W - 36, 0, 36, HDR_H);
	draw_circle(BOT_W - 18, HDR_H / 2, 11, cp2 ? T.surface2 : T.surface);
	draw_icon(IC_CLOSE, BOT_W - 18, HDR_H / 2, 12, T.text2);
	draw_rrect(36, 5, BOT_W - 72, HDR_H - 10, (HDR_H - 10) / 2, T.surface2);
	const char *u = strstr(B.url, "://");
	text_draw_clip(u ? u + 3 : B.url, 46, HDR_H / 2 - text_lineh(0.40f) / 2, BOT_W - 92, 0.40f, T.text2);
	bool edit = tap(36, 0, BOT_W - 72, HDR_H) || (g_kdown & KEY_X);
	// --- herramientas: zoom y PC / móvil
	float ty = HDR_H + 4;
	bool zout = tool_btn(8, ty, 34, "-", false), zin = tool_btn(46, ty, 34, "+", false);
	char zp[16];
	snprintf(zp, sizeof zp, "%d %%", ready ? (int)(B.zoom * 100 + 0.5f) : 100);
	text_draw(zp, 88, ty + 12 - text_lineh(TS_SUB) / 2, TS_SUB, T.text3);
	bool tlight = tool_btn(BOT_W - 112, ty, 104, "Modo ligero", B.light);
	// favoritos: guardar esta página y abrir la lista
	bm_load();
	bool is_fav = B.url[0] && bm_find_fav(B.url) >= 0;
	float fx = BOT_W - 182;
	bool fp = touch_in(fx, ty, 30, 24), lp = touch_in(fx + 34, ty, 30, 24);
	draw_rrect(fx, ty, 30, 24, 12, is_fav ? col_alpha(T.warn, 0.22f) : fp ? T.surface2 : T.surface);
	draw_icon(IC_STAR, fx + 15, ty + 12, 13, is_fav ? T.warn : T.text);
	draw_rrect(fx + 34, ty, 30, 24, 12, lp ? T.surface2 : T.surface);
	draw_icon(IC_LIST, fx + 49, ty + 12, 13, T.text);
	if (tap(fx, ty, 30, 24) && B.url[0]) {
		if (is_fav) {
			bm_del_fav(bm_find_fav(B.url));
			toast_icon("Quitado de favoritos", IC_STAR, T.text2);
		} else if (bm_add_fav(B.title, B.url))
			toast_icon("Guardado en favoritos", IC_STAR, T.warn);
		else
			toast_icon("Ya no caben más favoritos", IC_CLOSE, T.danger);
	}
	if (tap(fx + 34, ty, 30, 24)) {
		B.panel = 1;
		B.tab = TAB_FAV;
		B.lsel = 0, B.lsy = 0, B.lkeys = false;
		g_touch.up = false;
		g_kdown = 0;
		return;
	}
	// --- touchpad (con su trama de puntos) y franja para desplazar
	const float TX = 8, TW = BOT_W - 16 - STRIP - 6, SX = BOT_W - 8 - STRIP;
	// el touchpad con su trama: pintado una vez en una textura (eran 264 puntos en cada fotograma)
	static GfxRT pad;
	static bool pad_tried;
	static u32 pad_col;
	if (!pad_tried) {
		pad_tried = true;
		gfx_rt_init(&pad, 512, 256);
	}
	if (pad.ok) {
		if (pad_col != (T.surface ^ T.divider) || !pad_col) { // al cambiar el tema, otra vez
			pad_col = (T.surface ^ T.divider) | 1;
			gfx_rt_begin(&pad, 0);
			draw_rrect(0, 0, TW, TP_B - TP_Y, 12, T.surface);
			for (float yy = 10; yy < TP_B - TP_Y - 6; yy += 12)
				for (float xx = 10; xx < TW - 6; xx += 12)
					draw_rect(xx, yy, 1.5f, 1.5f, T.divider);
			gfx_resume(g_bot);
		}
		gfx_rt_draw(&pad, TX, TP_Y);
	} else {
		draw_rrect(TX, TP_Y, TW, TP_B - TP_Y, 12, T.surface);
		for (float yy = TP_Y + 10; yy < TP_B - 6; yy += 12)
			for (float xx = TX + 10; xx < TX + TW - 6; xx += 12)
				draw_rect(xx, yy, 1.5f, 1.5f, T.divider);
	}
	draw_rrect(SX, TP_Y, STRIP, TP_B - TP_Y, 12, T.surface);
	if (ready && B.total > view_h()) {
		float h = TP_B - TP_Y - 8, gh = fmaxf(18, h * view_h() / B.total), gy = TP_Y + 4 + (h - gh) * (B.sy / (B.total - view_h()));
		draw_rrect(SX + 8, gy, STRIP - 16, gh, (STRIP - 16) / 2, T.accent);
	}
	// el dedo empezó en el touchpad (sigue valiendo aunque se arrastre: touch_in deja de valer al
	// arrastrar, y el puntero solo se movía los primeros píxeles de cada pasada)
	bool on_pad = g_touch.held && g_touch.sx >= TX && g_touch.sx < TX + TW && g_touch.sy >= TP_Y && g_touch.sy < TP_B;
	bool on_strip = g_touch.held && g_touch.sx >= SX && g_touch.sy >= TP_Y && g_touch.sy < TP_B;
	bool click = false;
	// con su JavaScript trabajando de fondo (o atendiendo un clic) el puntero y el desplazamiento
	// siguen: lo que espera es el clic, el zoom y PC / móvil
	bool moving = ready || B.st == ST_ACT;
	if (moving) {
		// puntero: el dedo en el touchpad o la palanca
		if (on_pad) { // con aceleración: despacio, preciso; deprisa, cruza la pantalla
			float ddx = (float)(g_touch.x - g_touch.px), ddy = (float)(g_touch.y - g_touch.py);
			float sp = sqrtf(ddx * ddx + ddy * ddy), k = 1.2f + fminf(sp, 12.0f) * 0.12f;
			B.px += ddx * k;
			B.py += ddy * k;
			if (sp > 0)
				B.focus = -1;
		}
		circlePosition cp;
		hidCircleRead(&cp);
		if (abs(cp.dx) > 18 || abs(cp.dy) > 18) {
			B.px += cp.dx / 26.0f;
			B.py -= cp.dy / 26.0f;
			B.focus = -1;
		}
		// el puntero en el borde empuja la página
		float ox = B.px < 0 ? B.px : B.px > TOP_W - 1 ? B.px - (TOP_W - 1) : 0;
		float oy = B.py < 0 ? B.py : B.py > SCR_H - 1 ? B.py - (SCR_H - 1) : 0;
		B.sx += ox / B.zoom, B.sy += oy / B.zoom;
		B.px = clampf(B.px, 0, TOP_W - 1), B.py = clampf(B.py, 0, SCR_H - 1);
		// franja: arrastrar desplaza, con inercia
		if (on_strip) {
			float d = (float)(g_touch.y - g_touch.py) * 3 / B.zoom;
			B.sy += d;
			B.vy = d;
		} else if (fabsf(B.vy) > 0.2f) {
			B.sy += B.vy;
			B.vy *= 0.92f;
		} else
			B.vy = 0;
		if (g_krepeat & KEY_DOWN)
			B.sy += 60 / B.zoom, B.vy = 0;
		if (g_krepeat & KEY_UP)
			B.sy -= 60 / B.zoom, B.vy = 0;
		if (ready && (g_krepeat & KEY_RIGHT))
			focus_step(1);
		if (ready && (g_krepeat & KEY_LEFT))
			focus_step(-1);
		if (ready && (zin || (g_krepeat & KEY_R)))
			zoom_by(1.25f);
		if (ready && (zout || (g_krepeat & KEY_L)))
			zoom_by(0.8f);
		clamp_view();
		click = (g_touch.up && !g_touch.drag && g_touch.sx >= TX && g_touch.sx < TX + TW && g_touch.sy >= TP_Y && g_touch.sy < TP_B) ||
		        (g_kdown & KEY_A);
		if (!ready) { // el clic espera a que su JavaScript termine el rato que lleva (como mucho medio segundo)
			B.click_wait |= click;
			click = false;
		} else if (B.click_wait)
			click = true, B.click_wait = false;
	}
	if (ready) {
		if (tlight || (g_kdown & KEY_Y)) { // modo ligero: sin imágenes ni JavaScript (se recuerda)
			B.light = !B.light;
			light_save();
			toast_icon(B.light ? "Modo ligero: sin imágenes ni JavaScript" : "Modo normal: con imágenes y JavaScript", IC_BOLT, T.accent);
			char u2[1024];
			util_strlcpy(u2, B.url, sizeof u2);
			start_load(u2);
		}
	}
	// --- debajo: a dónde lleva el enlace del puntero, o cómo se usa
	const char *tip = "Desliza para mover el puntero · toca para hacer clic";
	u32 tc = T.text3;
	bool oom_tip = ready && B.oom && strcmp(B.reader_url, B.url);
	if (ready && B.hover >= 0 && B.hover < B.nlinks)
		tip = B.links[B.hover], tc = T.accent;
	else if (B.st == ST_ERROR)
		tip = B.err, tc = T.danger;
	else if (oom_tip)
		tip = "Memoria llena · toca aquí para verla en modo lectura", tc = T.warn;
	// sin desplazarse para leerse entero: cada línea que se desplaza recorta con tijeras y obliga a
	// vaciar la cola de dibujo de la GPU (lo más caro de esta pantalla)
	g_clip_ell = true;
	text_draw_center_clip(tip, BOT_W / 2, TP_B + 4, BOT_W - 20, 0.40f, tc);
	static char keys[160];
	if (!keys[0])
		snprintf(keys, sizeof keys, "%s Clic · %s Enlaces · L R Zoom · %s Ligero · %s Atrás", GLYPH_A, GLYPH_DPAD, GLYPH_Y, GLYPH_B);
	text_draw_center_clip(keys, BOT_W / 2, TP_B + 18, BOT_W - 20, 0.36f, T.text3);
	g_clip_ell = false;
	// --- acciones
	if (oom_tip && tap(0, TP_B, BOT_W, 16)) {
		oom_reader();
		g_kdown = 0;
		return;
	}
	if (tap(BOT_W - 36, 0, 36, HDR_H) || (g_kdown & KEY_START)) {
		close_br();
		g_kdown = 0;
		return;
	}
	if (B.in_js && (g_kdown & KEY_B)) { // cargando su JavaScript: B lo salta (la página se ve tal cual)
		B.cancel = true;
		g_kdown = 0;
		return;
	}
	if (tap(0, 0, 36, HDR_H) || (g_kdown & KEY_B)) {
		back();
		g_kdown = 0;
		return;
	}
	if (edit) {
		char nu[1024];
		if (kb_input("Dirección o búsqueda", B.url, nu, sizeof nu, false, false) && nu[0]) {
			char full[1024];
			address_to_url(nu, full, sizeof full);
			go(full, true);
		}
	} else if (click && ready && wdg_at_pointer() >= 0) {
		wdg_activate(wdg_at_pointer());
	} else if (click && ready) {
		int k = link_at_pointer(), fi = frag_at_pointer();
		if (k < 0 && B.focus >= 0)
			k = B.f[B.focus].link;
		dom_node *el = fi >= 0 ? B.f[fi].node : B.focus >= 0 ? B.f[B.focus].node : NULL;
		if (B.jsp && el)
			start_action(el, k);
		else
			go_link(k);
	}
	g_kdown = 0;
	g_touch.up = false;
}
