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
#include "dl.h"
#include "gfx.h"
#include "icons.h"
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
} Frag;

typedef struct {
	float y, h, gap;
	bool hr;       /* <hr>: una raya en vez de texto */
	bool used;     /* tiene texto: las vacías no ocupan alto */
} Line;

enum { ST_IDLE, ST_LOADING, ST_PARSED, ST_READY, ST_ERROR };

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
	float total, off, vel;
	int hover;           /* enlace bajo el puntero */
	float t_load;
	/* vista: la página a ancho de PC (o de móvil) arriba, con zoom y puntero */
	bool page_mobile;    /* la página pide ancho de móvil (<meta name=viewport … device-width>) */
	int force;           /* 0 lo que pida la página, 1 PC, 2 móvil */
	float vw, zoom, sx, sy, vy; /* ancho, zoom (px de pantalla por px de página), desplazamiento e inercia */
	float px, py;        /* puntero, en la pantalla de arriba */
	int focus;           /* trozo con enlace marcado con la cruceta (-1 ninguno) */
	float tip;           /* tiempo enseñando el % de zoom */
} B;

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

static void load_thread(void *arg)
{
	(void)arg;
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
	B.doc = doc;
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
	util_strlcpy(B.url, url, sizeof B.url);
	B.err[0] = 0;
	B.title[0] = 0;
	B.st = ST_LOADING;
	B.t_load = 0;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	B.thr = threadCreate(load_thread, NULL, 256 * 1024, prio + 1, -2, false); // hubbub y libdom anidan mucho en páginas grandes
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
	float sc, indent;
	u32 col;
	int link;
	bool pre, under, skip;
	int *olnum;    /* contador de la <ol> de dentro */
	int *cells;    /* celdas de la fila (separadores) */
} St;

static float L_x;          /* posición en la línea actual */
static bool L_has, L_space;
static float L_gap;        /* hueco antes de la próxima línea */
static float L_indent;
static bool L_marker;      /* en la línea solo está la viñeta de un <li> */

static Line *line_new(void)
{
	if (B.nl == B.cl) {
		int nc = B.cl ? B.cl * 2 : 256;
		Line *nl = realloc(B.l, nc * sizeof *nl);
		if (!nl)
			return NULL;
		B.l = nl, B.cl = nc;
	}
	Line *l = &B.l[B.nl++];
	memset(l, 0, sizeof *l);
	l->h = text_lineh(SC_TEXT);
	l->gap = L_gap;
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
	if (B.nl && !L_has) { // la línea actual está vacía: el hueco va delante de ella
		if (gap > B.l[B.nl - 1].gap)
			B.l[B.nl - 1].gap = gap;
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
	f->sc = st->sc, f->col = st->col, f->link = (s16)st->link, f->under = st->under;
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
	if (!B.nl || (L_has && L_x + sp + w > PAGE_W)) {
		line_new();
		sp = 0;
	}
	if (!L_has)
		L_x = L_indent;
	// una palabra más ancha que la pantalla: a trozos
	while (w > PAGE_W - L_indent && n > 1) {
		int k = n;
		while (k > 1 && text_width_n(s, k, st->sc) > PAGE_W - L_x)
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

static void text_run(const char *s, size_t n, const St *st)
{
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

static void walk(dom_node *node, St st)
{
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
	for (int i = 0; i < B.nlinks; i++)
		free(B.links[i]);
	free(B.links), B.links = NULL, B.nlinks = B.clinks = 0;
}

static void layout(void)
{
	layout_free();
	bool mob = B.force ? B.force == 2 : B.page_mobile;
	B.vw = mob ? VW_MOB : VW_PC;
	PADP = mob ? 8 : 16;
	PW = B.vw - 2 * PADP;
	L_x = L_indent = L_gap = 0;
	L_has = L_space = false;
	St st = {.sc = SC_TEXT, .col = PG_TEXT, .link = -1};
	// modo lectura: si la página marca su contenido (<main> o <article>), solo eso (más el <title>)
	dom_node *content = find_tag((dom_node *)B.doc, "main", 0);
	if (!content)
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
	float y = 6;
	for (int i = 0; i < B.nl; i++) {
		if (!B.l[i].used && !B.l[i].hr) { // vacía: solo cuenta su hueco (el mayor de los seguidos)
			B.l[i].h = 0;
			if (i + 1 < B.nl && B.l[i + 1].gap < B.l[i].gap)
				B.l[i + 1].gap = B.l[i].gap;
			B.l[i].gap = 0;
		}
		y += B.l[i].gap;
		B.l[i].y = y;
		y += B.l[i].h;
	}
	B.total = y + 20;
	B.off = B.vel = 0;
	B.zoom = TOP_W / B.vw; // entera a lo ancho; para leer, zoom
	B.sx = B.sy = B.vy = 0;
	B.px = TOP_W / 2, B.py = SCR_H / 2;
	B.focus = -1;
	B.tip = 2.5f;
	util_log("web: %s -> %u bytes, %d líneas, %d trozos, %d enlaces", B.url, (unsigned)B.bytes, B.nl, B.nf, B.nlinks);
}

/* ------------------------------------------------------------ uso */
static void go(const char *url, bool push)
{
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

// el primer trozo cuya línea acaba por debajo de y (búsqueda binaria: los trozos van en orden)
static int first_frag(float y)
{
	int lo = 0, hi = B.nf;
	while (lo < hi) {
		int m = (lo + hi) / 2;
		const Line *l = &B.l[B.f[m].line];
		if (l->y + l->h < y)
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
	for (int i = first_frag(qy - 60); i < B.nf; i++) {
		const Frag *f = &B.f[i];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		if (y > qy + pad)
			break;
		if (f->link >= 0 && qx >= x - pad && qx <= x + w + pad && qy >= y - pad && qy <= y + h + pad)
			return f->link;
	}
	return -1;
}

// cruceta ◀ ▶: al enlace anterior o siguiente; el puntero se pone encima y la vista lo sigue
static void focus_step(int dir)
{
	int i = B.focus;
	if (i < 0) { // el primero que se vea
		i = first_frag(B.sy);
		if (dir < 0)
			i--;
	} else
		i += dir;
	int cur = B.focus >= 0 ? B.f[B.focus].link : -2;
	for (; i >= 0 && i < B.nf; i += dir)
		if (B.f[i].link >= 0 && B.f[i].link != cur)
			break;
	if (i < 0 || i >= B.nf)
		return;
	if (dir < 0) // al principio de ese enlace, no a su última palabra
		while (i > 0 && B.f[i - 1].link == B.f[i].link)
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
			text_draw_center("Cargando…", TOP_W / 2, 158, TS_SUB, T.text3);
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
	int hov = link_at_pointer();
	B.hover = hov;
	for (int i = first_frag(B.sy); i < B.nf; i++) {
		const Frag *f = &B.f[i];
		float x, y, w, h;
		frag_rect(f, &x, &y, &w, &h);
		float X = (x - B.sx) * z, Y = (y - B.sy) * z, Wd = w * z, H = h * z;
		if (Y > SCR_H)
			break;
		if (X > TOP_W || X + Wd < 0 || Y + H < 0)
			continue;
		if (f->link >= 0 && (f->link == hov || i == B.focus))
			draw_rect(X - 1, Y, Wd + 2, H, col_alpha(PG_LINK, 0.18f));
		float sc = f->sc * z;
		if (sc < 0.17f) // tan pequeño que no se leería: una raya del color del texto (como una vista previa)
			draw_rect(X, Y + H * 0.35f, Wd, fmaxf(1, H * 0.35f), col_alpha(f->col, 0.55f));
		else {
			text_draw_n(B.text + f->off, f->n, X, Y, sc, f->col);
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

void br_bottom(void)
{
	if (B.st == ST_PARSED) { // el árbol está listo: se maqueta aquí (usa la fuente); se guarda para cambiar PC / móvil
		if (B.thr) {
			threadJoin(B.thr, U64_MAX);
			threadFree(B.thr);
			B.thr = NULL;
		}
		layout();
		B.st = ST_READY;
		B.hover = -1;
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
			if (B.doc)
				layout();
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
		if (kb_input("Dirección (https://…)", B.url, nu, sizeof nu, false, false) && nu[0]) {
			char full[1024];
			if (!strstr(nu, "://") && nu[0] != '/')
				snprintf(full, sizeof full, "https://%s", nu);
			else
				util_strlcpy(full, nu, sizeof full);
			go(full, true);
		}
	} else if (click && ready) {
		int k = link_at_pointer();
		if (k < 0 && B.focus >= 0)
			k = B.f[B.focus].link;
		go_link(k);
	}
	g_kdown = 0;
	g_touch.up = false;
}
