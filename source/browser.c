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
#include "brlayout.h"
#include "webfont.h"
#include "core/node.h" /* libdom por dentro: hijos y hermanos sin referencias */
#include "dl.h"
#include "gfx.h"
#include "icons.h"
#include "img.h"
#include "svg.h"
#include "canvas.h"
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
#include <zlib.h>

#define MAX_PAGE  (6 * 1024 * 1024)
#define HIST      16
#define HDR_H     30.0f
#define SC_TEXT   0.46f
/* cómo se presenta: en Móvil, como el navegador de la 3DS (las webs dan su versión sencilla); en PC,
 * como uno de escritorio */
#define UA_MOB "Mozilla/5.0 (Nintendo 3DS; U; ; es) Version/1.7630.EU Cloud3DS"
#define UA_PC  "Mozilla/5.0 (X11; Linux x86_64; rv:133.0) Gecko/20100101 Firefox/133.0 Cloud3DS"
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
	s16 bgi;       /* una imagen de fondo: su forma en B.bgi (-1 no) */
	u8 fit;        /* imagen: object-fit (0 cover, 1 contain, 2 fill) */
} Frag;

/* cómo va una imagen de fondo en su caja (la caja es la del trozo) */
typedef struct {
	u8 size, rep, pxp, pyp; /* BRL_BG_*, repetición (x 1, y 2), posición en % */
	float sw, px, py;
} BgImg;

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
enum { IM_WAIT, IM_RGBA, IM_TEX, IM_FAIL, IM_LOAD /* un hilo la está bajando */ };
typedef struct {
	char *url;
	float w, h;            /* tamaño en la página (px) */
	float y;               /* dónde está (para bajar antes lo que se ve) */
	bool sized;            /* el tamaño venía de la página */
	bool natural;          /* de fondo a su tamaño (sprites): se decodifica sin reducir (hasta 512) */
	bool glyph;            /* un icono de una fuente de la página: blanco, se tiñe al pintarlo */
	bool guessed;          /* se colocó sin saber su proporción (ni la página la daba): 4:3 */
	int ow, oh;            /* el tamaño original de la imagen */
	volatile int st;
	u8 *rgba;
	int tw, th, iw, ih;    /* textura y tamaño original */
	Texture tex;
	char *data;            /* un <svg> de la propia página: su texto (no se baja) */
	size_t dn;
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
	bool over;     /* va encima de una imagen (se pinta después de ellas) */
	float rad;     /* border-radius (px) */
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
	bool nojs;           /* la página de ahora es de una web con el JavaScript apagado (botón «JS») */
	bool shown;          /* la página de ahora ya se vio: al recolocarla, se sigue viendo (no la pantalla de carga) */
	int js_scripts, js_errors;
	char js_nav[1024];   /* la página pidió ir a otra dirección desde su JavaScript */
	JsPage *jsp;         /* el JavaScript de la página, vivo para los clics */
	bool js_defer;       /* la página ya se ve: su JavaScript, después (jsdef_thread) */
	size_t js_lim;       /* la memoria para ese JavaScript */
	u32 css_sig;         /* huella de sus hojas y de <html>/<body> (si el JavaScript la cambia, CSS otra vez) */
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
	bool dark;             /* modo oscuro (de serie; se guarda en la SD si se quita) */
	bool dark_tx;          /* esta página se oscurece al pintarla (lo claro; lo oscuro se queda) */
	bool click_wait;       /* un clic hecho mientras su JavaScript trabajaba: se hace al acabar */
	volatile bool thr_done; /* el hilo de B.thr ya acabó */
	volatile bool abort;   /* lo que está cargando se deja: cortar cuanto antes */
	int pend;              /* lo pedido mientras cargaba (se hace al soltar la carga) */
	char pend_url[1024];
	bool css_dark;         /* la página se pidió con tema oscuro */
	bool lt_tx;            /* se pidió oscura y ahora es modo claro: se aclara al pintar */  /* lo buscado en Google (por si Google no responde: DuckDuckGo) */
	int idle_relayouts;  /* recolocaciones por su JavaScript de fondo (como mucho 3) */
	bool img_relaid;     /* ya se recolocó por las proporciones de las imágenes (una vez por página) */
	bool idle_dirty;     /* cambió algo más que aún no se ve: se recoloca con el próximo clic */
	int nclk;            /* cosas que se tocan por JavaScript */
	volatile int click_link; /* tras un clic con JavaScript: el enlace a seguir (-1 ninguno, -2 esperando) */
	volatile int click_wdg;  /* tras tocar un control con JavaScript: hacer lo suyo (ACT_WDG / ACT_INPUT; 0 nada) */
	PImg *img;
	int nimg, cimg;
	BgImg *bgi;
	int nbgi, cbgi;
	Thread img_thr[3]; /* las imágenes: 3 a la vez (cada petición, con su TLS, es lo que más espera) */
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
	if (B.abort) // la página se deja: se corta la descarga
		return 0;
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
static int fetch_net(const char *url, const char *accept, Buf *b, long *code)
{
	*code = 0;
	if (B.abort) // la página se deja: ni se pide
		return DLF_FAIL;
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
	// algunas webs rechazan el TLS antiguo de httpc (403, 429…), no conecta o llega vacío: otra vez con libcurl
	if (r != DLF_OK || *code == 403 || *code == 429 || *code == 503 || (*code < 400 && !b->n)) { // (vacío: 200, 3xx sin seguir…)
		if (!skip)
			util_log("web: httpc no pudo con %.100s (%d, HTTP %ld): libcurl", url, r, *code);
		free(b->p);
		b->p = NULL, b->n = b->cap = 0;
		r = dl_fetch_curl(url, accept, buf_cb, b, code);
		if (r == DLF_OK && *code < 400 && !skip)
			nohc_add(url);
	}
	// llegó comprimido sin pedirlo (gzip): se descomprime
	if (r == DLF_OK && b->n > 18 && (u8)b->p[0] == 0x1f && (u8)b->p[1] == 0x8b) {
		z_stream z = {0};
		Buf o = {0};
		if (inflateInit2(&z, 16 + MAX_WBITS) == Z_OK) {
			z.next_in = (Bytef *)b->p, z.avail_in = (uInt)b->n;
			char tmp[16384];
			int zr;
			do {
				z.next_out = (Bytef *)tmp, z.avail_out = sizeof tmp;
				zr = inflate(&z, Z_NO_FLUSH);
				size_t got = sizeof tmp - z.avail_out;
				if (got && buf_cb(tmp, 1, got, &o) != got)
					zr = Z_MEM_ERROR;
			} while (zr == Z_OK);
			inflateEnd(&z);
			if (zr == Z_STREAM_END && o.n) {
				free(b->p);
				*b = o;
			} else
				free(o.p);
		}
	}
	return r;
}

static u32 mem_free(void);
static void browser_thread_net(void);
/* el documento que se está leyendo o colocando: la página o uno de sus iframes (su dirección, para lo relativo, y su
 * ancho, para el CSS) */
static const char *DOC_BASE;
static float DOC_W;
#define BASE_URL (DOC_BASE ? DOC_BASE : B.url)
#define BASE_W (DOC_W > 0 ? DOC_W : VW_PC)
static bool ci_has(const char *h, const char *nd);
static int nojs_find(const char *url);
/* tras tocar algo con JavaScript: un clic (en un enlace, en lo que sea o en un control) o lo escrito en una caja de texto */
enum { ACT_CLICK = 0, ACT_WDG, ACT_INPUT };
static void start_action_k(dom_node *el, int link, int kind, const char *str);

/* ------------------------------------------------------------ descarga por adelantado
 * Nada más leer el HTML se piden a la vez (varios hilos) los scripts y las hojas de estilo que la página va a
 * pedir: mientras corre el JavaScript, o mientras llega una hoja, ya se están bajando las demás. Antes iban de
 * una en una, y en la consola cada petición (con su TLS) es lo más lento de cargar una página */
#define PF_MAX 48
#define PF_THR 3 /* (httpc tiene pocas sesiones: con la de la página y la de las imágenes, 5) */
enum { PF_QUEUED, PF_RUN, PF_DONE, PF_TAKEN };
typedef struct {
	char url[1024], fin[1024];
	const char *accept;
	Buf b;
	long code;
	int r;
	volatile int st;
} PfEnt;
static struct {
	PfEnt *e;
	int n;
	volatile bool stop;
	LightLock lk;
	LightEvent ev;
	Thread th[PF_THR];
	int nth;
	bool init;
} PF;
static __thread char FA_FIN[1024]; /* a dónde acabó la última fetch_any (tras redirigir) */

static void pf_worker(void *arg)
{
	(void)arg;
	browser_thread_net();
	for (;;) {
		PfEnt *e = NULL;
		LightLock_Lock(&PF.lk);
		// primero las hojas (sin ellas no se enseña la página; los scripts van ya después), luego lo demás
		for (int pass = 0; pass < 2 && !e; pass++)
			for (int i = 0; i < PF.n && !PF.stop && !B.abort; i++)
				if (PF.e[i].st == PF_QUEUED && (pass || !strncmp(PF.e[i].accept, "text/css", 8))) {
					e = &PF.e[i];
					e->st = PF_RUN;
					break;
				}
		LightLock_Unlock(&PF.lk);
		if (!e)
			break;
		if (mem_free() < 6u * 1024 * 1024) // (poca memoria: que lo pida quien lo use, cuando lo use)
			e->r = DLF_FAIL, e->code = 0;
		else {
			e->r = fetch_net(e->url, e->accept, &e->b, &e->code);
			util_strlcpy(e->fin, dl_last_url()[0] ? dl_last_url() : e->url, sizeof e->fin);
		}
		e->st = PF_DONE;
		LightEvent_Signal(&PF.ev);
	}
}

static void pf_add(const char *url, const char *accept)
{
	if (!PF.e || PF.n >= PF_MAX || strncasecmp(url, "http", 4) || blocked_host(url))
		return;
	for (int i = 0; i < PF.n; i++)
		if (!strcmp(PF.e[i].url, url))
			return;
	PfEnt *e = &PF.e[PF.n];
	memset(e, 0, sizeof *e);
	util_strlcpy(e->url, url, sizeof e->url);
	e->accept = accept;
	e->st = PF_QUEUED;
	PF.n++;
}

// lo que la página pedirá: <script src> (si corre su JavaScript) y <link rel="stylesheet">, en su orden
static void pf_collect(dom_node *n, int depth, bool js)
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
			char tag[8] = "", a[256], href[1024], abs[1024];
			if (dom_node_get_node_name(ch, &nm) == DOM_NO_ERR && nm) {
				size_t l = dom_string_byte_length(nm);
				snprintf(tag, sizeof tag, "%.*s", (int)(l < 7 ? l : 7), dom_string_data(nm));
				dom_string_unref(nm);
			}
			if (!strcasecmp(tag, "link")) {
				if (attr(ch, "rel", a, sizeof a) && ci_has(a, "stylesheet") && !ci_has(a, "alternate") &&
				    !(attr(ch, "media", a, sizeof a) && !cssprep_media_ok(a, VW_PC)) && attr(ch, "href", href, sizeof href) && href[0]) {
					resolve(B.url, href, abs, sizeof abs);
					pf_add(abs, "text/css,*/*;q=0.1");
				}
			} else if (!strcasecmp(tag, "script")) {
				char nm[8];
				bool mod = attr(ch, "type", a, sizeof a) && !strcasecmp(a, "module");
				bool ty = mod || (!attr(ch, "nomodule", nm, sizeof nm) &&
				                  (!attr(ch, "type", a, sizeof a) || !a[0] || !strcasecmp(a, "text/javascript") || !strcasecmp(a, "application/javascript") ||
				                   !strcasecmp(a, "application/x-javascript") || !strcasecmp(a, "text/ecmascript")));
				if (js && ty && attr(ch, "src", href, sizeof href) && href[0]) {
					resolve(B.url, href, abs, sizeof abs);
					pf_add(abs, "*/*");
				}
			} else if (strcasecmp(tag, "svg"))
				pf_collect(ch, depth + 1, js);
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(ch, &nx);
		dom_node_unref(ch);
		ch = nx;
	}
}

static void pf_stop(void);
static void pf_start(dom_document *doc, bool js)
{
	pf_stop();
	if (!PF.init) {
		LightLock_Init(&PF.lk);
		LightEvent_Init(&PF.ev, RESET_ONESHOT);
		PF.init = true;
	}
	PF.e = calloc(PF_MAX, sizeof *PF.e);
	PF.n = 0, PF.stop = false, PF.nth = 0;
	if (!PF.e || !doc)
		return;
	pf_collect((dom_node *)doc, 0, js);
	if (!PF.n)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	for (int i = 0; i < PF_THR && i < PF.n; i++) {
		Thread t = threadCreate(pf_worker, NULL, 64 * 1024, prio, br_core(), false);
		if (t)
			PF.th[PF.nth++] = t;
	}
}

// se acabó de cargar: lo que se esté bajando termina y lo que sobró se suelta
static void pf_stop(void)
{
	if (!PF.init)
		return;
	PF.stop = true;
	for (int i = 0; i < PF.nth; i++) {
		threadJoin(PF.th[i], U64_MAX);
		threadFree(PF.th[i]);
	}
	PF.nth = 0;
	if (PF.e)
		for (int i = 0; i < PF.n; i++)
			free(PF.e[i].b.p);
	free(PF.e);
	PF.e = NULL, PF.n = 0;
}

// ¿ya se pidió por adelantado? se espera a que llegue (si no ha empezado, lo pide quien lo quiere)
static bool pf_take(const char *url, Buf *b, long *code, int *r)
{
	if (!PF.e || !PF.nth)
		return false;
	PfEnt *e = NULL;
	LightLock_Lock(&PF.lk);
	for (int i = 0; i < PF.n; i++)
		if (!strcmp(PF.e[i].url, url) && PF.e[i].st != PF_TAKEN) {
			e = &PF.e[i];
			if (e->st == PF_QUEUED)
				e->st = PF_TAKEN, e = NULL; // (lo baja ya quien lo pide)
			break;
		}
	LightLock_Unlock(&PF.lk);
	if (!e)
		return false;
	while (e->st == PF_RUN && !B.abort)
		LightEvent_WaitTimeout(&PF.ev, 100000000LL);
	if (e->st != PF_DONE)
		return false;
	*b = e->b, *code = e->code, *r = e->r;
	util_strlcpy(FA_FIN, e->fin, sizeof FA_FIN);
	memset(&e->b, 0, sizeof e->b);
	e->st = PF_TAKEN;
	return true;
}

static int fetch_any(const char *url, const char *accept, Buf *b, long *code)
{
	int r;
	if (pf_take(url, b, code, &r))
		return r;
	r = fetch_net(url, accept, b, code);
	util_strlcpy(FA_FIN, strstr(url, "://") && dl_last_url()[0] ? dl_last_url() : url, sizeof FA_FIN);
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
static void css_sheet(BrCss *c, const char *css, size_t n, const char *url, int *links, size_t *bytes, int depth);

/* con el JavaScript para después, las hojas bajadas se guardan un rato: si el JavaScript cambia las
 * hojas o las clases de <html>/<body>, el CSS se rehace sin volver a bajarlas */
static struct {
	char *url;
	Buf b;
} KEEP[CSS_MAX_LINKS];
static int NKEEP;
static bool KEEP_ON;
static int keep_find(const char *url)
{
	for (int i = 0; i < NKEEP; i++)
		if (!strcmp(KEEP[i].url, url))
			return i;
	return -1;
}
static void keep_free(void)
{
	for (int i = 0; i < NKEEP; i++)
		free(KEEP[i].url), free(KEEP[i].b.p);
	NKEEP = 0;
	KEEP_ON = false;
}

// una hoja de otra parte (un <link> o un @import): se baja (si no se bajó ya) y entra con las suyas
static __attribute__((noinline)) void css_fetch(BrCss *c, const char *abs, int *links, size_t *bytes, int depth)
{
	const char *key = strlen(abs) > 95 ? abs + strlen(abs) - 95 : abs; // el final: el nombre con su huella
	for (int i = 0; i < CSS_NSEEN; i++)
		if (!strcmp(CSS_SEEN[i], key))
			return;
	if (*links >= CSS_MAX_LINKS)
		return;
	if (CSS_NSEEN < CSS_MAX_LINKS)
		util_strlcpy(CSS_SEEN[CSS_NSEEN++], key, sizeof CSS_SEEN[0]);
	Buf cb = {0};
	long code = 200;
	(*links)++;
	int r = DLF_OK;
	int k = keep_find(abs);
	if (k >= 0) // (ya bajada: rehaciendo el CSS tras el JavaScript)
		cb = KEEP[k].b;
	else {
		r = fetch_any(abs, "text/css,*/*;q=0.1", &cb, &code);
		util_log("web: css %s -> %d, HTTP %ld, %u bytes", abs, r, code, (unsigned)cb.n);
	}
	if (r == DLF_OK && code < 400 && cb.n && *bytes + cb.n <= CSS_MAX_BYTES)
		css_sheet(c, cb.p, cb.n, abs, links, bytes, depth);
	if (k < 0 && KEEP_ON && NKEEP < CSS_MAX_LINKS && r == DLF_OK && code < 400 && cb.n && mem_free() > 8u * 1024 * 1024) {
		KEEP[NKEEP].url = strdup(abs);
		if (KEEP[NKEEP].url) {
			KEEP[NKEEP++].b = cb;
			return;
		}
	}
	if (k < 0)
		free(cb.p);
}

// una hoja: primero las de sus @import (van antes en la cascada), luego ella
static void css_sheet(BrCss *c, const char *css, size_t n, const char *url, int *links, size_t *bytes, int depth)
{
	for (size_t i = 0; depth < 3 && i + 7 < n;) {
		const char *q = css + i;
		if (*q == '/' && q[1] == '*') { // comentario
			const char *e = strstr(q + 2, "*/");
			i = e ? (size_t)(e - css) + 2 : n;
			continue;
		}
		if (*q == '@' && !strncasecmp(q, "@import", 7)) {
			const char *e = memchr(q, ';', n - i);
			if (!e)
				break;
			char rel[512] = "", med[160] = "";
			const char *t = q + 7;
			while (t < e && isspace((unsigned char)*t))
				t++;
			if (!strncasecmp(t, "url(", 4))
				t += 4;
			while (t < e && (isspace((unsigned char)*t) || *t == '"' || *t == '\''))
				t++;
			const char *u = t;
			while (t < e && *t != '"' && *t != '\'' && *t != ')' && !isspace((unsigned char)*t))
				t++;
			snprintf(rel, sizeof rel, "%.*s", (int)(t - u), u);
			while (t < e && (*t == '"' || *t == '\'' || *t == ')' || isspace((unsigned char)*t)))
				t++;
			snprintf(med, sizeof med, "%.*s", (int)(e - t), t);
			if (rel[0] && (!med[0] || !strncasecmp(med, "layer", 5) || cssprep_media_ok(med, VW_PC))) {
				char *abs = malloc(1024);
				if (abs) {
					resolve(url, rel, abs, 1024);
					css_fetch(c, abs, links, bytes, depth + 1);
					free(abs);
				}
			}
			i = (size_t)(e - css) + 1;
			continue;
		}
		// los @import van al principio (tras @charset): con la primera regla, se acabaron
		if (*q == '{')
			break;
		i++;
	}
	if (*bytes + n <= CSS_MAX_BYTES) {
		brcss_add(c, css, n, url);
		*bytes += n;
	}
}

// un <style> o un <link rel="stylesheet">; aparte, para que la recursión de css_collect no lleve
// estos búferes en cada nivel (con páginas profundas desbordaban la pila del hilo)
static __attribute__((noinline)) void css_one(BrCss *c, dom_node *el, bool is_style, int *links, size_t *bytes)
{
	char a[256];
	if (is_style) {
		if (attr(el, "media", a, sizeof a) && !cssprep_media_ok(a, BASE_W))
			return;
		dom_string *tx = NULL;
		if (dom_node_get_text_content(el, &tx) == DOM_NO_ERR && tx) {
			css_sheet(c, dom_string_data(tx), dom_string_byte_length(tx), BASE_URL, links, bytes, 0);
			dom_string_unref(tx);
		}
		return;
	}
	if (*links >= CSS_MAX_LINKS || !attr(el, "rel", a, sizeof a) || !ci_has(a, "stylesheet") || ci_has(a, "alternate"))
		return;
	if (attr(el, "media", a, sizeof a) && !cssprep_media_ok(a, BASE_W)) // print, tema oscuro…
		return;
	char *href = malloc(2048), *abs = href ? href + 1024 : NULL;
	if (!href)
		return;
	if (attr(el, "href", href, 1024) && href[0]) {
		resolve(BASE_URL, href, abs, 1024);
		css_fetch(c, abs, links, bytes, 0);
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
	                                 "moatads.com", "chartbeat.", "optimizely.com", "adsafeprotected.com", "2mdn.net", "yieldmo.com",
	                                 // (más redes de anuncios, pujas y medición que aparecieron en las webs de prueba)
	                                 "mrf.io", "sensic.net", "indexww.com", "casalemedia.com", "openx.net", "smartadserver.com", "teads.tv",
	                                 "seedtag.com", "permutive.", "omtrdc.net", "demdex.net", "krxd.net", "bluekai.com", "id5-sync.com",
	                                 "adform.net", "33across.com", "sharethrough.com", "gumgum.com", "bidswitch.net", "everesttech.net",
	                                 "pbstck.com", "omnitagjs.com", "parsely.com", "metrics.brightcove.com", "ads-twitter.com", "analytics.tiktok.com",
	                                 "snap.licdn.com", "bat.bing.com", "mc.yandex", "googleoptimize.com", "doubleverify.com",
	                                 "lijit.com", "adsrvr.org", "rlcdn.com", "tapad.com", "agkn.com", "media.net", "amazon-adsystem", NULL};
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

static bool js_rect(dom_node *n, float r[4]);
static void geo_free(void);
static void tag_of(dom_node *n, char *tag, size_t sz);
static int js_fetch(const char *url, const char *method, const char *hdrs, const char *body, size_t blen, char **out, size_t *n, long *code,
                    char *fin, size_t fl, char *ctype, size_t cl)
{
	if (blocked_host(url)) {
		util_log("web: bloqueado %.80s", url);
		return -1;
	}
	Buf b = {0};
	int r;
	*code = 0;
	if (ctype && cl)
		ctype[0] = 0;
	if (!strncasecmp(url, "file:", 5)) { // de una página de la SD: es un archivo de la SD
		url += 5;
		while (url[0] == '/' && url[1] == '/')
			url++;
	}
	bool get = !strcasecmp(method, "GET");
	if ((!get || (hdrs && *hdrs)) && strstr(url, "://")) { // POST y demás, o con cabeceras propias: tal cual las pide
		// sus cabeceras («Nombre: valor» por línea), y si manda datos sin decir de qué tipo, de formulario
		const char *H[24];
		char hb[2048];
		int nh = 0;
		bool has_ct = false;
		util_strlcpy(hb, hdrs ? hdrs : "", sizeof hb);
		for (char *p = hb, *e; *p && nh < 22; p = e) {
			e = p + strcspn(p, "\n");
			char *nx = *e ? e + 1 : e;
			*e = 0;
			if (strchr(p, ':') && strncasecmp(p, "host:", 5) && strncasecmp(p, "content-length:", 15) && strncasecmp(p, "cookie:", 7)) {
				has_ct |= !strncasecmp(p, "content-type:", 13);
				H[nh++] = p;
			}
			e = nx;
		}
		if (!get && !has_ct)
			H[nh++] = "Content-Type: application/x-www-form-urlencoded; charset=UTF-8";
		H[nh] = NULL;
		int m = get ? HTTPC_METHOD_GET : !strcasecmp(method, "PUT") ? HTTPC_METHOD_PUT : !strcasecmp(method, "DELETE") ? HTTPC_METHOD_DELETE :
		        !strcasecmp(method, "HEAD") ? HTTPC_METHOD_HEAD : HTTPC_METHOD_POST;
		r = dl_request(m, url, H, body, blen, buf_cb, &b, code);
		util_strlcpy(fin, url, fl);
	} else {
		r = fetch_any(url, "*/*", &b, code);
		util_strlcpy(fin, FA_FIN, fl);
	}
	if (ctype && cl)
		util_strlcpy(ctype, dl_last_ctype(), cl);
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

// cuánto texto se ve ya sin JavaScript (fuera de script, style, noscript y template), hasta «lim»
static void page_text(dom_node *n, int depth, size_t *k, size_t lim)
{
	if (depth > 80 || *k >= lim)
		return;
	dom_node *c = NULL;
	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return;
	while (c && *k < lim) {
		dom_node_type t;
		if (dom_node_get_node_type(c, &t) == DOM_NO_ERR) {
			if (t == DOM_TEXT_NODE) {
				dom_string *v = NULL;
				if (dom_node_get_node_value(c, &v) == DOM_NO_ERR && v) {
					const char *d = dom_string_data(v);
					for (size_t i = 0, l = dom_string_byte_length(v); i < l; i++)
						*k += !isspace((unsigned char)d[i]);
					dom_string_unref(v);
				}
			} else if (t == DOM_ELEMENT_NODE) {
				dom_string *nm = NULL;
				char tag[10] = "";
				if (dom_node_get_node_name(c, &nm) == DOM_NO_ERR && nm) {
					snprintf(tag, sizeof tag, "%.*s", (int)dom_string_byte_length(nm), dom_string_data(nm));
					dom_string_unref(nm);
				}
				if (strcasecmp(tag, "script") && strcasecmp(tag, "style") && strcasecmp(tag, "noscript") && strcasecmp(tag, "template") &&
				    strcasecmp(tag, "head"))
					page_text(c, depth + 1, k, lim);
			}
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
}

// la huella de lo que decide el CSS además de los estilos de cada elemento: sus hojas (<link> y el largo de
// cada <style>) y los atributos de <html> y <body> (clases como «js» o el tema)
static u32 sig_mix(u32 h, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		h = (h ^ (u8)p[i]) * 16777619u;
	return h;
}
static void css_sig_walk(dom_node *n, int depth, u32 *h)
{
	if (depth > 80)
		return;
	dom_node *c = NULL;
	if (dom_node_get_first_child(n, &c) != DOM_NO_ERR)
		return;
	while (c) {
		dom_node_type t;
		if (dom_node_get_node_type(c, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
			dom_string *nm = NULL;
			char tag[8] = "", a[512];
			if (dom_node_get_node_name(c, &nm) == DOM_NO_ERR && nm) {
				snprintf(tag, sizeof tag, "%.*s", (int)dom_string_byte_length(nm), dom_string_data(nm));
				dom_string_unref(nm);
			}
			if (!strcasecmp(tag, "style")) {
				dom_string *tx = NULL;
				size_t l = 0;
				if (dom_node_get_text_content(c, &tx) == DOM_NO_ERR && tx)
					l = dom_string_byte_length(tx), dom_string_unref(tx);
				*h = sig_mix(*h, (const char *)&l, sizeof l);
			} else if (!strcasecmp(tag, "link")) {
				if (attr(c, "rel", a, sizeof a) && ci_has(a, "stylesheet") && attr(c, "href", a, sizeof a))
					*h = sig_mix(*h, a, strlen(a));
			} else if (strcasecmp(tag, "script") && strcasecmp(tag, "svg")) {
				if (!strcasecmp(tag, "html") || !strcasecmp(tag, "body")) {
					static const char *const AT[] = {"class", "style", "data-theme", "lang", "dir", NULL};
					for (int i = 0; AT[i]; i++)
						if (attr(c, AT[i], a, sizeof a))
							*h = sig_mix(sig_mix(*h, AT[i], strlen(AT[i])), a, strlen(a));
				}
				css_sig_walk(c, depth + 1, h);
			}
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
}
// y el conjunto de clases de la página (las reglas que piden una clase que no hay no se le dan a libcss:
// si el JavaScript trae una nueva, como «is-open» al abrir un menú, hay que rehacer el CSS)
static u32 cls_set_sum(dom_node_internal *n, u32 *tab, int cap)
{
	u32 sum = 0;
	dom_node_internal *st[96];
	int sp = 0;
	for (; n; n = n->next) // (los hermanos, aquí; los hijos, con la pila o por recursión si se llena)
		if (n->type == DOM_ELEMENT_NODE) {
			lwc_string **cl = NULL;
			uint32_t nc = 0;
			if (dom_element_get_classes((dom_element *)n, &cl, &nc) == DOM_NO_ERR) {
				for (uint32_t k = 0; k < nc; k++) {
					u32 h = sig_mix(2166136261u, lwc_string_data(cl[k]), lwc_string_length(cl[k])) | 1;
					for (int i = (int)(h & (u32)(cap - 1)), t = 0; t < cap; i = (i + 1) & (cap - 1), t++) {
						if (tab[i] == h)
							break;
						if (!tab[i]) {
							tab[i] = h, sum += h;
							break;
						}
					}
					lwc_string_unref(cl[k]);
				}
			}
			if (n->first_child) {
				if (sp < 96)
					st[sp++] = n->first_child;
				else
					sum += cls_set_sum(n->first_child, tab, cap);
			}
		}
	while (sp)
		sum += cls_set_sum(st[--sp], tab, cap);
	return sum;
}
static u32 css_sig(dom_document *doc)
{
	u32 h = 2166136261u;
	if (!doc)
		return h;
	css_sig_walk((dom_node *)doc, 0, &h);
	u32 *tab = calloc(16384, sizeof *tab);
	if (tab) {
		h ^= cls_set_sum(((dom_node_internal *)doc)->first_child, tab, 16384) * 2654435761u;
		free(tab);
	}
	return h;
}

static BrCss *css_build(dom_document *doc, size_t *bytes_out);
// tras el JavaScript (al cargar, un clic, sus temporizadores): si cambió lo que decide qué reglas valen
// (hojas, <html>/<body> o clases nuevas), el CSS otra vez, con las hojas guardadas
static void css_refresh(void)
{
	if (!B.css || !B.doc || !B.css_sig)
		return;
	u32 sg = css_sig(B.doc);
	if (sg == B.css_sig)
		return;
	u64 t0 = osGetTime();
	BrCss *nc = css_build(B.doc, NULL);
	if (!nc)
		return;
	brcss_free(B.css);
	B.css = nc;
	B.ncss = brcss_sheets(nc);
	B.css_sig = sg;
	util_log("web: CSS otra vez (lo cambió su JavaScript), %u ms", (unsigned)(osGetTime() - t0));
}

// el CSS de la página, de sus <style> y <link> (las hojas, bajadas o de las guardadas)
static BrCss *css_build(dom_document *doc, size_t *bytes_out)
{
	BrCss *css = brcss_new(BASE_URL);
	if (!css)
		return NULL;
	int links = 0;
	size_t bytes = 0;
	CSS_NSEEN = 0;
	u64 t0 = osGetTime();
	css_collect(css, (dom_node *)doc, 0, &links, &bytes);
	u64 t1 = osGetTime();
	brcss_finish(css, doc, BASE_W); // su CSS moderno, traducido para libcss
	util_log("web: CSS: bajar %u ms, procesar %u ms", (unsigned)(t1 - t0), (unsigned)(osGetTime() - t1));
	if (bytes_out)
		*bytes_out = bytes;
	return css;
}

/* ------------------------------------------------------------ iframes: otra página dentro, con su CSS y su JavaScript
 * (su window y su document), pintada en su hueco. Las ventanas se hablan con postMessage (en JSON): la página es
 * la 0 y los iframes la 1… Así funcionan los vídeos incrustados, los inicios de sesión y los retos «no soy un robot» */
#define FR_MAX 4
typedef struct {
	dom_node *el;        /* su <iframe> en la página */
	char url[1024];
	dom_document *doc;
	BrCss *css;
	JsPage *jsp;
	float w, h;
	float cx, cy, cw, ch; /* su hueco por dentro (sin el borde), de la última maquetación */
	bool tried;
} Frame;
static Frame FR[FR_MAX];
static int NFR;
typedef struct {
	int from, to;
	char *json;
	char origin[128];
} FMsg;
static FMsg FMQ[64];
static int NFMQ;

static int js_frame_of(dom_node *el)
{
	for (int i = 0; i < NFR; i++)
		if (FR[i].el == el && FR[i].jsp)
			return i + 1;
	return -1;
}
// (todo el JavaScript, de la página y de sus iframes, va en el mismo hilo, de uno en uno: la cola no necesita cerrojo)
// una imagen para el <canvas> de una página (drawImage, createPattern): bajada y decodificada aquí mismo, en el
// hilo del JavaScript; hasta 1024x1024 (si es más grande, reducida: el canvas la escala a su tamaño de verdad)
static uint8_t *js_image(const char *url, int *w, int *h, int *ow, int *oh)
{
	Buf b = {0};
	long code = 0;
	int r = DLF_FAIL;
	if (!strncmp(url, "data:", 5)) {
		const char *c = strstr(url, ";base64,");
		if (c) {
			size_t l = strlen(c + 8);
			if ((b.p = malloc(l * 3 / 4 + 4)))
				b.n = util_b64_decode(c + 8, l, (u8 *)b.p), r = DLF_OK, code = 200;
		} else if ((c = strchr(url, ',')) && (b.p = strdup(c + 1))) // (data:image/svg+xml,<svg…> sin base64)
			b.n = strlen(b.p), r = DLF_OK, code = 200;
	} else if (!strncmp(url, "file://", 7)) // (una página de la SD: la imagen, de la SD)
		r = fetch_any(url + 7, "image/*", &b, &code);
	else if (!blocked_host(url))
		r = fetch_any(url, "image/png,image/jpeg,image/gif,image/*;q=0.5", &b, &code);
	u8 *px = NULL;
	*w = *h = *ow = *oh = 0;
	if (r == DLF_OK && code < 400 && b.n && mem_free() > 2 * MEM_RESERVE + (u32)b.n * 2 + 4u * 1024 * 1024) {
		bool svg = false;
		for (size_t i = 0; i < b.n && i < 512 && !svg; i++)
			svg = !strncmp(b.p + i, "<svg", 4);
		if (svg) {
			if (svg_render(b.p, b.n, 1024, 1024, &px, w, h))
				*ow = *w, *oh = *h;
		} else {
			img_mem_info((const u8 *)b.p, b.n, ow, oh);
			if (img_mem_fit((const u8 *)b.p, b.n, 1024, 1024, &px, w, h) != IMG_OK)
				px = NULL;
			if (px && (*ow <= 0 || *oh <= 0))
				*ow = *w, *oh = *h;
		}
	}
	util_log("web: canvas: imagen %.100s -> %s (%dx%d)", strncmp(url, "data:", 5) ? url : "data:…", px ? "bien" : "no", *ow, *oh);
	free(b.p);
	return px;
}
static void js_post(int from, int to, const char *json, const char *origin)
{
	if (NFMQ >= 64 || to < 0 || to > NFR || to == from)
		return;
	char *j = strdup(json);
	if (!j)
		return;
	FMsg *m = &FMQ[NFMQ++];
	m->from = from, m->to = to, m->json = j;
	util_strlcpy(m->origin, origin, sizeof m->origin);
}
static JsPage *frame_jsp(int id)
{
	return id == 0 ? B.jsp : id <= NFR ? FR[id - 1].jsp : NULL;
}
// el JavaScript de qué ventana lleva un nodo (el de la página o el de su iframe)
static JsPage *jsp_of_node(dom_node *n)
{
	dom_document *d = NULL;
	if (n && dom_node_get_owner_document(n, &d) == DOM_NO_ERR && d) {
		for (int i = 0; i < NFR; i++)
			if (FR[i].doc == d) {
				dom_node_unref(d);
				return FR[i].jsp;
			}
		dom_node_unref(d);
	}
	return B.jsp;
}
// los mensajes pendientes, a su ventana (pueden traer otros); true si alguna cambió
static bool frames_pump(void)
{
	bool any = false;
	for (int round = 0; round < 100 && NFMQ && !B.abort; round++) {
		FMsg m = FMQ[0];
		memmove(FMQ, FMQ + 1, sizeof *FMQ * (size_t)--NFMQ);
		JsPage *p = frame_jsp(m.to);
		if (p) {
			bool ch = false;
			JsResult r;
			jsdom_message(p, m.json, m.origin, m.from, &ch, &r);
			any |= ch;
			if (m.to == 0 && r.navigate[0])
				util_strlcpy(B.js_nav, r.navigate, sizeof B.js_nav);
		}
		free(m.json);
	}
	return any;
}
static void frames_free(void)
{
	for (int i = 0; i < NFR; i++) {
		jsdom_close(FR[i].jsp); // (antes que su documento: tiene sus nodos)
		if (FR[i].doc)
			dom_node_unref(FR[i].doc);
		brcss_free(FR[i].css);
	}
	memset(FR, 0, sizeof FR);
	NFR = 0;
	cv_free_all(); // (los <canvas> de la página y de sus iframes)
	for (int i = 0; i < NFMQ; i++)
		free(FMQ[i].json);
	NFMQ = 0;
}

static dom_document *html_parse(const char *p, size_t n)
{
	dom_hubbub_parser_params params = {.enc = utf8_ok(p, n) ? "UTF-8" : NULL, .fix_enc = true, .enable_script = false};
	dom_hubbub_parser *parser = NULL;
	dom_document *doc = NULL;
	if (dom_hubbub_parser_create(&params, &parser, &doc) != DOM_HUBBUB_OK || !parser)
		return NULL;
	dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)(p ? p : ""), n);
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	return doc;
}

static void frame_load(int idx, const char *srcdoc)
{
	Frame *f = &FR[idx];
	f->tried = true;
	Buf b = {0};
	long code = 200;
	u64 t0 = osGetTime();
	if (srcdoc) {
		b.p = strdup(srcdoc), b.n = b.p ? strlen(b.p) : 0;
	} else if (fetch_any(f->url, "text/html,application/xhtml+xml,*/*;q=0.8", &b, &code) != DLF_OK || code >= 400) {
		util_log("web: iframe %.100s -> HTTP %ld: no", f->url, code);
		free(b.p);
		return;
	} else if (FA_FIN[0])
		util_strlcpy(f->url, FA_FIN, sizeof f->url);
	f->doc = html_parse(b.p, b.n);
	free(b.p);
	if (!f->doc)
		return;
	// su CSS (respecto a su dirección y con su ancho)
	bool keep = KEEP_ON;
	KEEP_ON = false, DOC_BASE = f->url, DOC_W = f->w;
	f->css = css_build(f->doc, NULL);
	DOC_BASE = NULL, DOC_W = 0, KEEP_ON = keep;
	// su JavaScript, con su ventana (la idx+1) y su tamaño
	u32 fr = mem_free();
	size_t lim = fr > 16u * 1024 * 1024 ? 10u * 1024 * 1024 : fr > 8u * 1024 * 1024 ? fr - 6u * 1024 * 1024 : 0;
	if (B.js && lim) {
		JsHost h = {.mem_limit = lim, .dark = B.dark, .fetch = js_fetch, .log = js_log, .url = f->url, .ua = UA_PC, .width = f->w, .height = f->h,
		            .budget_ms = 15000, .cancel = &B.cancel, .cookie_get = ck_get, .cookie_set = ck_set, .rect = js_rect,
		            .frame = idx + 1, .frame_of = js_frame_of, .post = js_post, .image = js_image};
		JsResult jr;
		f->jsp = jsdom_open(f->doc, &h, &jr);
		util_log("web: iframe %.100s: %d scripts, %d con error, %u ms", f->url, jr.scripts, jr.errors, (unsigned)(osGetTime() - t0));
		if (jr.oom) {
			jsdom_close(f->jsp);
			f->jsp = NULL;
		}
	}
}

// los <iframe> de la página que se ven y aún no se cargaron (también los que crea su JavaScript): true si hay nuevos
static bool frames_sync(void)
{
	if (!B.doc || B.light || !strcmp(B.reader_url, B.url) || B.abort)
		return false;
	bool added = false;
	dom_node_internal *st[96];
	int sp = 0;
	st[sp++] = (dom_node_internal *)B.doc;
	while (sp && NFR < FR_MAX && !B.abort) {
		dom_node_internal *n = st[--sp];
		for (dom_node_internal *c = n->last_child; c; c = c->previous)
			if (c->type == DOM_ELEMENT_NODE && sp < 96)
				st[sp++] = c;
		if (n->type != DOM_ELEMENT_NODE)
			continue;
		char tag[8];
		tag_of((dom_node *)n, tag, sizeof tag);
		if (strcmp(tag, "iframe"))
			continue;
		bool known = false;
		for (int i = 0; i < NFR && !known; i++)
			known = FR[i].el == (dom_node *)n;
		float r[4];
		if (known || !js_rect((dom_node *)n, r) || r[2] < 20 || r[3] < 20) // (sin colocar o de juguete: fuera)
			continue;
		char src[1024] = "", *srcdoc = NULL;
		if (!attr((dom_node *)n, "src", src, sizeof src) || !src[0] || !strncasecmp(src, "about:", 6) || !strncasecmp(src, "javascript:", 11)) {
			src[0] = 0;
			dom_string *k = NULL, *v = NULL; // srcdoc="<html>…": la página va en el atributo
			if (dom_string_create((const uint8_t *)"srcdoc", 6, &k) == DOM_NO_ERR) {
				if (dom_element_get_attribute((dom_element *)n, k, &v) == DOM_NO_ERR && v) {
					srcdoc = strndup(dom_string_data(v), dom_string_byte_length(v));
					dom_string_unref(v);
				}
				dom_string_unref(k);
			}
			if (!srcdoc)
				continue;
		}
		Frame *f = &FR[NFR];
		memset(f, 0, sizeof *f);
		f->el = (dom_node *)n, f->w = r[2], f->h = r[3];
		if (src[0])
			resolve(B.url, src, f->url, sizeof f->url);
		else
			util_strlcpy(f->url, B.url, sizeof f->url);
		if (src[0] && blocked_host(f->url)) {
			free(srcdoc);
			continue;
		}
		NFR++;
		if (mem_free() > 10u * 1024 * 1024)
			frame_load(NFR - 1, srcdoc);
		free(srcdoc);
		added = true;
	}
	return added;
}

// tras el JavaScript de la página o de un iframe: sus mensajes y los iframes nuevos; true si hay que colocar otra vez
static bool frames_after_js(void)
{
	bool ch = frames_pump();
	if (frames_sync())
		ch = true, frames_pump();
	return ch;
}

static void load_thread_run(void *arg);
static void load_thread(void *arg)
{
	load_thread_run(arg);
	if (!B.js_defer || B.abort)
		pf_stop(), B.js_defer = false; // (lo que sobró de lo pedido por adelantado)
	if (B.abort || !B.css_sig)
		keep_free(); // (las hojas guardadas: solo con JavaScript vivo)
	B.thr_done = true; // ya se puede reutilizar (o soltar) sin esperar
}

static void load_thread_run(void *arg)
{
	(void)arg;
	browser_thread_net();
	Buf b = {0};
	char hcs[40] = "";
	u64 t0 = osGetTime(), t_dl = 0, t_parse = 0, t_js = 0; // (cuánto tarda cada parte, al registro)
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
		// el juego de caracteres de la cabecera (manda sobre el <meta> de la página, como en HTML)
		const char *cs = dl_last_ctype();
		while (*cs && strncasecmp(cs, "charset=", 8))
			cs++;
		if (*cs) {
			cs += 8;
			cs += *cs == '"' || *cs == '\'';
			snprintf(hcs, sizeof hcs, "%.*s", (int)strcspn(cs, "\"'; "), cs);
		}
	}
	if (B.abort) { // se dejó mientras se bajaba
		free(b.p);
		return;
	}
	B.bytes = b.n;
	t_dl = osGetTime();
	if (!b.n)
		util_log("web: %s vacía (HTTP %d)", B.url, B.http_code);
	B.oom = OOM_NONE;
	cssprep_set_dark(B.dark); // (prefers-color-scheme: dark): las webs con tema oscuro lo usan
	B.css_dark = B.dark;
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
	// UTF-8 válido, se le dice; si no, el de la cabecera HTTP (RTVE: ISO-8859-1, aunque su <meta> diga
	// UTF-8), y si tampoco, que lo detecte él. (latin1 e ISO-8859-1 son windows-1252, como en HTML)
	const char *enc = utf8_ok(b.p, b.n) ? "UTF-8" : NULL;
	if (!enc && hcs[0] && strcasecmp(hcs, "utf-8") && strcasecmp(hcs, "utf8"))
		enc = !strcasecmp(hcs, "iso-8859-1") || !strcasecmp(hcs, "latin1") || !strcasecmp(hcs, "iso8859-1") || !strcasecmp(hcs, "us-ascii") ? "windows-1252" : hcs;
	dom_hubbub_parser_params params = {.enc = enc, .fix_enc = true, .enable_script = false};
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
		if (!b.n || B.abort)
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
	t_parse = osGetTime();
	// su JavaScript, antes del CSS: lo que añada o cambie (también hojas de estilo) ya cuenta
	B.js_nav[0] = 0;
	// el JavaScript, con lo que quede (dejando sitio para colocar la página)
	u32 fr = mem_free(), lay = (u32)(b.n * 4) + MEM_RESERVE;
	size_t js_lim = fr > lay + 2u * 1024 * 1024 ? fr - lay : 0;
	if (js_lim > 24u * 1024 * 1024)
		js_lim = 24u * 1024 * 1024;
	B.nojs = nojs_find(B.url) >= 0; // (apagado para esta web: botón «JS»)
	B.js_scripts = B.js_errors = 0;
	// los scripts y las hojas, a bajarse ya, todos a la vez (los usan el JavaScript y el CSS de abajo)
	bool js_run = doc && B.js && !B.nojs && !reader && !B.light && js_lim && !B.abort; // (en modo ligero, sin JavaScript)
	if (doc && !reader && !B.abort)
		pf_start(doc, js_run);
	if (doc && B.js && !B.nojs && !reader && !B.light && !js_lim && B.oom == OOM_NONE)
		B.oom = OOM_JS, B.oom_need = lay + 2u * 1024 * 1024;
	// si la página ya trae su texto, se enseña así y su JavaScript va después, con ella a la vista (y se
	// vuelve a colocar si cambia algo); si es un cascarón que llena el JavaScript, primero el JavaScript
	size_t txt = 0;
	if (js_run)
		page_text((dom_node *)doc, 0, &txt, 1500);
	B.js_defer = js_run && txt >= 1500;
	B.js_lim = js_lim;
	KEEP_ON = js_run; // (con JavaScript: las hojas se guardan, por si trae clases nuevas)
	if (js_run && !B.js_defer) {
		JsHost h = {.mem_limit = js_lim, .dark = B.dark, .fetch = js_fetch, .log = js_log, .url = B.url, .ua = UA_PC, .width = VW_PC, .budget_ms = 20000, .cancel = &B.cancel,
		            .cookie_get = ck_get, .cookie_set = ck_set, .rect = js_rect,
		            .height = 600, .frame = 0, .frame_of = js_frame_of, .post = js_post, .image = js_image};
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
	t_js = osGetTime();
	// el CSS de la página (se baja y se lee aquí, en el hilo, para no parar la pantalla)
	size_t bytes = 0;
	BrCss *css = doc && !reader ? css_build(doc, &bytes) : NULL;
	if (css) {
		B.ncss = brcss_sheets(css);
		util_log("web: %d hojas CSS, %u bytes", B.ncss, (unsigned)bytes);
	}
	B.css_sig = css && js_run ? css_sig(doc) : 0;
	util_log("web: tiempos: HTML %u ms, leer %u, JavaScript %u, CSS %u (%d por adelantado)", (unsigned)(t_dl - t0), (unsigned)(t_parse - t_dl),
	         (unsigned)(t_js - t_parse), (unsigned)(osGetTime() - t_js), PF.n);
	if (B.js_defer)
		util_log("web: con texto (%u letras): su JavaScript, con la página a la vista", (unsigned)txt);
	B.css = css;
	B.doc = doc;
	B.nel_total = doc ? count_nodes((dom_node *)doc) : 0;
	B.st = ST_PARSED;
}

enum { PEND_NONE, PEND_LOAD, PEND_CLOSE, PEND_HOME };
static bool br_busy(void)
{
	return B.thr && !B.thr_done;
}

// dejar lo que está cargando (se corta en el próximo punto que lo mire) y hacer «p» al soltarse
static void br_defer(int p, const char *url)
{
	B.abort = true;
	B.cancel = true;
	B.pend = p;
	if (url)
		util_strlcpy(B.pend_url, url, sizeof B.pend_url);
}

static void start_load(const char *url)
{
	if (br_busy()) { // cargando otra: no se espera (la pantalla no se para); se hace al soltarse
		br_defer(PEND_LOAD, url);
		return;
	}
	B.abort = false;
	B.shown = false; // (otra página: hasta que se vea, la pantalla de carga)
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp); // antes que el documento: tiene sus nodos
	B.jsp = NULL;
	B.js_defer = false;
	pf_stop();
	keep_free();
	geo_free();
	frames_free();
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
	B.thr_done = false, B.thr = threadCreate(load_thread, NULL, 512 * 1024, prio + 1, br_core(), false); // hubbub y libdom anidan mucho en páginas grandes
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
	bool flexrow;  /* cs es un contenedor flex en fila (para justify-content / align-items) */
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
	f->bgi = -1;
	f->fit = 0;
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
	// una palabra más ancha que su columna se sale (como en los navegadores); solo las enormes
	// (direcciones largas…) van a trozos
	while (w > fmaxf(CUR_R - CUR_L, 200) && n > 1) {
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
	B.img_quit = true;
	for (int k = 0; k < 3; k++)
		if (B.img_thr[k]) {
			threadJoin(B.img_thr[k], U64_MAX);
			threadFree(B.img_thr[k]);
			B.img_thr[k] = NULL;
		}
	B.img_quit = false;
	for (int i = 0; i < B.nimg; i++) // (las que se quedaron a medias: otra vez a la cola)
		if (B.img[i].st == IM_LOAD)
			B.img[i].st = IM_WAIT;
}

static void img_free_all(void)
{
	img_stop();
	for (int i = 0; i < B.nimg; i++) {
		free(B.img[i].url);
		free(B.img[i].rgba);
		free(B.img[i].data);
		tex_free(&B.img[i].tex);
	}
	free(B.img);
	B.img = NULL;
	B.nimg = B.cimg = 0;
	B.img_px = 0;
}

static int fetch_any(const char *url, const char *accept, Buf *b, long *code);

// las fuentes de la página para los iconos (solo las usa el hilo de las imágenes): las últimas 4, de hasta 400 KB
static struct {
	char url[512];
	Buf b;
	bool bad;
	u32 used;
} FONTS[4];
static u32 FONT_T;

static const Buf *font_get(const char *url)
{
	int lru = 0;
	for (int i = 0; i < 4; i++) {
		if (!strcmp(FONTS[i].url, url)) {
			FONTS[i].used = ++FONT_T;
			return FONTS[i].bad ? NULL : &FONTS[i].b;
		}
		if (FONTS[i].used < FONTS[lru].used)
			lru = i;
	}
	free(FONTS[lru].b.p);
	memset(&FONTS[lru], 0, sizeof FONTS[lru]);
	util_strlcpy(FONTS[lru].url, url, sizeof FONTS[lru].url);
	FONTS[lru].used = ++FONT_T;
	long code = 0;
	int r = blocked_host(url) ? DLF_FAIL : fetch_any(url, "font/ttf,font/woff,*/*;q=0.5", &FONTS[lru].b, &code);
	util_log("web: fuente %.120s -> %d, HTTP %ld, %u bytes", url, r, code, (unsigned)FONTS[lru].b.n);
	if (r != DLF_OK || code >= 400 || FONTS[lru].b.n < 12 || FONTS[lru].b.n > 400 * 1024) {
		free(FONTS[lru].b.p);
		FONTS[lru].b.p = NULL, FONTS[lru].b.n = 0;
		FONTS[lru].bad = true;
		return NULL;
	}
	return &FONTS[lru].b;
}

// baja y decodifica, primero las que están más cerca de lo que se ve
static LightLock IMG_LK, FONT_LK;
static bool IMG_LK_OK;
#ifndef IMG_NTHR
#define IMG_NTHR 3
#endif
static int IMG_LIVE;    /* hilos de imágenes trabajando */
static u64 IMG_T0;      /* cuándo empezaron (para el registro) */

static void img_thread(void *arg)
{
	(void)arg;
	browser_thread_net();
	for (;;) {
		if (B.img_quit) {
			LightLock_Lock(&IMG_LK);
			IMG_LIVE--;
			LightLock_Unlock(&IMG_LK);
			return;
		}
		int best = -1;
		float bd = 1e30f, vy = B.sy;
		LightLock_Lock(&IMG_LK); // (la más cerca de la vista que nadie haya cogido)
		for (int i = 0; i < B.nimg; i++)
			if (B.img[i].st == IM_WAIT) {
				float d = fabsf(B.img[i].y - vy);
				if (d < bd)
					bd = d, best = i;
			}
		if (best >= 0)
			B.img[best].st = IM_LOAD;
		bool last = best < 0 && --IMG_LIVE == 0;
		LightLock_Unlock(&IMG_LK);
		if (last) {
			int ok = 0;
			for (int i = 0; i < B.nimg; i++)
				ok += B.img[i].st == IM_RGBA || B.img[i].st == IM_TEX;
			util_log("web: imágenes: %d de %d en %u ms", ok, B.nimg, (unsigned)(osGetTime() - IMG_T0));
		}
		if (best < 0)
			return;
		PImg *im = &B.img[best];
		Buf b = {0};
		long code = 0;
		int r;
		if (im->data) { // un <svg> de la página: ya está aquí
			b.p = im->data, b.n = im->dn, im->data = NULL;
			r = DLF_OK, code = 200;
		} else if (!strncmp(im->url, "data:", 5)) { // data:image/…;base64,…
			const char *c = strstr(im->url, ";base64,");
			r = DLF_FAIL;
			if (c) {
				size_t l = strlen(c + 8);
				b.p = malloc(l * 3 / 4 + 4);
				if (b.p) {
					b.n = util_b64_decode(c + 8, l, (u8 *)b.p);
					r = DLF_OK, code = 200;
				}
			}
		} else if (!strncmp(im->url, "glyph:", 6)) { // un icono: su glifo, de la fuente (guardada) de la página
			unsigned rgb = 0, cp = 0;
			const char *fu = NULL;
			if (sscanf(im->url + 6, "%x:%x:", &rgb, &cp) == 2 && (fu = strchr(strchr(im->url + 6, ':') + 1, ':')))
				fu++;
			LightLock_Lock(&FONT_LK); // (las fuentes guardadas son de todos los hilos de imágenes)
			const Buf *fb = fu ? font_get(fu) : NULL;
			u8 *gp = NULL;
			int gw = 0, gh = 0;
			bool gok = fb && webfont_glyph((const u8 *)fb->p, fb->n, cp, 48, rgb, &gp, &gw, &gh);
			LightLock_Unlock(&FONT_LK);
			if (gok) {
				im->rgba = gp, im->tw = gw, im->th = gh, im->iw = gw, im->ih = gh;
				LightLock_Lock(&IMG_LK);
				B.img_px += (size_t)gw * gh;
				LightLock_Unlock(&IMG_LK);
				im->st = IM_RGBA;
			} else {
				im->st = IM_FAIL;
				util_log("web: icono %x de %.100s: no se pudo (%s)", cp, fu ? fu : "-", fb ? "glifo" : "fuente");
			}
			continue;
		} else
			r = blocked_host(im->url) ? DLF_FAIL : fetch_any(im->url, "image/png,image/jpeg,image/gif,image/*;q=0.5", &b, &code);
		int mw = im->natural ? 512 : (int)fminf(512, fmaxf(16, im->w * 1.5f)), mh = im->natural ? 512 : (int)fminf(512, fmaxf(16, im->h * 1.5f));
		if (b.p && b.n)
			img_mem_info((const u8 *)b.p, b.n, &im->ow, &im->oh);
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
			LightLock_Lock(&IMG_LK);
			B.img_px += (size_t)w * h;
			LightLock_Unlock(&IMG_LK);
			im->st = IM_RGBA;
		} else {
			im->st = IM_FAIL;
			util_log("web: imagen %.120s -> %d, HTTP %ld, %u bytes, %.4s", strncmp(im->url, "data:", 5) ? im->url : "data:…", r, code, (unsigned)b.n,
			         b.p && b.n >= 12 ? (!memcmp(b.p + 8, "WEBP", 4) ? "webp" : !memcmp(b.p + 4, "ftyp", 4) ? "avif" : b.p) : "-");
		}
		free(b.p);
	}
}

static int next_pow2i(int v)
{
	int p = 8;
	while (p < v)
		p <<= 1;
	return p;
}
// los <canvas> que cambiaron: sus píxeles a su textura (cada 3 fotogramas como mucho; reducidos si son grandes)
static void cv_upload(void)
{
	static int tick, still, next;
	static float last_sy, last_z;
	// moviéndose por la página (o con zoom): casi nada, para que vaya suave; quieta, cada 3 fotogramas
	if (B.sy != last_sy || B.zoom != last_z)
		still = 0, last_sy = B.sy, last_z = B.zoom;
	else if (still < 1000)
		still++;
	if (++tick % (still < 20 ? 20 : 3) || !B.nimg)
		return;
	for (int n = 0; n < B.nimg; n++) { // (uno por vez, por turnos: cada subida es un buen trozo de fotograma)
		int i = (next + n) % B.nimg;
		PImg *im = &B.img[i];
		if (im->st != IM_TEX || strncmp(im->url, "canvas:", 7))
			continue;
		int w = 0, h = 0;
		u8 *px = cv_take(atoi(im->url + 7), &w, &h, !im->tex.ok);
		if (!px)
			continue;
		float k = fminf(1, fminf(1024.0f / w, 1024.0f / h));
		int tw = (int)(w * k), th = (int)(h * k);
		while (tw > 8 && th > 8 && (long)next_pow2i(tw) * next_pow2i(th) > 256 * 1024) // (hasta 512x512: 1 MB de memoria lineal)
			tw = tw * 9 / 10, th = th * 9 / 10;
		tw = tw < 1 ? 1 : tw, th = th < 1 ? 1 : th;
		if (tw != w || th != h) { // (al tamaño de la textura: el más cercano)
			u8 *d = malloc((size_t)tw * th * 4);
			if (d)
				for (int y = 0; y < th; y++)
					for (int x = 0; x < tw; x++)
						memcpy(d + ((size_t)y * tw + x) * 4, px + ((size_t)(y * h / th) * w + x * w / tw) * 4, 4);
			free(px);
			px = d;
		}
		if (px && !tex_from_rgba(&im->tex, px, tw, th))
			im->st = IM_FAIL;
		im->iw = tw, im->ih = th, im->ow = w, im->oh = h;
		free(px);
		B.page_ver++;
		next = i + 1;
		return;
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
	if (!IMG_LK_OK)
		LightLock_Init(&IMG_LK), LightLock_Init(&FONT_LK), IMG_LK_OK = true;
	IMG_LIVE = 0, IMG_T0 = osGetTime();
	for (int k = 0; k < IMG_NTHR; k++) {
		LightLock_Lock(&IMG_LK);
		IMG_LIVE++;
		LightLock_Unlock(&IMG_LK);
		B.img_thr[k] = threadCreate(img_thread, NULL, 64 * 1024, prio + 2, br_core(), false);
		if (!B.img_thr[k]) {
			LightLock_Lock(&IMG_LK);
			IMG_LIVE--;
			LightLock_Unlock(&IMG_LK);
		}
	}
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
		// fuera de la pantalla a propósito (left: -9999px, top: -1000px…): enlaces «saltar al
		// contenido», menús que se deslizan…
		if ((css_computed_left(cs, &v, &u) == CSS_LEFT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) < -400) ||
		    (css_computed_top(cs, &v, &u) == CSS_TOP_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) < -400) ||
		    (css_computed_right(cs, &v, &u) == CSS_RIGHT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, B.vw) < -400))
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

// el texto que se ve de un elemento (el de un <button>): sin lo que su CSS esconde (display: none,
// visibility, y lo de «solo para lectores de pantalla»: absoluto de 1 px o recortado)
static void vis_text(dom_node *n, const css_computed_style *cs, char *out, size_t sz, size_t *k, bool *sp)
{
	for (dom_node_internal *c = ((dom_node_internal *)n)->first_child; c && *k < sz - 1; c = c->next) {
		if (c->type == DOM_TEXT_NODE) {
			dom_string *t = NULL;
			if (dom_characterdata_get_data((dom_characterdata *)c, &t) != DOM_NO_ERR || !t)
				continue;
			const char *p = dom_string_data(t);
			for (size_t i = 0, l = dom_string_byte_length(t); i < l && *k < sz - 1; i++) {
				bool w = isspace((unsigned char)p[i]);
				if (w && !*sp)
					out[(*k)++] = ' ';
				else if (!w)
					out[(*k)++] = p[i];
				*sp = w;
			}
			dom_string_unref(t);
		} else if (c->type == DOM_ELEMENT_NODE) {
			css_computed_style *ccs = B.css ? brcss_style(B.css, (dom_node *)c, cs, PAGE_W) : NULL;
			bool hide = false;
			if (ccs) {
				css_fixed v;
				css_unit u;
				css_computed_clip_rect r;
				hide = css_computed_display(ccs, false) == CSS_DISPLAY_NONE || css_computed_visibility(ccs) != CSS_VISIBILITY_VISIBLE;
				if (!hide && css_computed_position(ccs) == CSS_POSITION_ABSOLUTE)
					hide = css_computed_clip(ccs, &r) == CSS_CLIP_RECT ||
					       (css_computed_width(ccs, &v, &u) == CSS_WIDTH_SET && u != CSS_UNIT_PCT && brcss_px(ccs, v, u, PAGE_W) <= 2) ||
					       (css_computed_height(ccs, &v, &u) == CSS_HEIGHT_SET && u != CSS_UNIT_PCT && brcss_px(ccs, v, u, PAGE_W) <= 2);
			}
			if (!hide)
				vis_text((dom_node *)c, ccs ? ccs : cs, out, sz, k, sp);
			if (ccs)
				css_computed_style_destroy(ccs);
		}
	}
}

static void vis_text_of(dom_node *n, const css_computed_style *cs, char *out, size_t sz)
{
	size_t k = 0;
	bool sp = true;
	out[0] = 0;
	vis_text(n, cs, out, sz, &k, &sp);
	while (k && out[k - 1] == ' ')
		k--;
	out[k] = 0;
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

// un control de formulario: se apunta en B.wg; su tamaño (con letra sc y un ancho disponible). -1 si no se ve
static int wdg_make(dom_node *node, const char *tag, float sc, const css_computed_style *cs, float avail, float *ow, float *oh)
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
		return -1;
	else
		w.type = WG_TEXT;
	FVal *v = fv_get(node, false);
	if (w.type == WG_BUTTON) {
		if (!strcmp(tag, "button")) {
			if (cs)
				vis_text_of(node, cs, w.label, sizeof w.label);
			else
				text_of(node, w.label, sizeof w.label);
		}
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
	float lh = text_lineh(sc), bw, bh = lh * 1.5f;
	css_fixed cv;
	css_unit cu;
	char a[16];
	if (w.type == WG_CHECK || w.type == WG_RADIO)
		bw = bh = lh;
	else if (css_computed_width(cs, &cv, &cu) == CSS_WIDTH_SET)
		bw = len_px(cs, cv, cu, avail);
	else if (w.type == WG_BUTTON || w.type == WG_SELECT)
		bw = text_width(w.label[0] ? w.label : "Enviar", sc) + lh * (w.type == WG_SELECT ? 2.2f : 1.4f);
	else if (w.type == WG_AREA)
		bw = (attr(node, "cols", a, sizeof a) ? atoi(a) : 30) * text_width("n", sc) + 12;
	else
		bw = attr(node, "size", a, sizeof a) && atoi(a) > 0 ? atoi(a) * text_width("n", sc) + 12 : 180;
	if (w.type == WG_AREA)
		bh = (attr(node, "rows", a, sizeof a) && atoi(a) > 0 ? atoi(a) : 2) * lh + 8;
	bw = clampf(bw, 12, avail > 40 ? avail : 400);
	if (B.nwg == B.cwg) {
		int nc = B.cwg ? B.cwg * 2 : 16;
		Wdg *nw = realloc(B.wg, nc * sizeof *nw);
		if (!nw)
			return -1;
		B.wg = nw, B.cwg = nc;
	}
	B.wg[B.nwg] = w;
	*ow = bw, *oh = bh;
	return B.nwg++;
}

static void widget(dom_node *node, const char *tag, const St *st, const css_computed_style *cs)
{
	float bw, bh;
	int k = wdg_make(node, tag, st->sc, cs, RIGHT - L_indent, &bw, &bh);
	if (k < 0)
		return;
	int nf0 = B.nf;
	put_box(bw, bh, st, -1);
	if (B.nf > nf0)
		B.f[B.nf - 1].wdg = (s16)k;
	else
		B.nwg--;
}

static void walk_css(dom_node *node, const char *tag, St st, const css_computed_style *cs, uint8_t d);
static bool css_invisible(const css_computed_style *cs);
static bool is(const char *t, const char *const *list);
static float measure(dom_node *n, float sc, float cap, int depth);
static void line_at(float y);
static float cur_bottom(void);
static const char *const NEVER_TAGS[] = {"script", "style", "noscript", "template", "svg", "iframe", "object", "embed", "canvas", "video", "audio", "head", NULL};


// los títulos, con su letra más grande (lo habitual: 2, 1,5, 1,17… veces), para medirlos
static float head_scale(const char *tag)
{
	static const float K[6] = {2.0f, 1.5f, 1.25f, 1.1f, 1.0f, 1.0f};
	return tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2] ? K[tag[1] - '1'] : 1.0f;
}

static float sv_len(dom_node *n, const char *name, float em);

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
			if (!strcmp(tag, "svg")) { // un dibujo de la página: su ancho (atributo, o alto × proporción)
				char vb[96];
				float w = sv_len((dom_node *)c, "width", 16), h = sv_len((dom_node *)c, "height", 16);
				if (w <= 0 && attr((dom_node *)c, "viewbox", vb, sizeof vb)) {
					float a[4] = {0};
					sscanf(vb, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &a[0], &a[1], &a[2], &a[3]);
					w = a[3] > 0 ? (h > 0 ? h : 16) * a[2] / a[3] : 16;
				}
				sum += w > 0 ? w : 16;
				continue;
			}
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
			float m = measure((dom_node *)c, sc * head_scale(tag), cap, depth + 1);
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
				mx = fmaxf(mx, min_word((dom_node *)c, sc * head_scale(tag), depth + 1));
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
		bool fixed, span, brk;
		int32_t order;
		int gstart, gspan; /* rejilla: columna de inicio (1…, 0 la siguiente) y cuántas ocupa */
		float xoff;        /* rejilla: hueco a su izquierda (columnas que se salta) */
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
			// lo medido no cuenta los márgenes de lo de dentro (enlaces de un menú separados): un poco de holgura
			w = measure((dom_node *)ch, sc, avail, 0) * 1.12f + clampf(ex, 0, 80) + 8;
		}
		c[n].minw = fixed ? 0 : fminf(min_word((dom_node *)ch, sc, 0) + 10, avail * 0.6f);
		c[n].n = (dom_node *)ch, c[n].cs = cs, c[n].d = d;
		util_strlcpy(c[n].tag, tag, sizeof c[n].tag);
		c[n].w = clampf(w, 0, avail);
		c[n].grow = css_computed_flex_grow(cs, &fl) == CSS_FLEX_GROW_SET ? FIXTOFLT(fl) : 0;
		c[n].fixed = fixed;
		c[n].order = 0;
		css_computed_order(cs, &c[n].order);
		c[n].span = css_computed_column_span(cs) == CSS_COLUMN_SPAN_ALL;
		{ // de cssprep: z-index = 100000 + columna de inicio * 100 + cuántas ocupa
			int32_t z = 0;
			c[n].gstart = 0, c[n].gspan = 1;
			if (css_computed_z_index(cs, &z) == CSS_Z_INDEX_SET) {
				if (z >= 100000 * 1024)
					z = FIXTOINT(z);
				if (z >= 100000 && z < 200000)
					c[n].gstart = (z - 100000) / 100, c[n].gspan = (z - 100000) % 100;
			}
			if (c[n].gspan < 1)
				c[n].gspan = 1;
		}
		c[n].brk = false, c[n].xoff = 0;
		n++;
	}
	// order (flex) y grid-area (su orden, de cssprep): se colocan en ese orden, estable
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && c[j - 1].order > c[j].order; j--) {
			typeof(c[0]) t = c[j];
			c[j] = c[j - 1], c[j - 1] = t;
		}
	/* rejilla con columnas (grid-template-columns, pasado por cssprep a column-count: cuántas;
	 * column-width: ancho fijo de la primera; column-rule-width: el de la última): cada hijo, su
	 * columna; los que ocupan toda la fila (column-span: all), solos */
	int gcols = 0;
	float gw[16];
	if (st.cs && css_computed_display(st.cs, false) == CSS_DISPLAY_GRID) {
		int32_t k = 0;
		if (css_computed_column_count(st.cs, &k) == CSS_COLUMN_COUNT_SET && k > 0) {
			k = FIXTOINT(k); // libcss lo guarda en coma fija
			gcols = k < 1 ? 1 : k > 16 ? 16 : k;
		}
	}
	if (gcols) {
		css_fixed v;
		css_unit u;
		float first = -1, last = -1, room = avail - gapx * (gcols - 1);
		if (css_computed_column_width(st.cs, &v, &u) == CSS_COLUMN_WIDTH_SET)
			first = brcss_px(st.cs, v, u, B.vw);
		if (gcols > 1 && css_computed_column_rule_style(st.cs) == CSS_COLUMN_RULE_STYLE_SOLID &&
		    css_computed_column_rule_width(st.cs, &v, &u) == CSS_COLUMN_RULE_WIDTH_WIDTH) // solo la de cssprep
			last = brcss_px(st.cs, v, u, B.vw);
		int flex = gcols;
		float fixed_w = 0;
		if (first > 0 && first < room)
			fixed_w += first, flex--;
		if (last > 0 && fixed_w + last < room)
			fixed_w += last, flex--;
		float each = flex > 0 ? (room - fixed_w) / flex : 0;
		for (int i = 0; i < gcols; i++)
			gw[i] = each;
		if (first > 0 && first < room)
			gw[0] = first;
		if (last > 0 && gcols > 1 && gw[gcols - 1] == each)
			gw[gcols - 1] = last;
		// el ancho de cada pista (de cssprep, en counter-reset: «g1 200 g2 -10…»: px, o -10 por fr)
		const css_computed_counter *ctr = NULL;
		if (css_computed_counter_reset(st.cs, &ctr) == CSS_COUNTER_RESET_NAMED && ctr) {
			int tw[16];
			bool have = false;
			for (int i = 0; i < gcols; i++)
				tw[i] = -10;
			for (; ctr->name; ctr++) {
				const char *nm = lwc_string_data(ctr->name);
				if (nm[0] == 'g' && isdigit((unsigned char)nm[1])) {
					int k2 = atoi(nm + 1) - 1;
					if (k2 >= 0 && k2 < gcols)
						tw[k2] = FIXTOINT(ctr->value), have = true;
				}
			}
			if (have) {
				float fixed = 0, frs = 0;
				for (int i = 0; i < gcols; i++)
					if (tw[i] > 0)
						fixed += tw[i];
					else
						frs += -tw[i];
				float k3 = fixed > room ? room / fixed : 1; // no caben: los fijos, a escala
				float rest = fmaxf(0, room - fixed * k3);
				for (int i = 0; i < gcols; i++)
					gw[i] = tw[i] > 0 ? tw[i] * k3 : (frs > 0 ? rest * -tw[i] / frs : 0);
			}
		}
		// cada hijo: su columna (la siguiente libre o la que pida) y su ancho (las que ocupe)
		int col = 0;
		for (int i = 0; i < n; i++) {
			if (c[i].span) {
				c[i].w = avail, c[i].brk = true, col = gcols; // solo en su fila
				continue;
			}
			int sp = c[i].gspan < 1 ? 1 : c[i].gspan > gcols ? gcols : c[i].gspan;
			// colocación automática: las pistas estrechas (separaciones de 2-3 rem) no son para contenido
			if (c[i].gstart <= 0)
				while (col < gcols && gw[col] < 48 && gcols > 1)
					col++;
			// red de seguridad: lo colocado solo que cae en una pista estrecha (rejillas que no se han
			// entendido del todo) ocupa toda la fila: apilado se lee; en 50 px, no
			if (c[i].gstart <= 0 && sp == 1 && col < gcols && gw[col] < 160 && gcols > 1) {
				c[i].span = true, c[i].w = avail, c[i].brk = true, col = gcols;
				continue;
			}
			int want = c[i].gstart > 0 ? c[i].gstart - 1 : col;
			if (want + sp > gcols)
				want = c[i].gstart > 0 && c[i].gstart - 1 + sp <= gcols ? c[i].gstart - 1 : 0;
			if (want < col || col >= gcols || (c[i].gstart <= 0 && col + sp > gcols)) // no cabe en esta fila
				c[i].brk = true, col = 0;
			if (i == 0)
				c[i].brk = true;
			float off = 0;
			for (int k = col; k < want; k++)
				off += gw[k] + gapx;
			c[i].xoff = off;
			col = want;
			float w = 0;
			for (int k = col; k < col + sp && k < gcols; k++)
				w += gw[k] + (k > col ? gapx : 0);
			c[i].w = w, c[i].fixed = true, c[i].grow = 0;
			col += sp;
		}
	}
	// a filas (si se puede partir) y en cada una: repartir lo que sobra o encoger
	float top = (L_has || !B.nl) ? cur_bottom() : B.l[B.nl - 1].y;
	int i0 = 0;
	float o_ind = L_indent, o_right = L_right;
	while (i0 < n) {
		int i1 = i0;
		float tot = 0, grow = 0;
		while (i1 < n && (i1 == i0 || (gcols ? !c[i1].brk && !c[i0].span : (!wrap || tot + gapx + c[i1].w <= avail)))) {
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
		// justify-content: dónde va lo que sobra (si nada crece)
		float x = st.indent, maxb = top, gap_extra = 0;
		float left = avail;
		for (int i = i0; i < i1; i++)
			left -= c[i].w + (i > i0 ? gapx : 0);
		uint8_t jc = st.cs && st.flexrow ? css_computed_justify_content(st.cs) : CSS_JUSTIFY_CONTENT_FLEX_START;
		int cnt = i1 - i0;
		if (left > 1 && grow <= 0) {
			if (jc == CSS_JUSTIFY_CONTENT_CENTER)
				x += left / 2;
			else if (jc == CSS_JUSTIFY_CONTENT_FLEX_END)
				x += left;
			else if (jc == CSS_JUSTIFY_CONTENT_SPACE_BETWEEN && cnt > 1)
				gap_extra = left / (cnt - 1);
			else if (jc == CSS_JUSTIFY_CONTENT_SPACE_AROUND)
				gap_extra = left / cnt, x += gap_extra / 2;
			else if (jc == CSS_JUSTIFY_CONTENT_SPACE_EVENLY)
				gap_extra = left / (cnt + 1), x += gap_extra;
		}
		struct {
			int l0, l1, b0, b1;
			float bot;
		} rng[MAX_COLS];
		for (int i = i0; i < i1; i++) {
			x += c[i].xoff; // rejilla: las columnas que se salta
			L_indent = x;
			L_right = x + c[i].w;
			L_align = st.align;
			line_at(top);
			rng[i].l0 = B.nl - 1, rng[i].b0 = B.nbg;
			St cs = st;
			cs.indent = x, cs.right = x + c[i].w, cs.cells = NULL, cs.fixw = true;
			if (c[i].cs)
				walk_css(c[i].n, c[i].tag, cs, c[i].cs, c[i].d);
			else
				walk(c[i].n, cs);
			rng[i].l1 = B.nl, rng[i].b1 = B.nbg;
			rng[i].bot = cur_bottom();
			maxb = fmaxf(maxb, rng[i].bot);
			x += c[i].w + gapx + gap_extra;
		}
		uint8_t ai = st.cs && st.flexrow ? css_computed_align_items(st.cs) : CSS_ALIGN_ITEMS_STRETCH;
		for (int i = i0; i < i1; i++) {
			// align-items: center / flex-end: se baja lo de cada columna respecto a la más alta
			float dy = ai == CSS_ALIGN_ITEMS_CENTER ? (maxb - rng[i].bot) / 2 : ai == CSS_ALIGN_ITEMS_FLEX_END ? maxb - rng[i].bot : 0;
			if (dy > 0.5f && cnt > 1) {
				for (int l = rng[i].l0 + (rng[i].l0 < 0); l < rng[i].l1; l++)
					if (l >= 0 && B.l[l].y >= top - 0.5f)
						B.l[l].y += dy;
				for (int k = rng[i].b0; k < rng[i].b1; k++)
					B.bg[k].y0 += dy, B.bg[k].y1 += dy;
			}
			// align-items: stretch: el fondo de cada columna (su caja, la última que se añadió) llega
			// hasta la más alta; lo de dentro (etiquetas, insignias…) se queda como está
			if (ai == CSS_ALIGN_ITEMS_STRETCH && rng[i].b1 > rng[i].b0) {
				BgBox *bb = &B.bg[rng[i].b1 - 1];
				if (bb->y0 <= top + 8)
					bb->y1 = fmaxf(bb->y1, maxb);
			}
		}
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

// hijos de un flex en columna con align-items: center: cada uno, a su ancho y centrado
static void col_center(dom_node *node, St st)
{
	float avail = st.right - st.indent;
	for (dom_node_internal *ch = NI(node)->first_child; ch; ch = ch->next) {
		if (ch->type == DOM_TEXT_NODE) {
			St s2 = st;
			s2.align = 1;
			walk((dom_node *)ch, s2);
			continue;
		}
		if (ch->type != DOM_ELEMENT_NODE)
			continue;
		char tag[16];
		tag_of((dom_node *)ch, tag, sizeof tag);
		if (is(tag, NEVER_TAGS) && strcmp(tag, "svg"))
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
		float w;
		if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET)
			w = len_px(cs, v, u, avail);
		else {
			css_computed_font_size(cs, &v, &u);
			float sc = clampf(SC16 * brcss_px(cs, v, u, B.vw) / 16, 0.15f, 2.4f);
			float ex = 0;
			if (css_computed_padding_left(cs, &v, &u) == CSS_PADDING_SET)
				ex += len_px(cs, v, u, avail);
			if (css_computed_padding_right(cs, &v, &u) == CSS_PADDING_SET)
				ex += len_px(cs, v, u, avail);
			w = !strcmp(tag, "svg") ? avail : measure((dom_node *)ch, sc, avail, 0) * 1.12f + clampf(ex, 0, 80) + 8;
		}
		w = clampf(w, 16, avail);
		float x = st.indent + (avail - w) / 2;
		St s2 = st;
		s2.indent = x, s2.right = x + w, s2.fixw = true, s2.cells = NULL;
		if (!strcmp(tag, "svg")) { // el dibujo, solo y centrado
			s2.align = 1, s2.indent = st.indent, s2.right = st.right;
			line_at(cur_bottom());
			L_align = 1;
			walk((dom_node *)ch, s2);
			line_new();
			L_align = st.align;
		} else {
			line_at(cur_bottom());
			walk_css((dom_node *)ch, tag, s2, cs, d);
		}
		css_computed_style_destroy(cs);
	}
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
		// una imagen de fondo (no se pinta) con texto claro encima: un fondo oscuro, para que se lea
		if (!bgc && strcmp(tag, "body") && strcmp(tag, "html")) {
			lwc_string *bi = NULL;
			css_color tcol;
			if (css_computed_background_image(cs, &bi) == CSS_BACKGROUND_IMAGE_IMAGE && bi && css_computed_color(cs, &tcol) == CSS_COLOR_COLOR) {
				float tl = (0.2126f * ((tcol >> 16) & 0xFF) + 0.7152f * ((tcol >> 8) & 0xFF) + 0.0722f * (tcol & 0xFF)) / 255;
				if (tl > 0.6f)
					bgc = col_hex(0x2B3038);
			}
		}
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
	// flex en columna con align-items: center: cada hijo centrado a su ancho (el centrado típico)
	bool colc = blk && !row && (d == CSS_DISPLAY_FLEX || d == CSS_DISPLAY_INLINE_FLEX) && css_computed_align_items(cs) == CSS_ALIGN_ITEMS_CENTER;
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
	if (colc)
		col_center(node, st);
	else if (row) {
		st.flexrow = d == CSS_DISPLAY_FLEX || d == CSS_DISPLAY_INLINE_FLEX;
		row_children(node, st, d == CSS_DISPLAY_GRID || css_computed_flex_wrap(cs) == CSS_FLEX_WRAP_WRAP, gapx);
		st.flexrow = false;
	}
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

static u32 dk_map(u32 c, bool fg);

/* ------------------------------------------------------------ <svg> de la propia página (logos, iconos) */
static void sv_put(Buf *b, const char *s, size_t n)
{
	if (n && buf_cb((char *)s, 1, n, b) != n)
		b->n = MAX_PAGE; // lleno: se deja
}
static void sv_puts(Buf *b, const char *s) { sv_put(b, s, strlen(s)); }

static void sv_esc(Buf *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char ch = s[i];
		if (ch == '&')
			sv_puts(b, "&amp;");
		else if (ch == '<')
			sv_puts(b, "&lt;");
		else if (ch == '"')
			sv_puts(b, "&quot;");
		else
			sv_put(b, &ch, 1);
	}
}

// el elemento al que apunta un <use> (href o xlink:href con #id); hay que soltarlo con dom_node_unref
static dom_node *use_target(dom_node *u)
{
	char h[160];
	if (!attr(u, "href", h, sizeof h) && !attr(u, "xlink:href", h, sizeof h))
		return NULL;
	const char *id = strchr(h, '#');
	if (!id || !id[1] || !B.doc)
		return NULL;
	dom_string *ds = NULL;
	dom_element *e = NULL;
	if (dom_string_create((const uint8_t *)id + 1, strlen(id + 1), &ds) != DOM_NO_ERR)
		return NULL;
	dom_document_get_element_by_id(B.doc, ds, &e);
	dom_string_unref(ds);
	return (dom_node *)e;
}

/* el árbol del <svg> otra vez como texto para NanoSVG: con los nombres en su forma (el lector de HTML
 * los pasa a minúsculas: viewBox, linearGradient…) y currentColor con el color del texto */
static void sv_write(Buf *b, dom_node *n, const char *color, int depth)
{
	if (depth > 40 || b->n >= MAX_PAGE)
		return;
	for (dom_node_internal *c = NI(n)->first_child; c && b->n < MAX_PAGE; c = c->next) {
		if (c->type == DOM_TEXT_NODE) {
			dom_string *d = NULL;
			if (dom_characterdata_get_data((dom_characterdata *)c, &d) == DOM_NO_ERR && d) {
				sv_esc(b, dom_string_data(d), dom_string_byte_length(d));
				dom_string_unref(d);
			}
			continue;
		}
		if (c->type != DOM_ELEMENT_NODE)
			continue;
		char tag[24];
		tag_of((dom_node *)c, tag, sizeof tag);
		if (!strcmp(tag, "title") || !strcmp(tag, "desc") || !strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "foreignobject"))
			continue;
		if (!strcmp(tag, "use")) { // <use href="#icono">: lo de dentro del <symbol> al que apunta
			dom_node *ref = use_target((dom_node *)c);
			if (ref) {
				sv_puts(b, "<g>");
				sv_write(b, ref, color, depth + 1);
				sv_puts(b, "</g>");
				dom_node_unref(ref);
			}
			continue;
		}
		static const char *const EL[][2] = {{"lineargradient", "linearGradient"}, {"radialgradient", "radialGradient"}, {"clippath", "clipPath"}};
		const char *tn = tag;
		for (size_t i = 0; i < sizeof EL / sizeof EL[0]; i++)
			if (!strcmp(tag, EL[i][0]))
				tn = EL[i][1];
		sv_puts(b, "<");
		sv_puts(b, tn);
		dom_namednodemap *m = NULL;
		if (dom_node_get_attributes((dom_node *)c, &m) == DOM_NO_ERR && m) {
			uint32_t na = 0;
			dom_namednodemap_get_length(m, &na);
			for (uint32_t i = 0; i < na; i++) {
				dom_node *a = NULL;
				if (dom_namednodemap_item(m, i, &a) != DOM_NO_ERR || !a)
					continue;
				dom_string *k = NULL, *v = NULL;
				dom_attr_get_name((dom_attr *)a, &k);
				dom_attr_get_value((dom_attr *)a, &v);
				if (k && v) {
					char kn[40];
					snprintf(kn, sizeof kn, "%.*s", (int)dom_string_byte_length(k), dom_string_data(k));
					static const char *const AT[][2] = {{"viewbox", "viewBox"}, {"preserveaspectratio", "preserveAspectRatio"},
					                                    {"gradientunits", "gradientUnits"}, {"gradienttransform", "gradientTransform"},
					                                    {"spreadmethod", "spreadMethod"}};
					const char *an = kn;
					for (size_t j = 0; j < sizeof AT / sizeof AT[0]; j++)
						if (!strcmp(kn, AT[j][0]))
							an = AT[j][1];
					sv_puts(b, " ");
					sv_puts(b, an);
					sv_puts(b, "=\"");
					const char *vs = dom_string_data(v);
					size_t vl = dom_string_byte_length(v);
					if (vl == 12 && !strncasecmp(vs, "currentcolor", 12))
						sv_puts(b, color);
					else
						sv_esc(b, vs, vl);
					sv_puts(b, "\"");
				}
				if (k)
					dom_string_unref(k);
				if (v)
					dom_string_unref(v);
				dom_node_unref(a);
			}
			dom_namednodemap_unref(m);
		}
		sv_puts(b, ">");
		sv_write(b, (dom_node *)c, color, depth + 1);
		sv_puts(b, "</");
		sv_puts(b, tn);
		sv_puts(b, ">");
	}
}

// un número de un atributo de tamaño («24», «24px», «1.5em»); -1 si no hay o es en %
static float sv_len(dom_node *n, const char *name, float em)
{
	char v[32];
	if (!attr(n, name, v, sizeof v) || strchr(v, '%'))
		return -1;
	char *u;
	float x = strtof(v, &u);
	if (u == v)
		return -1;
	return strstr(u, "em") ? x * em : x;
}

// un <svg> de la página como imagen: su número en B.img y su tamaño (-1 si no vale)
static int svg_make(dom_node *node, const css_computed_style *cs, float *ow, float *oh)
{
	css_fixed v;
	css_unit u;
	css_computed_font_size(cs, &v, &u);
	float em = brcss_px(cs, v, u, B.vw);
	if (em <= 0)
		em = 16;
	float w = -1, h = -1, vbw = 0, vbh = 0;
	if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET && u != CSS_UNIT_PCT)
		w = brcss_px(cs, v, u, B.vw);
	if (css_computed_height(cs, &v, &u) == CSS_HEIGHT_SET && u != CSS_UNIT_PCT)
		h = brcss_px(cs, v, u, B.vw);
	if (w <= 0)
		w = sv_len(node, "width", em);
	if (h <= 0)
		h = sv_len(node, "height", em);
	char vb[96];
	bool hvb = attr(node, "viewbox", vb, sizeof vb);
	if (!hvb) // sin viewBox: el del <symbol> de su <use> (iconos de un sprite)
		for (dom_node_internal *c = NI(node)->first_child; c && !hvb; c = c->next) {
			if (c->type != DOM_ELEMENT_NODE)
				continue;
			char t[8];
			tag_of((dom_node *)c, t, sizeof t);
			if (!strcmp(t, "use")) {
				dom_node *ref = use_target((dom_node *)c);
				if (ref) {
					hvb = attr(ref, "viewbox", vb, sizeof vb);
					dom_node_unref(ref);
				}
			}
		}
	if (hvb) {
		float a[4] = {0};
		sscanf(vb, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &a[0], &a[1], &a[2], &a[3]);
		vbw = a[2], vbh = a[3];
	}
	float ar = vbw > 0 && vbh > 0 ? vbw / vbh : 1;
	if (w <= 0 && h <= 0)
		h = em, w = em * ar; // sin tamaño: como la letra (lo normal en iconos)
	else if (w <= 0)
		w = h * ar;
	else if (h <= 0)
		h = w / ar;
	if (w < 3 || h < 3 || w > 1600 || h > 1600)
		return -1;
	// el color del texto (para currentColor y lo que no diga su color), en modo oscuro aclarado
	css_color tc = 0xFF000000;
	css_computed_color(cs, &tc);
	u32 col = css_col(tc) | 0xFF000000;
	if (B.dark)
		col = dk_map(col, true);
	char hex[16];
	snprintf(hex, sizeof hex, "#%02x%02x%02x", (unsigned)(col & 0xFF), (unsigned)((col >> 8) & 0xFF), (unsigned)((col >> 16) & 0xFF));
	Buf b = {0};
	sv_puts(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\"");
	if (vbw > 0 && vbh > 0) {
		char t[160];
		snprintf(t, sizeof t, " viewBox=\"%s\"", vb);
		sv_puts(&b, t);
	}
	char fill[16] = "";
	if (!attr(node, "fill", fill, sizeof fill) || !strcasecmp(fill, "currentcolor")) {
		sv_puts(&b, " fill=\"");
		sv_puts(&b, hex);
		sv_puts(&b, "\"");
	} else {
		sv_puts(&b, " fill=\"");
		sv_puts(&b, fill);
		sv_puts(&b, "\"");
	}
	char wh[64];
	snprintf(wh, sizeof wh, " width=\"%g\" height=\"%g\">", w, h);
	sv_puts(&b, wh);
	sv_write(&b, node, hex, 0);
	sv_puts(&b, "</svg>");
	if (b.n >= MAX_PAGE || b.n < 40) {
		free(b.p);
		return -1;
	}
	// clave: el mismo icono repetido se pinta una vez
	uint32_t hsh = 2166136261u;
	for (size_t i = 0; i < b.n; i++)
		hsh = (hsh ^ (unsigned char)b.p[i]) * 16777619u;
	char key[48];
	snprintf(key, sizeof key, "svg:%08lx:%zu", (unsigned long)hsh, b.n);
	int k = img_find_or_add(key);
	if (k < 0) {
		free(b.p);
		return -1;
	}
	PImg *im = &B.img[k];
	if (im->st == IM_WAIT && !im->data && !im->rgba && !im->tex.ok)
		im->data = b.p, im->dn = b.n;
	else
		free(b.p);
	im->w = w, im->h = h, im->sized = true;
	*ow = w, *oh = h;
	return k;
}

static void svg_inline(dom_node *node, St *st, const css_computed_style *cs)
{
	float w, h;
	int k = svg_make(node, cs, &w, &h);
	if (k >= 0)
		put_box(w, h, st, k);
}

static void walk(dom_node *node, St st)
{
	if (B.lay_oom || B.abort) // sin memoria, o la página se deja
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
		if (!strcmp(tag, "svg")) { // un dibujo de la página (logo, icono): como una imagen
			if (B.light || !strcmp(B.reader_url, B.url))
				return;
			css_computed_style *cs = brcss_style(B.css, node, st.cs, B.vw);
			if (cs) {
				if (css_computed_display(cs, false) != CSS_DISPLAY_NONE && css_computed_position(cs) != CSS_POSITION_FIXED && !css_invisible(cs) &&
				    css_computed_visibility(cs) != CSS_VISIBILITY_HIDDEN)
					svg_inline(node, &st, cs);
				css_computed_style_destroy(cs);
			}
			return;
		}
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
	free(B.bgi), B.bgi = NULL, B.nbgi = B.cbgi = 0;
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

/* ------------------------------------------------------------ con el motor de cajas (brlayout) */
static float px_sc(float px) { return clampf(SC16 * px / 16, 0.15f, 2.4f); }
static float bl_tw(void *u, const char *t, int n, float px, bool bold)
{
	(void)u, (void)bold;
	return text_width_n(t, n, px_sc(px));
}
static float bl_lh(void *u, float px)
{
	(void)u;
	return text_lineh(px_sc(px));
}
static int bl_link(void *u, dom_node *a, const char *href)
{
	(void)u, (void)a;
	if (DOC_BASE && !strstr(href, "://") && href[0] != '#') { // (en un iframe: respecto a su página)
		char abs[1024];
		resolve(DOC_BASE, href, abs, sizeof abs);
		return link_add(abs);
	}
	return link_add(href);
}
static int bl_clk(void *u, dom_node *n)
{
	(void)u, (void)n;
	return B.nclk++;
}
static int bl_img(void *u, dom_node *n, const char *src, float *iw, float *ih)
{
	(void)u;
	char abs[1024];
	int k;
	if (!strcmp(src, "canvas:")) { // un <canvas>: sus píxeles los pone el hilo principal (cv_upload), no se bajan
		int id = cv_of(n);
		if (!id)
			return -1;
		snprintf(abs, sizeof abs, "canvas:%d", id);
		k = img_find_or_add(abs);
		if (k >= 0 && B.img[k].st == IM_WAIT)
			B.img[k].st = IM_TEX; // (sin textura aún: no se pinta nada hasta que llegue)
		if (k >= 0)
			B.img[k].sized = true;
		return k;
	}
	if (!strncmp(src, "data:", 5)) { // la imagen va dentro de la página (base64): entera, del atributo
		dom_string *kk = NULL, *v = NULL;
		k = -1;
		if (dom_string_create((const uint8_t *)"src", 3, &kk) == DOM_NO_ERR) {
			if (dom_element_get_attribute((dom_element *)n, kk, &v) == DOM_NO_ERR && v) {
				size_t l = dom_string_byte_length(v);
				char *d = l < 512 * 1024 ? malloc(l + 1) : NULL;
				if (d) {
					memcpy(d, dom_string_data(v), l);
					d[l] = 0;
					k = img_find_or_add(d);
					free(d);
				}
				dom_string_unref(v);
			}
			dom_string_unref(kk);
		}
	} else {
		resolve(BASE_URL, src, abs, sizeof abs);
		k = img_find_or_add(abs);
	}
	if (k >= 0 && B.img[k].ow > 0 && B.img[k].oh > 0) // (su tamaño de verdad, no el reducido al decodificarla)
		*iw = (float)B.img[k].ow, *ih = (float)B.img[k].oh;
	else if (k >= 0 && B.img[k].iw > 0)
		*iw = B.img[k].iw, *ih = B.img[k].ih;
	else if (k >= 0) { // aún no se sabe: se coloca a 4:3 salvo que la página dé ancho y alto
		char w[16], h[16];
		B.img[k].guessed = !(attr(n, "width", w, sizeof w) && attr(n, "height", h, sizeof h));
	}
	return k;
}
// un icono de una fuente de la página: una imagen más («glyph:color:carácter:fuente»), la pinta su hilo
static int bl_glyph(void *u, const char *font_url, uint32_t cp, uint32_t col)
{
	(void)u, (void)col; // (el color se le da al pintarlo: el del texto, también en modo oscuro)
	char k[700];
	if (snprintf(k, sizeof k, "glyph:ffffff:%x:%s", (unsigned)cp, font_url) >= (int)sizeof k)
		return -1;
	int i = img_find_or_add(k);
	if (i >= 0)
		B.img[i].w = B.img[i].h = 24, B.img[i].sized = true, B.img[i].glyph = true;
	return i;
}

static int bl_svg(void *u, dom_node *n, const css_computed_style *cs, float *w, float *h)
{
	(void)u;
	if (!strcmp(B.reader_url, B.url))
		return -1;
	return svg_make(n, cs, w, h);
}
static int bl_wdg(void *u, dom_node *n, const char *tag, const css_computed_style *cs, float px, float *w, float *h)
{
	(void)u;
	return wdg_make(n, tag, px_sc(px), cs, B.vw, w, h);
}
// una línea por cada alto y posición distintos (la de antes se reutiliza si es igual)
static int bl_line(float y, float h)
{
	if (B.nl && B.l[B.nl - 1].y == y && B.l[B.nl - 1].h == h)
		return B.nl - 1;
	if (B.nl == B.cl) {
		int nc = B.cl ? B.cl * 2 : 256;
		Line *nl = realloc(B.l, nc * sizeof *nl);
		if (!nl)
			return -1;
		B.l = nl, B.cl = nc;
	}
	Line *l = &B.l[B.nl];
	memset(l, 0, sizeof *l);
	l->y = y, l->h = h, l->used = true;
	return B.nl++;
}
static Frag *bl_frag(float x, float y, float w, float h, const char *t, int n, const BrlText *st)
{
	int li = bl_line(y, h);
	if (li < 0)
		return NULL;
	if (B.nf == B.cf) {
		int nc = B.cf ? B.cf * 2 : 1024;
		Frag *nf = realloc(B.f, nc * sizeof *nf);
		if (!nf)
			return NULL;
		B.f = nf, B.cf = nc;
	}
	if (B.nt + n + 1 > B.ct) {
		u32 nc = B.ct ? B.ct * 2 : 64 * 1024;
		while (nc < B.nt + n + 1)
			nc *= 2;
		char *nt = realloc(B.text, nc);
		if (!nt)
			return NULL;
		B.text = nt, B.ct = nc;
	}
	memcpy(B.text + B.nt, t, n);
	Frag *f = &B.f[B.nf++];
	memset(f, 0, sizeof *f);
	f->line = li, f->x = x, f->w = w;
	f->off = B.nt, f->n = (u16)n;
	f->sc = px_sc(st->px), f->col = st->col, f->link = st->link, f->under = st->under, f->bold = st->bold;
	f->img = -1, f->wdg = -1, f->bgi = -1;
	f->node = st->node, f->clk = st->clk;
	B.nt += n;
	return f;
}
/* dónde quedó cada elemento en la última maquetación (para el JavaScript: getBoundingClientRect, offsetWidth…).
 * Tabla por dirección del nodo; los elementos en línea, unión de los trozos de su texto (y la de los suyos) */
typedef struct {
	dom_node *n;
	float x0, y0, x1, y1;
	bool block;
} Geo;
static Geo *GEO;
static int GEO_CAP;
static Geo *geo_slot(dom_node *n, bool make)
{
	if (!GEO_CAP)
		return NULL;
	uintptr_t h = ((uintptr_t)n >> 3) * 2654435761u;
	for (int k = 0; k < GEO_CAP; k++) {
		Geo *g = &GEO[(h + (uintptr_t)k) & (uintptr_t)(GEO_CAP - 1)];
		if (g->n == n)
			return g;
		if (!g->n) {
			if (!make)
				return NULL;
			g->n = n, g->x0 = g->y0 = 1e30f, g->x1 = g->y1 = -1e30f, g->block = false;
			return g;
		}
	}
	return NULL;
}
static void geo_reset(int nodes)
{
	int cap = 1024;
	while (cap < nodes * 2 && cap < (1 << 17))
		cap <<= 1;
	if (cap != GEO_CAP) {
		free(GEO);
		GEO = calloc((size_t)cap, sizeof *GEO);
		GEO_CAP = GEO ? cap : 0;
	} else if (GEO)
		memset(GEO, 0, sizeof *GEO * (size_t)cap);
}
static void geo_free(void)
{
	free(GEO);
	GEO = NULL, GEO_CAP = 0;
}
static void bl_geom(void *u, dom_node *n, float x, float y, float w, float h, bool block)
{
	(void)u;
	if (block) {
		Geo *g = geo_slot(n, true);
		if (g)
			g->x0 = x, g->y0 = y, g->x1 = x + w, g->y1 = y + h, g->block = true;
		return;
	}
	// un trozo de texto: crece su elemento y los en línea de encima, hasta el primer bloque
	for (dom_node_internal *p = (dom_node_internal *)n; p && p->type != DOM_DOCUMENT_NODE; p = p->parent) {
		Geo *g = geo_slot((dom_node *)p, true);
		if (!g || g->block)
			break;
		g->x0 = fminf(g->x0, x), g->y0 = fminf(g->y0, y), g->x1 = fmaxf(g->x1, x + w), g->y1 = fmaxf(g->y1, y + h);
	}
}
// para el JavaScript: el rectángulo de un elemento (false: no se colocó, como display: none); NULL: la página entera
static bool js_rect(dom_node *n, float r[4])
{
	if (!n) {
		r[0] = r[1] = 0, r[2] = PAGE_W, r[3] = B.total > 0 ? B.total : 600;
		return true;
	}
	Geo *g = geo_slot(n, false);
	if (!g || g->x1 < g->x0)
		return false;
	r[0] = g->x0, r[1] = g->y0, r[2] = g->x1 - g->x0, r[3] = g->y1 - g->y0;
	return true;
}

/* colocando un iframe: lo suyo va en su hueco de la página (desplazado y recortado) */
static struct {
	bool on;
	float ox, oy, x0, y0, x1, y1;
} FT;
static bool ft_cut(float *x, float *y, float w, float h)
{
	if (!FT.on)
		return false;
	*x += FT.ox, *y += FT.oy;
	return *y >= FT.y1 || *y + h <= FT.y0 || *x >= FT.x1 || *x + w <= FT.x0;
}
static void bl_text(void *u, float x, float y, float w, float h, const char *t, int n, const BrlText *st)
{
	(void)u, (void)h;
	float lh = text_lineh(px_sc(st->px));
	if (ft_cut(&x, &y, w, lh))
		return;
	bl_frag(x, y, w, lh, t, n, st);
}
static void bl_box(void *u, float x, float y, float w, float h, int img, int wdg, const BrlText *st)
{
	(void)u;
	if (!FT.on && st->node) // (el hueco de un iframe por dentro: ahí va su página)
		for (int i = 0; i < NFR; i++)
			if (FR[i].el == st->node)
				FR[i].cx = x, FR[i].cy = y, FR[i].cw = w, FR[i].ch = h;
	if (ft_cut(&x, &y, w, h))
		return;
	Frag *f = bl_frag(x, y, w, h, "", 0, st);
	if (!f)
		return;
	f->under = 0;
	f->fit = st->fit;
	if (img >= 0 && img < B.nimg) {
		f->img = (s16)img;
		if (w > B.img[img].w || !B.img[img].sized) // (la misma en dos tamaños: se baja para el mayor)
			B.img[img].w = w, B.img[img].h = h;
		B.img[img].sized = true;
	}
	if (wdg >= 0 && wdg < B.nwg)
		f->wdg = (s16)wdg;
}
static void bl_bgimg(void *u, const BrlBg *g, const BrlText *st)
{
	(void)u;
	char abs[1024];
	if (strncmp(g->url, "data:", 5))
		resolve(BASE_URL, g->url, abs, sizeof abs);
	else
		util_strlcpy(abs, g->url, sizeof abs);
	if (!strncmp(abs, "data:image/svg", 14) && !strstr(abs, "base64")) // (un svg escrito en la url: aún no)
		return;
	BrlBg gg = *g;
	if (FT.on) {
		gg.x0 += FT.ox, gg.x1 += FT.ox, gg.y0 += FT.oy, gg.y1 += FT.oy;
		gg.x0 = fmaxf(gg.x0, FT.x0), gg.y0 = fmaxf(gg.y0, FT.y0), gg.x1 = fminf(gg.x1, FT.x1), gg.y1 = fminf(gg.y1, FT.y1);
		if (gg.x1 <= gg.x0 || gg.y1 <= gg.y0)
			return;
		g = &gg;
	}
	int k = img_find_or_add(abs);
	if (k < 0)
		return;
	if (B.nbgi == B.cbgi) {
		int nc = B.cbgi ? B.cbgi * 2 : 32;
		BgImg *nb = realloc(B.bgi, nc * sizeof *nb);
		if (!nb)
			return;
		B.bgi = nb, B.cbgi = nc;
	}
	Frag *f = bl_frag(g->x0, g->y0, g->x1 - g->x0, g->y1 - g->y0, "", 0, st);
	if (!f)
		return;
	f->under = 0;
	f->img = (s16)k;
	f->bgi = (s16)B.nbgi;
	B.bgi[B.nbgi++] = (BgImg){g->size, g->rep, g->pxp, g->pyp, g->sw, g->px, g->py};
	PImg *im = &B.img[k];
	if (g->size == BRL_BG_AUTO || g->size == BRL_BG_WIDTH)
		im->natural = true;
	else if (g->x1 - g->x0 > im->w)
		im->w = g->x1 - g->x0, im->h = g->y1 - g->y0;
}

static void bl_bg(void *u, float x0, float y0, float x1, float y1, uint32_t col, const float bw[4], const uint32_t bc[4], float rad)
{
	(void)u;
	if (FT.on) {
		x0 = fmaxf(x0 + FT.ox, FT.x0), x1 = fminf(x1 + FT.ox, FT.x1), y0 = fmaxf(y0 + FT.oy, FT.y0), y1 = fminf(y1 + FT.oy, FT.y1);
		if (x1 <= x0 || y1 <= y0)
			return;
	}
	if (B.nbg == B.cbg) {
		int nc = B.cbg ? B.cbg * 2 : 64;
		BgBox *nb = realloc(B.bg, nc * sizeof *nb);
		if (!nb)
			return;
		B.bg = nb, B.cbg = nc;
	}
	BgBox *b = &B.bg[B.nbg++];
	*b = (BgBox){y0, y1, x0, x1, col, {0}, {0}, false, rad}; /* (con su transparencia) */
	for (int i = B.nf - 1; i >= 0 && i >= B.nf - 400 && !b->over; i--) { // ¿tapa una imagen ya puesta?
		const Frag *f = &B.f[i];
		if (f->img < 0 || B.img[f->img].glyph)
			continue;
		const Line *l = &B.l[f->line];
		b->over = f->x < x1 && f->x + f->w > x0 && l->y < y1 && l->y + l->h > y0;
	}
	for (int k = 0; k < 4; k++)
		b->bw[k] = bw[k], b->bc[k] = bc[k] | 0xFF000000;
}
static bool bl_stop(void *u)
{
	(void)u;
	if (B.abort)
		return true;
	if (mem_free() < MEM_RESERVE) {
		B.lay_oom = true;
		return true;
	}
	B.nel_done += 128;
	return false;
}

// el <title> de la página
static void title_get(void)
{
	if (B.title[0])
		return;
	dom_node *t = find_tag((dom_node *)B.doc, "title", 0);
	if (!t)
		return;
	dom_string *tx = NULL;
	if (dom_node_get_text_content(t, &tx) == DOM_NO_ERR && tx) {
		size_t l = dom_string_byte_length(tx);
		snprintf(B.title, sizeof B.title, "%.*s", (int)(l < sizeof B.title - 1 ? l : sizeof B.title - 1), dom_string_data(tx));
		dom_string_unref(tx);
	}
	dom_node_unref(t);
}

static float layout_boxes(void)
{
	title_get();
	BrLay L = {.css = B.css, .vw = B.vw, .vh = 600, .light = B.light || !strcmp(B.reader_url, B.url), .u = NULL,
	           .text_w = bl_tw, .line_h = bl_lh, .link = bl_link, .clk = bl_clk, .img = bl_img, .svg = bl_svg, .wdg = bl_wdg,
	           .bg = bl_bg, .text = bl_text, .box = bl_box, .bgimg = bl_bgimg, .geom = bl_geom, .glyph = bl_glyph, .stop = bl_stop};
	L.frames = !L.light && B.js && !B.nojs;
	L.canvas = L.frames;
	uint32_t pbg = 0;
	geo_reset(B.nel_total);
	float h = brl_run(&L, B.doc, &pbg);
	B.page_bg = pbg;
	// los iframes cargados: su página, colocada a su ancho, en su hueco (recortada a él)
	for (int i = 0; i < NFR && !B.abort; i++) {
		Frame *f = &FR[i];
		float r[4] = {f->cx, f->cy, f->cw, f->ch};
		if (!f->doc || !f->css || r[2] < 1 || r[3] < 1)
			continue;
		BrLay L2 = L;
		L2.css = f->css, L2.vw = r[2], L2.vh = r[3], L2.frames = false, L2.canvas = !L2.light && B.js && !B.nojs; // (sin iframes dentro de iframes)
		FT.on = true, FT.ox = r[0], FT.oy = r[1], FT.x0 = r[0], FT.y0 = r[1], FT.x1 = r[0] + r[2], FT.y1 = r[1] + r[3];
		DOC_BASE = f->url, DOC_W = r[2];
		uint32_t fbg = 0;
		size_t nbg0 = (size_t)B.nbg;
		brl_run(&L2, f->doc, &fbg);
		if (fbg >> 24) { // su fondo, debajo de lo suyo
			float bw[4] = {0}; uint32_t bc[4] = {0};
			FT.on = false;
			bl_bg(NULL, r[0], r[1], r[0] + r[2], r[1] + r[3], fbg, bw, bc, 0);
			if ((size_t)B.nbg > nbg0 + 1) { // (el último: al principio de los suyos)
				BgBox t = B.bg[B.nbg - 1];
				memmove(&B.bg[nbg0 + 1], &B.bg[nbg0], sizeof t * ((size_t)B.nbg - 1 - nbg0));
				B.bg[nbg0] = t;
			}
		}
		FT.on = false, DOC_BASE = NULL, DOC_W = 0;
	}
	return h;
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
	float boxes_h = -1;
	if (B.css) { // con CSS: el motor de cajas (todo en px de página, sin margen propio)
		PADP = 0;
		PW = B.vw;
		boxes_h = layout_boxes();
	}
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
	} else if (boxes_h < 0)
		walk((dom_node *)B.doc, st);
	if (L_has && boxes_h < 0)
		line_new();
	B.t_css = (u32)(util_ms() - t0);
	B.page_ver++;
	// modo oscuro: se oscurece al pintar lo que sea claro (lo que ya es oscuro no cambia, así que
	// también vale en las webs oscuras o con tema oscuro propio: quedan islas blancas si no)
	B.dark_tx = B.dark;
	B.lt_tx = !B.dark && B.css_dark;
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
	B.total = (boxes_h >= 0 ? fmaxf(y, boxes_h) : y) + 20;
	// orden para pintar y tocar: por altura (con columnas, el orden del documento ya no lo es)
	free(B.ord);
	B.ord = malloc(sizeof *B.ord * (B.nf ? B.nf : 1));
	if (B.ord) {
		for (int i = 0; i < B.nf; i++)
			B.ord[i] = i;
		qsort(B.ord, B.nf, sizeof *B.ord, ord_cmp);
	}
	// dónde está cada imagen (las de cerca se bajan antes): si sale varias veces, la de más arriba
	for (int i = 0; i < B.nimg; i++)
		B.img[i].y = 1e9f;
	for (int i = 0; i < B.nf; i++)
		if (B.f[i].img >= 0 && B.f[i].img < B.nimg)
			B.img[B.f[i].img].y = fminf(B.img[B.f[i].img].y, B.l[B.f[i].line].y);
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
/* las webs con el JavaScript apagado (botón «JS»): una por línea */
#define NOJS_PATH APP_DIR "/navegador_sin_js.txt"
#define NOJS_MAX 64
static char NOJS[NOJS_MAX][96];
static int NNOJS;
static bool NOJS_READ;
static const char *host_of(const char *u, char *out, size_t n);
static void nojs_load(void)
{
	if (NOJS_READ)
		return;
	NOJS_READ = true;
	FILE *f = fopen(NOJS_PATH, "r");
	char ln[128];
	while (f && NNOJS < NOJS_MAX && fgets(ln, sizeof ln, f)) {
		ln[strcspn(ln, "\r\n")] = 0;
		if (ln[0])
			util_strlcpy(NOJS[NNOJS++], ln, sizeof NOJS[0]);
	}
	if (f)
		fclose(f);
}
static int nojs_find(const char *url)
{
	char h[96];
	nojs_load();
	host_of(url, h, sizeof h);
	for (int i = 0; i < NNOJS; i++)
		if (!strcasecmp(NOJS[i], h))
			return i;
	return -1;
}
// apaga o enciende el JavaScript de la web de esa dirección (y lo guarda); devuelve si queda apagado
static bool nojs_toggle(const char *url)
{
	int i = nojs_find(url);
	if (i >= 0)
		memmove(NOJS[i], NOJS[i + 1], sizeof NOJS[0] * (size_t)(NNOJS - i - 1)), NNOJS--;
	else {
		if (NNOJS == NOJS_MAX) // (llena: se va la más antigua)
			memmove(NOJS[0], NOJS[1], sizeof NOJS[0] * (NOJS_MAX - 1)), NNOJS--;
		host_of(url, NOJS[NNOJS++], sizeof NOJS[0]);
	}
	FILE *f = fopen(NOJS_PATH, "w");
	for (int k = 0; f && k < NNOJS; k++)
		fprintf(f, "%s\n", NOJS[k]);
	if (f)
		fclose(f);
	return i < 0;
}
#define CLARO_PATH APP_DIR "/navegador_claro"
static void light_load(void)
{
	FILE *f = fopen(LIGHT_PATH, "r");
	B.light = f != NULL;
	if (f)
		fclose(f);
	f = fopen(CLARO_PATH, "r"); // el modo oscuro va de serie: se apunta si se quita
	B.dark = f == NULL;
	if (f)
		fclose(f);
}

static void dark_save(void)
{
	if (!B.dark) {
		FILE *f = fopen(CLARO_PATH, "w");
		if (f)
			fputs("1\n", f), fclose(f);
	} else
		remove(CLARO_PATH);
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
	if (br_busy()) { // se cierra ya; lo suyo se suelta al acabar el hilo (br_poll)
		br_defer(PEND_CLOSE, NULL);
		return;
	}
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp); // antes que el documento: tiene sus nodos
	B.jsp = NULL;
	B.js_defer = false;
	pf_stop();
	keep_free();
	geo_free();
	frames_free();
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
	B.abort = B.cancel = true; // al salir de la app: se espera a que se corte
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		B.thr_done = true;
	}
	close_br();
}

static void home_enter(void);
/* lo pedido mientras cargaba, en cuanto la carga se suelta (cada fotograma, aunque esté cerrado) */
void br_poll(void)
{
	if (!B.pend || br_busy())
		return;
	int p = B.pend;
	B.pend = PEND_NONE;
	if (p == PEND_LOAD) {
		char u[1024];
		util_strlcpy(u, B.pend_url, sizeof u);
		start_load(u);
	} else if (p == PEND_CLOSE)
		close_br();
	else if (p == PEND_HOME)
		home_enter();
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

// tocar un control: escribir, marcar, elegir o enviar. Con JavaScript, primero su página (start_action_k): lo de
// aquí va después, si no lo canceló (after: ya pasó por la página)
static void wdg_default(int k, bool after);
static void wdg_activate(int k) { wdg_default(k, false); }
static void wdg_default(int k, bool after)
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
		if (after) { // la página ya vio lo escrito y no lo canceló: se envía
			if (w->type != WG_AREA && w->form)
				form_submit(w->form, NULL);
		} else if (kb_input(hint, v->value ? v->value : w->label, out, sizeof out, w->type == WG_AREA, w->type == WG_PASS)) {
			free(v->value);
			v->value = strdup(out);
			util_strlcpy(w->label, out, sizeof w->label);
			if (B.jsp) // con JavaScript: primero la página (sus eventos de escribir y de enviar)
				start_action_k(w->node, -1, ACT_INPUT, out);
			else if (w->type != WG_AREA && w->form) // como pulsar Intro: se envía (una búsqueda, por ejemplo)
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

/* modo oscuro en páginas sin tema oscuro: se invierte la luminosidad (en HSL, sin tocar el tono):
 * los fondos claros pasan a oscuros y el texto oscuro a claro; lo que ya va bien (fondos oscuros,
 * texto claro) se queda. Las imágenes, tal cual */
static u32 dk_map(u32 c, bool fg)
{
	float r = (c & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = ((c >> 16) & 0xFF) / 255.0f;
	u32 a = c >> 24;
	float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), l = (mx + mn) / 2, h = 0, sat = 0, d = mx - mn;
	if (d > 0.0001f) {
		sat = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
		if (mx == r)
			h = (g - b) / d + (g < b ? 6 : 0);
		else if (mx == g)
			h = (b - r) / d + 2;
		else
			h = (r - g) / d + 4;
		h /= 6;
	}
	if (fg) {
		if (l >= 0.55f)
			return c;
		l = 0.95f - l * 0.55f; // negro → casi blanco; azul de enlace (0,5) → 0,67
		sat = fminf(sat, 0.85f);
	} else {
		if (l <= 0.45f)
			return c;
		l = 0.07f + (1 - l) * 0.85f; // blanco → casi negro; gris claro → gris oscuro
	}
	float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
	float rgb[3];
	for (int i = 0; i < 3; i++) {
		float t = h + (i == 0 ? 1.0f / 3 : i == 1 ? 0 : -1.0f / 3);
		if (t < 0)
			t += 1;
		if (t > 1)
			t -= 1;
		rgb[i] = t < 1.0f / 6 ? p + (q - p) * 6 * t : t < 0.5f ? q : t < 2.0f / 3 ? p + (q - p) * (2.0f / 3 - t) * 6 : p;
	}
	return RGBA((u8)(rgb[0] * 255 + 0.5f), (u8)(rgb[1] * 255 + 0.5f), (u8)(rgb[2] * 255 + 0.5f), (u8)a);
}
/* al revés: la página se pidió con tema oscuro y se pasó a modo claro (sin volver a cargarla) */
static u32 lt_map(u32 c, bool fg)
{
	// con el color invertido, oscurecer es aclarar: fondo oscuro → claro, texto claro → oscuro (el tono,
	// invertido dos veces, se queda)
	u32 inv = (c & 0xFF000000) | (~c & 0x00FFFFFF);
	u32 r = dk_map(inv, fg);
	return (r & 0xFF000000) | (~r & 0x00FFFFFF);
}
#define DKB(c) (B.dark_tx ? dk_map((c), false) : B.lt_tx ? lt_map((c), false) : (c))
#define DKF(c) (B.dark_tx ? dk_map((c), true) : B.lt_tx ? lt_map((c), true) : (c))

// una imagen en r (rx, ry, rw × rh), pintada solo dentro de la caja c
static void img_clip(Texture *t, float rx, float ry, float rw, float rh, float cx0, float cy0, float cx1, float cy1)
{
	float x0 = fmaxf(rx, cx0), y0 = fmaxf(ry, cy0), x1 = fminf(rx + rw, cx1), y1 = fminf(ry + rh, cy1);
	if (x1 <= x0 || y1 <= y0 || rw <= 0 || rh <= 0)
		return;
	draw_texture_part(t, x0, y0, x1 - x0, y1 - y0, (x0 - rx) / rw, (y0 - ry) / rh, (x1 - rx) / rw, (y1 - ry) / rh);
}

static void bgimg_paint(PImg *im, const BgImg *g, float BX, float BY, float BW, float BH)
{
	const float z = B.zoom;
	float nw = im->ow > 0 ? im->ow : im->tw, nh = im->oh > 0 ? im->oh : im->th, dw, dh;
	if (nw <= 0 || nh <= 0)
		return;
	switch (g->size) {
	case BRL_BG_COVER: {
		float k = fmaxf(BW / nw, BH / nh);
		dw = nw * k, dh = nh * k;
		break;
	}
	case BRL_BG_CONTAIN: {
		float k = fminf(BW / nw, BH / nh);
		dw = nw * k, dh = nh * k;
		break;
	}
	case BRL_BG_FILL: dw = BW, dh = BH; break;
	case BRL_BG_WIDTH: dw = g->sw * z, dh = dw * nh / nw; break;
	default: dw = nw * z, dh = nh * z; break;
	}
	float ox = g->pxp ? (BW - dw) * g->px / 100 : g->px * z, oy = g->pyp ? (BH - dh) * g->py / 100 : g->py * z;
	int nx = 1, ny = 1;
	float sx = BX + ox, sy = BY + oy;
	if ((g->rep & 1) && dw >= 4 && dw < BW) { // repetida: en mosaico (como mucho 32 × 32)
		sx = BX + fmodf(ox, dw);
		if (sx > BX)
			sx -= dw;
		nx = (int)ceilf((BX + BW - sx) / dw);
	}
	if ((g->rep & 2) && dh >= 4 && dh < BH) {
		sy = BY + fmodf(oy, dh);
		if (sy > BY)
			sy -= dh;
		ny = (int)ceilf((BY + BH - sy) / dh);
	}
	if (nx > 32 || ny > 32)
		nx = ny = 1, sx = BX + ox, sy = BY + oy;
	for (int j = 0; j < ny; j++)
		for (int i = 0; i < nx; i++)
			img_clip(&im->tex, sx + i * dw, sy + j * dh, dw, dh, BX, BY, BX + BW, BY + BH);
}

static float g_paint_cut = 1e9f; /* page_paint: dónde paró porque la GPU no daba para más (px) */

static void bg_paint(float VH, bool over)
{
	const float z = B.zoom;
	for (int i = 0; i < B.nbg; i++) {
		const BgBox *bb = &B.bg[i];
		if (bb->over != over)
			continue;
		float Y0 = (bb->y0 - B.sy) * z, Y1 = (bb->y1 - B.sy) * z;
		if (Y1 < 0 || Y0 > VH)
			continue;
		if (gfx_cmd_busy()) { // la GPU no da para más en este fotograma
			g_paint_cut = fminf(g_paint_cut, fmaxf(0, Y0));
			break;
		}
		float X0 = (PAD_X + bb->x0 - B.sx) * z, Wb = (bb->x1 - bb->x0) * z;
		bool even = bb->bw[0] > 0 && bb->bw[1] == bb->bw[0] && bb->bw[2] == bb->bw[0] && bb->bw[3] == bb->bw[0];
		if (bb->rad > 0.5f && (bb->col || even)) { // con esquinas redondeadas (botones, tarjetas…)
			float r = bb->rad * z, t = fmaxf(bb->bw[0] * z, 1);
			if (bb->col)
				draw_rrect(X0, Y0, Wb, Y1 - Y0, r, DKB(bb->col));
			if (even)
				draw_rrect_outline(X0, Y0, Wb, Y1 - Y0, r, t, DKB(bb->bc[0]));
			continue;
		}
		if (bb->col)
			draw_rect(X0, Y0, Wb, Y1 - Y0, DKB(bb->col));
		float t;
		if ((t = bb->bw[0] * z) > 0) draw_rect(X0, Y0, Wb, fmaxf(t, 1), DKB(bb->bc[0]));
		if ((t = bb->bw[2] * z) > 0) draw_rect(X0, Y1 - fmaxf(t, 1), Wb, fmaxf(t, 1), DKB(bb->bc[2]));
		if ((t = bb->bw[3] * z) > 0) draw_rect(X0, Y0, fmaxf(t, 1), Y1 - Y0, DKB(bb->bc[3]));
		if ((t = bb->bw[1] * z) > 0) draw_rect(X0 + Wb - fmaxf(t, 1), Y0, fmaxf(t, 1), Y1 - Y0, DKB(bb->bc[1]));
	}
}

/* la página en un rectángulo de VW x VH desde (B.sx, B.sy): mode 0 todo, 1 sin resaltados (para la
 * caché), 2 solo los resaltados (el enlace del puntero, lo marcado), encima de la caché */
static void page_paint(float VW, float VH, int mode, int hov)
{
	const float z = B.zoom;
	if (mode != 2) {
	draw_rect(0, 0, VW, VH, DKB(B.page_bg ? B.page_bg : PG_BG));
	// reglas <hr>
	for (int i = 0; i < B.nl; i++) {
		const Line *l = &B.l[i];
		if (!l->hr)
			continue;
		float y = (l->y + l->h / 2 - B.sy) * z;
		if (y >= 0 && y < VH)
			draw_rect((PAD_X - B.sx) * z, y, PAGE_W * z, fmaxf(1, z), DKB(PG_RULE));
	}
	// fondos de color de los bloques (los que tapan una imagen, después de las imágenes)
	bg_paint(VH, false);
	}
	// primero las imágenes y luego lo demás: el texto que va encima de una foto (titulares) se lee
	for (int pass = 0; pass < 2; pass++) {
	if (pass == 1 && mode != 2)
		bg_paint(VH, true);
	for (int k = first_frag(B.sy); k < B.nf; k++) {
		int i = B.ord[k];
		const Frag *f = &B.f[i];
		if ((f->img >= 0 && f->img < B.nimg && !B.img[f->img].glyph) != (pass == 0)) // (los iconos de fuente, con el texto)
			continue;
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		float X = (x - B.sx) * z, Y = (y - B.sy) * z, Wd = w * z, H = h * z;
		if (Y + H > 0 && Y < VH && gfx_cmd_busy()) {
			g_paint_cut = fminf(g_paint_cut, fmaxf(0, Y));
			break;
		}
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
			u32 border = DKB(col_hex(0xA2A9B1));
			FVal *v = fv_get(w->node, false);
			if (mode == 2) {
			} else if (w->type == WG_CHECK || w->type == WG_RADIO) {
				bool on = v && v->checked >= 0 ? v->checked : has_attr(w->node, "checked");
				float r = BH / 2;
				if (w->type == WG_RADIO) {
					draw_circle(X + r, by + r, r, border);
					draw_circle(X + r, by + r, r - fmaxf(1, z), DKB(col_hex(0xFFFFFF)));
					if (on)
						draw_circle(X + r, by + r, r * 0.5f, PG_LINK);
				} else {
					draw_rect(X, by, BH, BH, on ? PG_LINK : border);
					if (!on)
						draw_rect(X + fmaxf(1, z), by + fmaxf(1, z), BH - 2 * fmaxf(1, z), BH - 2 * fmaxf(1, z), DKB(col_hex(0xFFFFFF)));
					else
						draw_icon(IC_CHECK, X + r, by + r, BH * 0.8f, 0xFFFFFFFF);
				}
			} else {
				bool btn = w->type == WG_BUTTON;
				draw_rrect(X, by, Wd, BH, fminf(4 * z, BH / 2), border);
				draw_rrect(X + fmaxf(1, z * 0.8f), by + fmaxf(1, z * 0.8f), Wd - 2 * fmaxf(1, z * 0.8f), BH - 2 * fmaxf(1, z * 0.8f), fminf(3 * z, BH / 2),
				           DKB(btn ? col_hex(0xF8F9FA) : col_hex(0xFFFFFF)));
				char shown[96];
				const char *t = w->label;
				u32 tc = DKF(PG_TEXT);
				if (w->type == WG_PASS && t[0]) {
					size_t n = strlen(t);
					memset(shown, '*', n < 95 ? n : 95);
					shown[n < 95 ? n : 95] = 0;
					t = shown;
				}
				if (!t[0] && !btn && w->type != WG_SELECT) { // vacío: lo que la página sugiere
					if (attr(w->node, "placeholder", shown, sizeof shown) || attr(w->node, "aria-label", shown, sizeof shown))
						t = shown, tc = DKF(PG_MUTED);
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
			float bw = Wd, bh = im->glyph ? Wd : im->h * z; // (un icono: cuadrado, del tamaño de su caja)
			float by = (B.l[f->line].y + B.l[f->line].h - B.sy) * z - bh;
			if (f->bgi >= 0 && f->bgi < B.nbgi) { // de fondo: a su tamaño y posición, recortada a su caja
				if (mode != 2 && im->st == IM_TEX && im->tex.ok)
					bgimg_paint(im, &B.bgi[f->bgi], X, (B.l[f->line].y - B.sy) * z, Wd, B.l[f->line].h * z);
				continue;
			}
			if (mode == 2) {
			} else if (im->st == IM_TEX && im->tex.ok) {
				if (im->glyph) // un icono de una fuente: del color de su texto
					draw_texture_tint(&im->tex, X, by, bw, bh, DKF(f->col));
				else if (f->fit == 2) // object-fit: fill: estirada
					draw_texture(&im->tex, X, by, bw, bh, 1);
				else if (f->fit == 1) { // contain: entera, centrada
					float k = fminf(bw / im->tw, bh / im->th), dw = im->tw * k, dh = im->th * k;
					draw_texture(&im->tex, X + (bw - dw) / 2, by + (bh - dh) / 2, dw, dh, 1);
				} else { // cover (y lo normal): llena su caja, lo que sobra recortado por igual
					float k = fmaxf(bw / im->tw, bh / im->th), fu = fminf(1, bw / (im->tw * k)), fv = fminf(1, bh / (im->th * k));
					draw_texture_part(&im->tex, X, by, bw, bh, (1 - fu) / 2, (1 - fv) / 2, (1 + fu) / 2, (1 + fv) / 2);
				}
			} else if (im->glyph) { // (un icono que aún no está: nada, como una letra que falta)
			} else
				draw_rect(X, by, bw, bh, DKB(im->st == IM_FAIL ? col_hex(0xF2F2F2) : col_hex(0xEAECF0)));
			if (mode != 1 && f->link >= 0 && (f->link == hov || i == B.focus))
				draw_rect(X, by, bw, bh, col_alpha(PG_LINK, 0.25f));
			continue;
		}
		if (mode == 2)
			continue;
		float sc = f->sc * z;
		if (sc < 0.17f) // tan pequeño que no se leería: una raya del color del texto (como una vista previa)
			draw_rect(X, Y + H * 0.35f, Wd, fmaxf(1, H * 0.35f), col_alpha(DKF(f->col), 0.55f));
		else {
			text_draw_n(B.text + f->off, f->n, X, Y, sc, DKF(f->col));
			if (f->bold) // la fuente no tiene negrita: otra vez, un poco a la derecha
				text_draw_n(B.text + f->off, f->n, X + fmaxf(0.5f, z * 0.6f), Y, sc, DKF(f->col));
			if (f->under)
				draw_rect(X, Y + H - fmaxf(1, z * 0.8f), Wd, fmaxf(1, z * 0.6f), col_alpha(DKF(f->col), 0.5f));
		}
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
	float cut; /* hasta dónde (px de la textura) se pudo pintar (la GPU no daba para más) */
	float dx, dy; /* dónde se dibujó la última vez (para enseñarla quieta mientras se recoloca) */
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
	bool ok = PCS.valid && PCS.zoom == z && PCS.ver == B.page_ver && dx >= 0 && dy >= 0 && dx + TOP_W <= PC_W && dy + SCR_H <= PC_H &&
	          (dy + SCR_H <= PCS.cut || PCS.cut < my + SCR_H); // (cortada: desde más abajo, si así llega)
	if (!ok) {
		float sx = B.sx, sy = B.sy;
		PCS.x0 = B.sx - floorf(mx) / z, PCS.y0 = B.sy - floorf(my) / z;
		B.sx = PCS.x0, B.sy = PCS.y0;
		gfx_rt_begin(&PC, DKB(B.page_bg ? B.page_bg : PG_BG));
		bool ell = g_clip_ell;
		g_clip_ell = true; // sin recortes (las tijeras van por pantalla, no por textura)
		g_paint_cut = 1e9f;
		page_paint(PC_W, PC_H, 1, -1);
		PCS.cut = g_paint_cut;
		g_clip_ell = ell;
		gfx_resume(g_top);
		B.sx = sx, B.sy = sy;
		PCS.valid = true, PCS.zoom = z, PCS.ver = B.page_ver;
		dx = (B.sx - PCS.x0) * z, dy = (B.sy - PCS.y0) * z;
	}
	PCS.dx = dx, PCS.dy = dy;
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
	if (B.st == ST_LAYOUT && B.shown && PC.ok && PCS.valid) { // se recoloca una página que ya se veía: la de antes,
		gfx_rt_draw(&PC, -floorf(PCS.dx + 0.5f), -floorf(PCS.dy + 0.5f)); // quieta, con un aviso pequeño
		const char *msg = "Actualizando la página…";
		float w = text_width(msg, TS_SUB) + 44, x = (TOP_W - w) / 2, y = SCR_H - 34;
		draw_rrect(x, y, w, 24, 12, col_alpha(T.surface, 0.94f));
		B.t_load += 1.0f / 60;
		draw_arc(x + 16, y + 12, 6, 2, B.t_load * 6, B.t_load * 6 + 4.2f, T.accent);
		text_draw(msg, x + 28, y + 12 - text_lineh(TS_SUB) / 2, TS_SUB, T.text2);
		if (B.nel_total > 0) {
			float f = clampf((float)B.nel_done / B.nel_total, 0, 1);
			draw_rect(x + 12, y + 21, (w - 24) * f, 2, T.accent);
		}
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
	if (B.st == ST_READY)
		B.shown = true;
	cv_upload();
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

static void layout_thread_run(void *arg);
static void layout_thread(void *arg)
{
	layout_thread_run(arg);
	B.thr_done = true; // ya se puede reutilizar (o soltar) sin esperar
}

static void layout_thread_run(void *arg)
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
	B.img_relaid = false;
	B.idle_dirty = false;
	B.ready_ms = util_ms();
	B.st = ST_READY;
	img_start();
}

// un clic con el JavaScript de la página: sus eventos y, si cambió algo, se vuelve a colocar
static dom_node *ACT_EL;
static int ACT_LINK;
static int ACT_KIND;
static float ACT_X = -1, ACT_Y = -1; /* dónde se tocó, en px de la página (-1: no se sabe) */
static char *ACT_STR;
static void act_thread_run(void *arg);
static void act_thread(void *arg)
{
	act_thread_run(arg);
	B.thr_done = true; // ya se puede reutilizar (o soltar) sin esperar
}

static void act_thread_run(void *arg)
{
	(void)arg;
	browser_thread_net();
	JsResult r;
	bool changed = false;
	bool go_on = ACT_KIND == ACT_INPUT ? jsdom_input(jsp_of_node(ACT_EL), ACT_EL, ACT_STR, true, &changed, &r)
	                                   : jsdom_click(jsp_of_node(ACT_EL), ACT_EL, ACT_X, ACT_Y, &changed, &r); // (de la página o de un iframe)
	js_oom_check(&r);
	changed |= frames_after_js();
	if (B.idle_dirty)
		changed = true, B.idle_dirty = false;
	if (r.navigate[0])
		util_strlcpy(B.js_nav, r.navigate, sizeof B.js_nav);
	if (changed) { // se coloca otra vez, sin mover la vista
		float z = B.zoom, sx = B.sx, sy = B.sy, px = B.px, py = B.py;
		dom_node *fn = B.focus >= 0 && B.focus < B.nf ? B.f[B.focus].node : NULL;
		B.st = ST_LAYOUT;
		css_refresh();
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
	B.click_link = go_on && ACT_KIND == ACT_CLICK ? ACT_LINK : -1;
	B.click_wdg = go_on && ACT_KIND != ACT_CLICK ? ACT_KIND : 0; // (lo de siempre del control, ya en el principal)
	B.st = ST_READY;
}

// lo que la página dejó para después, con ella ya a la vista: un rato corto; si cambia, se recoloca
// el JavaScript de una página que ya se ve (la carga lo dejó para después): sus scripts (casi todos ya
// bajados por adelantado) y, si cambió algo, la página otra vez (con el CSS de nuevo si cambió lo que lo decide)
static void jsdef_thread_run(void *arg);
static void jsdef_thread(void *arg)
{
	jsdef_thread_run(arg);
	pf_stop();
	if (!B.jsp)
		keep_free(), B.css_sig = 0;
	if (B.st == ST_ACT || B.st == ST_LAYOUT)
		B.st = ST_READY;
	B.thr_done = true;
}

static void jsdef_thread_run(void *arg)
{
	(void)arg;
	browser_thread_net();
	B.js_defer = false;
	if (!B.doc || B.abort)
		return;
	JsHost h = {.mem_limit = B.js_lim, .dark = B.dark, .fetch = js_fetch, .log = js_log, .url = B.url, .ua = UA_PC, .width = VW_PC, .budget_ms = 20000,
	            .cancel = &B.cancel, .cookie_get = ck_get, .cookie_set = ck_set, .rect = js_rect,
		            .height = 600, .frame = 0, .frame_of = js_frame_of, .post = js_post, .image = js_image};
	JsResult jr;
	B.jsp = jsdom_open(B.doc, &h, &jr);
	B.js_scripts = jr.scripts, B.js_errors = jr.errors;
	util_log("web: JavaScript (después): %d scripts, %d con error, %u ms%s", jr.scripts, jr.errors, jr.ms, jr.changed ? ", cambió la página" : "");
	if (jr.navigate[0])
		util_strlcpy(B.js_nav, jr.navigate, sizeof B.js_nav);
	if (jr.oom) {
		util_log("web: el JavaScript se quedó sin memoria (%u KB)", (unsigned)(jr.mem / 1024));
		jsdom_close(B.jsp);
		B.jsp = NULL;
	}
	B.idle_left = B.jsp ? 30000 : 0;
	B.idle_next = util_ms() + 600;
	bool fch = !B.abort && frames_after_js(); // (sus iframes: se cargan con la página ya colocada, que da su tamaño)
	if ((!jr.changed && !fch) || B.abort || B.js_nav[0])
		return;
	// otra vez, sin mover la vista
	float z = B.zoom, sx = B.sx, sy = B.sy, px = B.px, py = B.py;
	dom_node *fn = B.focus >= 0 && B.focus < B.nf ? B.f[B.focus].node : NULL;
	B.st = ST_LAYOUT;
	u64 t0 = osGetTime();
	css_refresh();
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
	util_log("web: colocada otra vez tras su JavaScript, %u ms", (unsigned)(osGetTime() - t0));
	img_start();
}

static void idle_thread_run(void *arg);
static void idle_thread(void *arg)
{
	idle_thread_run(arg);
	B.thr_done = true; // ya se puede reutilizar (o soltar) sin esperar
}

static void idle_thread_run(void *arg)
{
	(void)arg;
	browser_thread_net();
	JsResult r;
	bool changed = false;
	u64 t0 = util_ms();
	bool more = jsdom_idle(B.jsp, 600, &changed, &r);
	js_oom_check(&r);
	for (int i = 0; i < NFR; i++) // (los de sus iframes, un rato cada uno)
		if (FR[i].jsp) {
			bool c2 = false;
			JsResult r2;
			more |= jsdom_idle(FR[i].jsp, 300, &c2, &r2);
			changed |= c2;
		}
	changed |= frames_after_js();
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
		css_refresh();
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

/* recolocar cuando ya se sabe la proporción de las imágenes que se colocaron sin saberla (a 4:3): las fotos
 * de una portada salían con otro alto que en un navegador. Una vez por página, sin moverse de donde se está */
static void imgfit_thread(void *arg)
{
	(void)arg;
	float z = B.zoom, sx = B.sx, sy = B.sy, px = B.px, py = B.py;
	B.st = ST_LAYOUT;
	int keep = B.idle_left;
	util_log("web: recolocar con la proporción de las imágenes");
	layout();
	B.idle_left = keep;
	B.zoom = z, B.sx = sx, B.sy = sy, B.px = px, B.py = py;
	clamp_view();
	B.tip = 0;
	img_start();
	B.st = ST_READY;
	B.thr_done = true;
}

// ¿hace falta? las de cerca de lo que se ve ya están, y alguna no era 4:3
static bool imgfit_due(void)
{
	if (B.img_relaid || !B.nf)
		return false;
	float lim = B.sy + 2.5f * (float)SCR_H / (B.zoom > 0 ? B.zoom : 1);
	bool off = false;
	for (int i = 0; i < B.nimg; i++) {
		const PImg *im = &B.img[i];
		if (!im->guessed || im->y > lim)
			continue;
		if (im->st == IM_WAIT || im->st == IM_LOAD || im->st == IM_RGBA)
			return false; // (aún llegan: se espera a tenerlas todas)
		if (im->st == IM_TEX && im->ow > 0 && im->oh > 0) {
			float ar = (float)im->ow / im->oh;
			if (fabsf(ar - 4.0f / 3) > 0.08f * 4 / 3)
				off = true;
		}
	}
	return off;
}

static void start_action_k(dom_node *el, int link, int kind, const char *str);
static void start_action(dom_node *el, int link) { start_action_k(el, link, ACT_CLICK, NULL); }
static void start_action_k(dom_node *el, int link, int kind, const char *str)
{
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	ACT_EL = el, ACT_LINK = link, ACT_KIND = kind;
	ACT_X = B.sx + B.px / B.zoom - PAD_X, ACT_Y = B.sy + B.py / B.zoom; // (el puntero, en la página)
	if (ACT_X < 0 || ACT_Y < 0)
		ACT_X = ACT_Y = -1;
	free(ACT_STR);
	ACT_STR = str ? strdup(str) : NULL;
	B.click_link = -2, B.click_wdg = 0;
	B.st = ST_ACT;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thr_done = false, B.thr = threadCreate(act_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
	if (!B.thr) { // sin hilo: el clic sigue como un enlace normal (o el control, como siempre)
		B.st = ST_READY;
		B.click_link = kind == ACT_CLICK ? link : -1;
		B.click_wdg = kind == ACT_CLICK ? 0 : kind;
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
	if (br_busy()) {
		br_defer(PEND_HOME, NULL);
		return;
	}
	if (B.thr) {
		threadJoin(B.thr, U64_MAX);
		threadFree(B.thr);
		B.thr = NULL;
	}
	jsdom_close(B.jsp);
	B.jsp = NULL;
	B.js_defer = false;
	pf_stop();
	keep_free();
	geo_free();
	frames_free();
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
	br_poll();
	if (B.st == ST_PARSED) { // el árbol está listo (se guarda para cambiar PC / móvil sin volver a bajarla)
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		B.st = ST_LAYOUT;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		B.thr_done = false, B.thr = threadCreate(layout_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
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
	if (B.st == ST_READY && B.js_defer && !B.jsp) { // la página ya se ve: ahora su JavaScript
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		B.st = ST_ACT;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		// (por debajo de las imágenes: que sigan llegando mientras trabaja)
		B.thr_done = false, B.thr = threadCreate(jsdef_thread, NULL, 1024 * 1024, prio + 3, br_core(), false);
		if (!B.thr)
			B.st = ST_READY, B.js_defer = false;
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
		B.thr_done = false, B.thr = threadCreate(idle_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
		if (!B.thr)
			B.st = ST_READY, B.idle_left = 0;
	}
	if (B.st == ST_READY && !B.light && strcmp(B.reader_url, B.url) && util_ms() - B.last_in > 1200 && imgfit_due()) {
		B.img_relaid = true;
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		B.st = ST_ACT;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		B.thr_done = false, B.thr = threadCreate(imgfit_thread, NULL, 1024 * 1024, prio + 1, br_core(), false);
		if (!B.thr)
			B.st = ST_READY;
	}
	if (B.st == ST_READY && B.click_wdg) { // el control, tras su JavaScript: lo de siempre (buscado por su nodo: la
		int kind = B.click_wdg; // página pudo recolocarse)
		B.click_wdg = 0;
		for (int i = 0; i < B.nwg; i++)
			if (B.wg[i].node == ACT_EL) {
				wdg_default(i, kind == ACT_INPUT);
				break;
			}
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
	// modo ligero: una pluma (la palabra, al lado de la luna, podía leerse como «claro»)
	float lx0 = BOT_W - 42;
	bool lgp = touch_in(lx0, ty, 30, 24);
	draw_rrect(lx0, ty, 30, 24, 12, B.light ? col_lerp(T.bg, T.accent, 0.25f) : lgp ? T.surface2 : T.surface);
	draw_icon(IC_FEATHER, lx0 + 15, ty + 12, 14, B.light ? T.accent : T.text);
	bool tlight = tap(lx0, ty, 30, 24);
	// modo oscuro: una luna (llena por dentro si está puesto)
	float mx0 = BOT_W - 76;
	bool mp = touch_in(mx0, ty, 30, 24);
	u32 mbg = B.dark ? col_lerp(T.bg, T.accent, 0.25f) : mp ? T.surface2 : T.surface;
	draw_rrect(mx0, ty, 30, 24, 12, mbg);
	draw_circle(mx0 + 15, ty + 12, 6.5f, B.dark ? T.accent : T.text);
	draw_circle(mx0 + 18.5f, ty + 9.5f, 5.5f, mbg); // el mordisco: del color del botón
	bool tdark = tap(mx0, ty, 30, 24);
	// JavaScript de esta web: encendido (en color) o apagado (tachado); se recuerda por web
	float jx = BOT_W - 180;
	bool jon = !B.nojs && !B.light, jp = touch_in(jx, ty, 32, 24);
	draw_rrect(jx, ty, 32, 24, 12, jon ? col_lerp(T.bg, T.accent, B.js_scripts > 0 ? 0.25f : 0.12f) : jp ? T.surface2 : T.surface);
	float jw = text_width("JS", TS_SUB);
	text_draw("JS", jx + 16 - jw / 2, ty + 12 - text_lineh(TS_SUB) / 2, TS_SUB, jon ? T.accent : T.text3);
	if (!jon)
		draw_rect(jx + 16 - jw / 2 - 2, ty + 12, jw + 4, 1.5f, T.text3);
	if (tap(jx, ty, 32, 24) && B.url[0]) {
		char h[96];
		host_of(B.url, h, sizeof h);
		if (B.light)
			toast_icon("En modo ligero no hay JavaScript (la pluma)", IC_FEATHER, T.text2);
		else {
			bool off = nojs_toggle(B.url);
			char m[160];
			snprintf(m, sizeof m, off ? "JavaScript apagado en %s" : "JavaScript encendido en %s", h);
			toast_icon(m, IC_CODE, off ? T.text2 : T.accent);
			char u2[1024];
			util_strlcpy(u2, B.url, sizeof u2);
			start_load(u2);
		}
	}
	// favoritos: guardar esta página y abrir la lista
	bm_load();
	bool is_fav = B.url[0] && bm_find_fav(B.url) >= 0;
	float fx = BOT_W - 144;
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
	}
	// la luna y la pluma valen también mientras carga (la pluma corta la carga y la repite en su modo)
	if (tdark) { // modo oscuro: al momento, al pintar (sin volver a cargar la página)
		B.dark = !B.dark;
		dark_save();
		B.dark_tx = B.dark;
		B.lt_tx = !B.dark && B.css_dark;
		B.page_ver++;
		toast_icon(B.dark ? "Modo oscuro" : "Modo claro", IC_BULB, T.accent);
	}
	if (tlight || (g_kdown & KEY_Y)) { // modo ligero: sin imágenes ni JavaScript (se recuerda)
		B.light = !B.light;
		light_save();
		toast_icon(B.light ? "Modo ligero: sin imágenes ni JavaScript" : "Modo normal: con imágenes y JavaScript", IC_FEATHER, T.accent);
		char u2[1024];
		util_strlcpy(u2, B.url, sizeof u2);
		start_load(u2);
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
		int wk = wdg_at_pointer();
		const Wdg *w = &B.wg[wk];
		if (B.jsp && w->type != WG_TEXT && w->type != WG_PASS && w->type != WG_AREA)
			start_action_k(w->node, -1, ACT_WDG, NULL); // (su clic, a la página; luego, lo suyo)
		else
			wdg_activate(wk);
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
