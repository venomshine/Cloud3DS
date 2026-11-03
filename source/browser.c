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
#include "core/node.h" /* libdom por dentro: hijos y hermanos sin referencias */
#include "dl.h"
#include "gfx.h"
#include "icons.h"
#include "img.h"
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

#define MAX_PAGE  (6 * 1024 * 1024)
#define HIST      16
#define HDR_H     30.0f
#define SC_TEXT   0.46f
/* cómo se presenta: en Móvil, como el navegador de la 3DS (las webs dan su versión sencilla); en PC,
 * como uno de escritorio */
#define UA_MOB "Mozilla/5.0 (Nintendo 3DS; U; ; es) Version/1.7630.EU Cloud3DS"
#define UA_PC  "Mozilla/5.0 (X11; Linux x86_64; rv:115.0) Gecko/20100101 Firefox/115.0 Cloud3DS"
#define VW_PC     980.0f   /* ancho de la página «de PC» (como un portátil) */
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

typedef struct {   /* fondo de color de un bloque (background-color) */
	float y0, y1, x0, x1;
	u32 col;
} BgBox;

enum { ST_IDLE, ST_LOADING, ST_PARSED, ST_LAYOUT, ST_READY, ST_ERROR };

static struct {
	bool open;
	volatile int st;
	char url[1024], err[128], title[160];
	char hist[HIST][1024];
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
	PImg *img;
	int nimg, cimg;
	Thread img_thr;
	volatile bool img_quit;
	size_t img_px;       /* píxeles decodificados (límite de memoria) */
} B;

static void img_free_all(void);
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
	return dl_fetch(url, accept, buf_cb, NULL, b, code);
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
#define CSS_MAX_LINKS 10
#define CSS_MAX_BYTES (1536 * 1024)
// un <style> o un <link rel="stylesheet">; aparte, para que la recursión de css_collect no lleve
// estos búferes en cada nivel (con páginas profundas desbordaban la pila del hilo)
static __attribute__((noinline)) void css_one(BrCss *c, dom_node *el, bool is_style, int *links, size_t *bytes)
{
	char a[256];
	if (is_style) {
		if (attr(el, "media", a, sizeof a) && strstr(a, "print") && !strstr(a, "screen"))
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
	if (attr(el, "media", a, sizeof a) && strstr(a, "print") && !strstr(a, "screen"))
		return;
	char *href = malloc(2048), *abs = href ? href + 1024 : NULL;
	if (!href)
		return;
	if (attr(el, "href", href, 1024) && href[0]) {
		resolve(B.url, href, abs, 1024);
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
	dl_set_thread_ua(B.force == 1 ? UA_PC : UA_MOB);
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
		int r = dl_fetch(B.url, "text/html,*/*;q=0.8", buf_cb, NULL, &b, &code);
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
	dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)(b.p ? b.p : ""), b.n);
	dom_hubbub_parser_completed(parser);
	dom_hubbub_parser_destroy(parser);
	free(b.p);
	// el CSS de la página (se baja y se lee aquí, en el hilo, para no parar la pantalla)
	BrCss *css = doc ? brcss_new(B.url) : NULL;
	if (css) {
		int links = 0;
		size_t bytes = 0;
		css_collect(css, (dom_node *)doc, 0, &links, &bytes);
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
	B.err[0] = 0;
	B.title[0] = 0;
	B.st = ST_LOADING;
	B.t_load = 0;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thr = threadCreate(load_thread, NULL, 512 * 1024, prio + 1, -2, false); // hubbub y libdom anidan mucho en páginas grandes
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
	u8 align;
	bool list;     /* <li> con viñeta (list-style no es none) */
	int *olnum;    /* contador de la <ol> de dentro */
	int *cells;    /* celdas de la fila (separadores) */
	const css_computed_style *cs; /* el estilo calculado del padre (con CSS) */
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
			L_gap = 0;
			L_x = L_indent;
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
	L_gap = 0;
	L_x = L_indent;
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
	if (!B.nl || (L_has && L_x + sp + w > RIGHT)) {
		line_new();
		sp = 0;
	}
	if (!L_has)
		L_x = L_indent;
	// una palabra más ancha que la pantalla: a trozos
	while (w > RIGHT - L_indent && n > 1) {
		int k = n;
		while (k > 1 && text_width_n(s, k, st->sc) > RIGHT - L_x)
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
	if (L_has && L_x + sp + w > RIGHT)
		line_new(), sp = 0;
	if (!L_has)
		L_x = L_indent;
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
	dl_set_thread_ua(B.force == 1 ? UA_PC : UA_MOB);
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
		int r = fetch_any(im->url, "image/png,image/jpeg,image/gif,image/*;q=0.5", &b, &code);
		int mw = (int)fminf(512, fmaxf(16, im->w * 1.5f)), mh = (int)fminf(512, fmaxf(16, im->h * 1.5f));
		u8 *px = NULL;
		int w = 0, h = 0;
		if (r == DLF_OK && code < 400 && b.n && B.img_px < IMG_BUDGET && img_mem_fit((const u8 *)b.p, b.n, mw, mh, &px, &w, &h) == IMG_OK) {
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
	bool any = false;
	for (int i = 0; i < B.nimg; i++)
		any |= B.img[i].st == IM_WAIT;
	if (!any)
		return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.img_thr = threadCreate(img_thread, NULL, 64 * 1024, prio + 2, -2, false);
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
		L_x = L_indent;
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
	B.bg[B.nbg++] = (BgBox){B.l[l0].y - 4, B.l[l1].y + B.l[l1].h + 4, x0, x1, col};
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
	L_x = L_indent;
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
			cs.indent = x, cs.right = x + c[i].w, cs.cells = NULL;
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

/* un elemento con su estilo calculado: la página decide qué se ve, colores, tamaños, márgenes */
static void walk_css(dom_node *node, const char *tag, St st, const css_computed_style *cs, uint8_t d)
{
	char a[256];
	st.cs = cs;
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
	if (!strcmp(tag, "img")) {
		char src[1024], abs[1024], v[32];
		bool ok = attr(node, "src", src, sizeof src) && src[0] && strncmp(src, "data:", 5);
		if (!ok) // las que se cargan «perezosas» guardan la dirección en data-src
			ok = attr(node, "data-src", src, sizeof src) && src[0];
		size_t sl = ok ? strlen(src) : 0;
		bool svg = sl > 4 && (!strcasecmp(src + sl - 4, ".svg") || strstr(src, ".svg?"));
		if (ok && !svg) {
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
		if (css_computed_width(cs, &v, &vu) == CSS_WIDTH_SET && d != CSS_DISPLAY_TABLE_ROW)
			w = fminf(w, len_px(cs, v, vu, avail));
		if (css_computed_max_width(cs, &v, &vu) == CSS_MAX_WIDTH_SET)
			w = fminf(w, len_px(cs, v, vu, avail));
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
		block(mt + clampf(pt, 0, 40));
		L_indent = st.indent;
		L_right = st.right;
		L_align = st.align;
		line_ctx();
		if (bgc) {
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
	if (!strcmp(tag, "a") && attr(node, "href", a, sizeof a) && a[0] && a[0] != '#' && strncasecmp(a, "javascript:", 11))
		st.link = link_add(a);
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
		if (bgc && l0 >= 0 && l1 >= l0)
			bg_add(l0, l1, bx0, bx1, bgc);
		block(mb);
		L_indent = o_ind, L_right = o_right, L_align = o_align;
		line_ctx();
	}
}

static void walk(dom_node *node, St st)
{
	B.nel_done++;
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
			if (d != CSS_DISPLAY_NONE && css_computed_position(cs) != CSS_POSITION_FIXED && !css_invisible(cs))
				walk_css(node, tag, st, cs, d);
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
	bool mob = B.force ? B.force == 2 : B.page_mobile;
	B.vw = mob ? VW_MOB : VW_PC;
	PADP = mob ? 8 : 16;
	PW = B.vw - 2 * PADP;
	L_x = L_indent = L_gap = 0;
	L_right = 1e9;
	L_align = 0;
	L_pb = 6;
	L_has = L_space = false;
	SC16 = SC_TEXT * 19.2f / text_lineh(SC_TEXT); // 16 px de letra = 19,2 px de línea
	St st = {.sc = SC_TEXT, .col = PG_TEXT, .link = -1, .right = PAGE_W};
	u64 t0 = util_ms();
	B.nel_done = 0;
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

static void go(const char *url, bool push)
{
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
		util_strlcpy(B.hist[B.nhist++], B.url, sizeof B.hist[0]);
	}
	start_load(url);
}

void br_open_url(const char *url)
{
	B.open = true;
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

static void back(void)
{
	if (B.nhist)
		start_load(B.hist[--B.nhist]);
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
#define FKEY(f) ((f).wdg >= 0 ? 100000 + (f).wdg : (f).link)
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

void br_top(void)
{
	if (B.st != ST_READY) {
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
			text_draw_center(B.st == ST_LAYOUT ? "Colocando la página…" : "Cargando…", TOP_W / 2, 158, TS_SUB, T.text3);
			if (B.st == ST_LAYOUT && B.nel_total > 0) {
				float f = clampf((float)B.nel_done / B.nel_total, 0, 1);
				draw_rrect(TOP_W / 2 - 80, 180, 160, 5, 2.5f, T.surface2);
				draw_rrect(TOP_W / 2 - 80, 180, fmaxf(5, 160 * f), 5, 2.5f, T.accent);
			}
		}
		(void)X;
		return;
	}
	draw_rect(0, 0, TOP_W, SCR_H, PG_BG);
	const float z = B.zoom;
	// reglas <hr>
	for (int i = 0; i < B.nl; i++) {
		const Line *l = &B.l[i];
		if (!l->hr)
			continue;
		float y = (l->y + l->h / 2 - B.sy) * z;
		if (y >= 0 && y < SCR_H)
			draw_rect((PAD_X - B.sx) * z, y, PAGE_W * z, fmaxf(1, z), PG_RULE);
	}
	// fondos de color de los bloques
	for (int i = 0; i < B.nbg; i++) {
		const BgBox *bb = &B.bg[i];
		float Y0 = (bb->y0 - B.sy) * z, Y1 = (bb->y1 - B.sy) * z;
		if (Y1 < 0 || Y0 > SCR_H)
			continue;
		draw_rect((PAD_X + bb->x0 - B.sx) * z, Y0, (bb->x1 - bb->x0) * z, Y1 - Y0, bb->col);
	}
	// texturas de las imágenes ya decodificadas (pocas por fotograma, para no dar tirones)
	for (int i = 0, up = 0; i < B.nimg && up < 2; i++)
		if (B.img[i].st == IM_RGBA) {
			bool ok = tex_from_rgba(&B.img[i].tex, B.img[i].rgba, B.img[i].tw, B.img[i].th);
			free(B.img[i].rgba);
			B.img[i].rgba = NULL;
			B.img[i].st = ok ? IM_TEX : IM_FAIL;
			up++;
		}
	int hov = link_at_pointer();
	B.hover = hov;
	B.hw = wdg_at_pointer();
	for (int k = first_frag(B.sy); k < B.nf; k++) {
		int i = B.ord[k];
		const Frag *f = &B.f[i];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		float X = (x - B.sx) * z, Y = (y - B.sy) * z, Wd = w * z, H = h * z;
		if ((B.l[f->line].y - B.sy) * z > SCR_H)
			break;
		if (X > TOP_W || X + Wd < 0 || Y + H < 0)
			continue;
		if (f->link >= 0 && (f->link == hov || i == B.focus))
			draw_rect(X - 1, Y, Wd + 2, H, col_alpha(PG_LINK, 0.18f));
		if (f->wdg >= 0 && f->wdg < B.nwg) { // control de formulario
			const Wdg *w = &B.wg[f->wdg];
			const Line *l = &B.l[f->line];
			float bh = (w->type == WG_CHECK || w->type == WG_RADIO) ? text_lineh(f->sc) : (w->type == WG_AREA ? l->h : text_lineh(f->sc) * 1.5f);
			float by = (l->y + l->h - bh - B.sy) * z, BH = bh * z;
			u32 border = col_hex(0xA2A9B1);
			FVal *v = fv_get(w->node, false);
			if (w->type == WG_CHECK || w->type == WG_RADIO) {
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
			if (f->wdg == B.hw)
				draw_rect(X, by, Wd, BH, col_alpha(PG_LINK, 0.18f));
			continue;
		}
		if (f->img >= 0 && f->img < B.nimg) { // imagen: encajada en su sitio sin deformarla
			PImg *im = &B.img[f->img];
			float bw = Wd, bh = im->h * z, by = (B.l[f->line].y + B.l[f->line].h - im->h - B.sy) * z;
			if (im->st == IM_TEX && im->tex.ok) {
				float k = fminf(bw / im->tw, bh / im->th), dw = im->tw * k, dh = im->th * k;
				draw_texture(&im->tex, X + (bw - dw) / 2, by + (bh - dh) / 2, dw, dh, 1);
			} else
				draw_rect(X, by, bw, bh, im->st == IM_FAIL ? col_hex(0xF2F2F2) : col_hex(0xEAECF0));
			if (f->link >= 0 && (f->link == hov || i == B.focus))
				draw_rect(X, by, bw, bh, col_alpha(PG_LINK, 0.25f));
			continue;
		}
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
	// barra de desplazamiento fina a la derecha
	if (B.total > view_h()) {
		float bh = fmaxf(16, SCR_H * view_h() / B.total), by = (SCR_H - bh) * (B.sy / (B.total - view_h()));
		draw_rrect(TOP_W - 5, by, 3, bh, 1.5f, col_alpha(0xFF000000, 0.3f));
	}
	// arriba: la web, PC / móvil y el zoom (un momento, al cambiar)
	char host[96], chip[160];
	host_of(B.url, host, sizeof host);
	snprintf(chip, sizeof chip, "%s   %s · %d %%", host, B.vw == VW_MOB ? "Móvil" : "PC", (int)(z * 100 + 0.5f));
	float cw = fminf(TOP_W - 16, text_width(chip, 0.40f) + 20);
	B.tip = fmaxf(0, B.tip - 1.0f / 60);
	float ca = clampf(B.tip / 0.4f, 0, 1); // se ve al abrir y al hacer zoom, y se va sola para no tapar la página
	if (ca > 0.01f) {
		draw_rrect(8, 6, cw, 18, 9, col_alpha(0xFF1A1F2A, 0.85f * ca));
		text_draw_clip(chip, 18, 6 + 9 - text_lineh(0.40f) / 2, cw - 20, 0.40f, col_alpha(0xFFFFFFFF, ca));
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
static void layout_thread(void *arg)
{
	(void)arg;
	layout();
	B.hover = -1;
	B.st = ST_READY;
	img_start();
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
		B.thr = threadCreate(layout_thread, NULL, 1024 * 1024, prio + 1, -2, false);
		if (!B.thr) {
			snprintf(B.err, sizeof B.err, "Sin memoria para colocar la página");
			B.st = ST_ERROR;
		}
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
	bool mob = ready && B.vw == VW_MOB;
	bool tpc = tool_btn(BOT_W - 112, ty, 52, "PC", ready && !mob), tmob = tool_btn(BOT_W - 58, ty, 50, "Móvil", mob);
	// --- touchpad (con su trama de puntos) y franja para desplazar
	const float TX = 8, TW = BOT_W - 16 - STRIP - 6, SX = BOT_W - 8 - STRIP;
	draw_rrect(TX, TP_Y, TW, TP_B - TP_Y, 12, T.surface);
	for (float yy = TP_Y + 10; yy < TP_B - 6; yy += 12)
		for (float xx = TX + 10; xx < TX + TW - 6; xx += 12)
			draw_rect(xx, yy, 1.5f, 1.5f, T.divider);
	draw_rrect(SX, TP_Y, STRIP, TP_B - TP_Y, 12, T.surface);
	if (ready && B.total > view_h()) {
		float h = TP_B - TP_Y - 8, gh = fmaxf(18, h * view_h() / B.total), gy = TP_Y + 4 + (h - gh) * (B.sy / (B.total - view_h()));
		draw_rrect(SX + 8, gy, STRIP - 16, gh, (STRIP - 16) / 2, T.accent);
	}
	bool on_pad = g_touch.held && touch_in(TX, TP_Y, TW, TP_B - TP_Y) && g_touch.sx >= TX && g_touch.sx < TX + TW && g_touch.sy >= TP_Y;
	bool on_strip = g_touch.held && g_touch.sx >= SX && g_touch.sy >= TP_Y && g_touch.sy < TP_B;
	bool click = false;
	if (ready) {
		// puntero: el dedo en el touchpad o la palanca
		if (on_pad) {
			B.px += (g_touch.x - g_touch.px) * 1.4f;
			B.py += (g_touch.y - g_touch.py) * 1.4f;
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
		if (g_krepeat & KEY_RIGHT)
			focus_step(1);
		if (g_krepeat & KEY_LEFT)
			focus_step(-1);
		if (zin || (g_krepeat & KEY_R))
			zoom_by(1.25f);
		if (zout || (g_krepeat & KEY_L))
			zoom_by(0.8f);
		clamp_view();
		click = (g_touch.up && !g_touch.drag && g_touch.sx >= TX && g_touch.sx < TX + TW && g_touch.sy >= TP_Y && g_touch.sy < TP_B) ||
		        (g_kdown & KEY_A);
		if ((tpc && B.force != 1) || (tmob && B.force != 2) || (g_kdown & KEY_Y)) {
			B.force = (g_kdown & KEY_Y) ? (mob ? 1 : 2) : tpc ? 1 : 2;
			if (strstr(B.url, "://")) { // de Internet: se vuelve a pedir, presentándose como PC o como móvil
				char u2[1024];
				util_strlcpy(u2, B.url, sizeof u2);
				start_load(u2);
			} else if (B.doc)
				B.st = ST_PARSED; // de la SD: se vuelve a colocar (en su hilo)
		}
	}
	// --- debajo: a dónde lleva el enlace del puntero, o cómo se usa
	const char *tip = "Desliza para mover el puntero · toca para hacer clic";
	u32 tc = T.text3;
	if (ready && B.hover >= 0 && B.hover < B.nlinks)
		tip = B.links[B.hover], tc = T.accent;
	else if (B.st == ST_ERROR)
		tip = B.err, tc = T.danger;
	text_draw_center_clip(tip, BOT_W / 2, TP_B + 4, BOT_W - 20, 0.40f, tc);
	char keys[160];
	snprintf(keys, sizeof keys, "%s Clic · %s Enlaces · L R Zoom · %s PC/Móvil · %s Atrás", GLYPH_A, GLYPH_DPAD, GLYPH_Y, GLYPH_B);
	text_draw_center_clip(keys, BOT_W / 2, TP_B + 18, BOT_W - 20, 0.36f, T.text3);
	// --- acciones
	if (tap(BOT_W - 36, 0, 36, HDR_H) || (g_kdown & KEY_START)) {
		close_br();
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
			bool url = strstr(nu, "://") || nu[0] == '/' || (strchr(nu, '.') && !strchr(nu, ' '));
			if (!url) { // palabras sueltas: se buscan (DuckDuckGo en HTML funciona sin JavaScript)
				char q[900];
				url_esc(nu, q, sizeof q);
				snprintf(full, sizeof full, "https://html.duckduckgo.com/html/?q=%s", q);
			} else if (!strstr(nu, "://") && nu[0] != '/')
				snprintf(full, sizeof full, "https://%s", nu);
			else
				util_strlcpy(full, nu, sizeof full);
			go(full, true);
		}
	} else if (click && ready && wdg_at_pointer() >= 0) {
		wdg_activate(wdg_at_pointer());
	} else if (click && ready) {
		int k = link_at_pointer();
		if (k < 0 && B.focus >= 0)
			k = B.f[B.focus].link;
		go_link(k);
	}
	g_kdown = 0;
	g_touch.up = false;
}
