/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* CSS del navegador con libcss (NetSurf, MIT). libcss lee las hojas y decide qué reglas tocan a
 * cada elemento; para eso pregunta por el árbol (nombre, clases, id, padres, hermanos,
 * atributos) a este «manejador», que lo mira en el DOM de libdom. */
#include "brcss.h"
#include "cssprep.h"
#include "core/node.h" /* la estructura interna de libdom: padre y hermanos sin tocar referencias */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_SHEETS 24

struct BrCss {
	css_select_ctx *ctx;
	css_stylesheet *sheets[MAX_SHEETS];
	int n;          /* todas, con la del navegador */
	char base[1024];
	/* las de la página, en crudo hasta brcss_finish (las variables pueden venir de otra hoja) */
	struct {
		char *css, *url;
		size_t n;
	} raw[MAX_SHEETS];
	int nraw;
	CssPrep *prep;  /* para los style="…" y los temas por ámbitos */
	int nscope;     /* ámbitos de tema que cambian colores */
	float width;
};

/* la hoja del navegador: lo que hace cada etiqueta si la página no dice otra cosa (como el anexo D
 * de CSS 2.1, más las etiquetas de HTML5) */
static const char UA_CSS[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,"
    "center,dir,hr,menu,pre,header,footer,nav,main,section,article,aside,figure,figcaption,details,summary,"
    "hgroup,legend,search{display:block}"
    "li{display:list-item}"
    "head,script,style,noscript,template,title,meta,link,base,datalist,param,source,track,area,map,"
    "canvas,iframe,video,audio,object,embed,dialog,input[type=hidden]{display:none}"
    "svg{display:inline}" /* los dibujos de la página se pintan (como imagen) */
    "input,button,select,textarea{display:inline-block}button{text-align:center}"
    "[hidden]{display:none}"
    "table{display:table;border-spacing:2px}tr{display:table-row}thead{display:table-header-group}"
    "tbody{display:table-row-group}tfoot{display:table-footer-group}col{display:table-column}"
    "colgroup{display:table-column-group}td,th{display:table-cell;padding:1px}"
    "caption{display:table-caption;text-align:center}th{font-weight:bold;text-align:center}"
    "body{margin:8px;color:#202122;background-color:#fff}"
    "h1{font-size:2em;margin:.67em 0}h2{font-size:1.5em;margin:.75em 0}h3{font-size:1.17em;margin:.83em 0}"
    "h4,p,blockquote,ul,fieldset,form,ol,dl,dir,menu{margin:1.12em 0}h5{font-size:.83em;margin:1.5em 0}"
    "h6{font-size:.75em;margin:1.67em 0}"
    "h1,h2,h3,h4,h5,h6,b,strong{font-weight:bold}"
    "blockquote{margin-left:40px;margin-right:40px}i,cite,em,var,address{font-style:italic}"
    "pre,tt,code,kbd,samp{font-family:monospace}pre{white-space:pre}"
    "big{font-size:1.17em}small,sub,sup{font-size:.83em}"
    "ol,ul,dir,menu,dd{margin-left:40px}ol{list-style-type:decimal}ul ul,ol ul{list-style-type:circle}"
    "ol ol,ul ol,ol ul,ul ul{margin-top:0;margin-bottom:0}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "hr{margin:.5em 0}center{text-align:-libcss-center}"
    "a[href]{color:#3366cc;text-decoration:underline}";

/* ------------------------------------------------------------ ayudas sobre libdom */
static bool elem_name_is(dom_node *n, lwc_string *name)
{
	dom_string *nm = NULL;
	if (dom_node_get_node_name(n, &nm) != DOM_NO_ERR || !nm)
		return false;
	bool eq = dom_string_caseless_lwc_isequal(nm, name);
	dom_string_unref(nm);
	return eq;
}

#define NI(n) ((dom_node_internal *)(n))

// el hermano elemento anterior y el siguiente; el árbol no cambia mientras se usa, así que sin
// referencias (soltar una referencia y seguir usando el nodo era justo lo que fallaba)
static dom_node *prev_elem(dom_node *n)
{
	for (dom_node_internal *p = NI(n)->previous; p; p = p->previous)
		if (p->type == DOM_ELEMENT_NODE)
			return (dom_node *)p;
	return NULL;
}

static dom_node *next_elem(dom_node *n)
{
	for (dom_node_internal *p = NI(n)->next; p; p = p->next)
		if (p->type == DOM_ELEMENT_NODE)
			return (dom_node *)p;
	return NULL;
}

// el valor de un atributo (dom_string con referencia, o NULL)
static dom_string *get_attr(dom_node *n, lwc_string *name)
{
	dom_string *k = NULL, *v = NULL;
	if (dom_string_create((const uint8_t *)lwc_string_data(name), lwc_string_length(name), &k) != DOM_NO_ERR)
		return NULL;
	dom_element_get_attribute((dom_element *)n, k, &v);
	dom_string_unref(k);
	return v;
}

/* ------------------------------------------------------------ el manejador */
static css_error node_name(void *pw, void *n, css_qname *q)
{
	(void)pw;
	dom_string *nm = NULL;
	q->ns = NULL;
	q->name = NULL;
	if (dom_node_get_node_name(n, &nm) != DOM_NO_ERR || !nm)
		return CSS_NOMEM;
	char low[64];
	size_t l = dom_string_byte_length(nm);
	if (l > sizeof low - 1)
		l = sizeof low - 1;
	for (size_t i = 0; i < l; i++)
		low[i] = (char)tolower((unsigned char)dom_string_data(nm)[i]);
	dom_string_unref(nm);
	return lwc_intern_string(low, l, &q->name) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}

static css_error node_classes(void *pw, void *n, lwc_string ***classes, uint32_t *nc)
{
	(void)pw;
	*classes = NULL;
	*nc = 0;
	dom_element_get_classes((dom_element *)n, classes, nc);
	return CSS_OK;
}

static css_error node_id(void *pw, void *n, lwc_string **id)
{
	(void)pw;
	*id = NULL;
	lwc_string *k;
	if (lwc_intern_string("id", 2, &k) != lwc_error_ok)
		return CSS_NOMEM;
	dom_string *v = get_attr(n, k);
	lwc_string_unref(k);
	if (v) {
		if (dom_string_byte_length(v))
			dom_string_intern(v, id);
		dom_string_unref(v);
	}
	return CSS_OK;
}

static css_error named_ancestor_node(void *pw, void *n, const css_qname *q, void **anc)
{
	(void)pw;
	*anc = NULL;
	dom_element_named_ancestor_node((dom_element *)n, q->name, (dom_element **)anc);
	if (*anc)
		dom_node_unref(*anc); // sigue vivo: está en el árbol
	return CSS_OK;
}

static css_error named_parent_node(void *pw, void *n, const css_qname *q, void **par)
{
	(void)pw;
	*par = NULL;
	dom_element_named_parent_node((dom_element *)n, q->name, (dom_element **)par);
	if (*par)
		dom_node_unref(*par);
	return CSS_OK;
}

static css_error named_sibling_node(void *pw, void *n, const css_qname *q, void **sib)
{
	(void)pw;
	dom_node *p = prev_elem(n);
	*sib = p && elem_name_is(p, q->name) ? p : NULL;
	return CSS_OK;
}

static css_error named_generic_sibling_node(void *pw, void *n, const css_qname *q, void **sib)
{
	(void)pw;
	*sib = NULL;
	for (dom_node *p = prev_elem(n); p; p = prev_elem(p))
		if (elem_name_is(p, q->name)) {
			*sib = p;
			break;
		}
	return CSS_OK;
}

static css_error parent_node(void *pw, void *n, void **par)
{
	(void)pw;
	*par = NULL;
	dom_element_parent_node((dom_element *)n, (dom_element **)par);
	if (*par)
		dom_node_unref(*par);
	return CSS_OK;
}

static css_error sibling_node(void *pw, void *n, void **sib)
{
	(void)pw;
	*sib = prev_elem(n);
	return CSS_OK;
}

static css_error node_has_name(void *pw, void *n, const css_qname *q, bool *m)
{
	(void)pw;
	*m = (lwc_string_length(q->name) == 1 && lwc_string_data(q->name)[0] == '*') || elem_name_is(n, q->name);
	return CSS_OK;
}

static css_error node_has_class(void *pw, void *n, lwc_string *name, bool *m)
{
	(void)pw;
	*m = false;
	dom_element_has_class((dom_element *)n, name, m);
	return CSS_OK;
}

static css_error node_has_id(void *pw, void *n, lwc_string *name, bool *m)
{
	lwc_string *id = NULL;
	node_id(pw, n, &id);
	*m = id && lwc_string_isequal(id, name, m) == lwc_error_ok && *m;
	if (id)
		lwc_string_unref(id);
	return CSS_OK;
}

static css_error node_has_attribute(void *pw, void *n, const css_qname *q, bool *m)
{
	(void)pw;
	dom_string *v = get_attr(n, q->name);
	*m = v != NULL;
	if (v)
		dom_string_unref(v);
	return CSS_OK;
}

enum { AM_EQ, AM_DASH, AM_INCL, AM_PREFIX, AM_SUFFIX, AM_SUB };
static css_error attr_match(void *n, const css_qname *q, lwc_string *val, bool *m, int how)
{
	*m = false;
	dom_string *v = get_attr(n, q->name);
	if (!v)
		return CSS_OK;
	const char *a = dom_string_data(v), *b = lwc_string_data(val);
	size_t al = dom_string_byte_length(v), bl = lwc_string_length(val);
	switch (how) {
	case AM_EQ: *m = al == bl && !strncasecmp(a, b, al); break;
	case AM_DASH: *m = (al == bl && !strncasecmp(a, b, al)) || (al > bl && a[bl] == '-' && !strncasecmp(a, b, bl)); break;
	case AM_PREFIX: *m = bl && al >= bl && !strncmp(a, b, bl); break;
	case AM_SUFFIX: *m = bl && al >= bl && !strncmp(a + al - bl, b, bl); break;
	case AM_SUB:
		for (size_t i = 0; bl && i + bl <= al && !*m; i++)
			*m = !strncmp(a + i, b, bl);
		break;
	default: // includes: una palabra de la lista separada por espacios
		for (size_t i = 0; bl && i < al && !*m;) {
			while (i < al && isspace((unsigned char)a[i]))
				i++;
			size_t j = i;
			while (j < al && !isspace((unsigned char)a[j]))
				j++;
			*m = j - i == bl && !strncmp(a + i, b, bl);
			i = j;
		}
	}
	dom_string_unref(v);
	return CSS_OK;
}

static css_error node_has_attribute_equal(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_EQ); }
static css_error node_has_attribute_dashmatch(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_DASH); }
static css_error node_has_attribute_includes(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_INCL); }
static css_error node_has_attribute_prefix(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_PREFIX); }
static css_error node_has_attribute_suffix(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_SUFFIX); }
static css_error node_has_attribute_substring(void *pw, void *n, const css_qname *q, lwc_string *v, bool *m) { (void)pw; return attr_match(n, q, v, m, AM_SUB); }

static css_error node_is_root(void *pw, void *n, bool *m)
{
	(void)pw;
	dom_node_internal *p = NI(n)->parent;
	*m = !p || p->type != DOM_ELEMENT_NODE;
	return CSS_OK;
}

static css_error node_count_siblings(void *pw, void *n, bool same_name, bool after, int32_t *count)
{
	(void)pw;
	css_qname q = {0};
	if (same_name)
		node_name(NULL, n, &q);
	int32_t c = 0;
	for (dom_node *p = after ? next_elem(n) : prev_elem(n); p; p = after ? next_elem(p) : prev_elem(p))
		if (!same_name || (q.name && elem_name_is(p, q.name)))
			c++;
	if (q.name)
		lwc_string_unref(q.name);
	*count = c;
	return CSS_OK;
}

static css_error node_is_empty(void *pw, void *n, bool *m)
{
	(void)pw;
	*m = true;
	for (dom_node_internal *c = NI(n)->first_child; c && *m; c = c->next)
		if (c->type == DOM_ELEMENT_NODE || c->type == DOM_TEXT_NODE)
			*m = false;
	return CSS_OK;
}

static css_error node_is_link(void *pw, void *n, bool *m)
{
	(void)pw;
	lwc_string *a, *href;
	*m = false;
	if (lwc_intern_string("a", 1, &a) != lwc_error_ok)
		return CSS_NOMEM;
	if (elem_name_is(n, a) && lwc_intern_string("href", 4, &href) == lwc_error_ok) {
		dom_string *v = get_attr(n, href);
		*m = v != NULL;
		if (v)
			dom_string_unref(v);
		lwc_string_unref(href);
	}
	lwc_string_unref(a);
	return CSS_OK;
}

static css_error no(void *pw, void *n, bool *m)
{
	(void)pw, (void)n;
	*m = false;
	return CSS_OK;
}

static css_error node_is_lang(void *pw, void *n, lwc_string *lang, bool *m)
{
	(void)lang;
	return no(pw, n, m);
}

/* los atributos de presentación del HTML de siempre (bgcolor, width, align, cellpadding, <font color>…):
 * como en un navegador, con la prioridad más baja del autor (cualquier regla CSS de la página manda) */
static bool hattr(dom_node *n, const char *name, char *out, size_t on)
{
	dom_string *k = NULL, *v = NULL;
	out[0] = 0;
	if (dom_string_create((const uint8_t *)name, strlen(name), &k) != DOM_NO_ERR)
		return false;
	dom_element_get_attribute((dom_element *)n, k, &v);
	dom_string_unref(k);
	if (!v)
		return false;
	size_t l = dom_string_byte_length(v);
	if (l >= on)
		l = on - 1;
	memcpy(out, dom_string_data(v), l);
	out[l] = 0;
	dom_string_unref(v);
	while (*out == ' ')
		memmove(out, out + 1, strlen(out));
	return true;
}

// un color de atributo: «#ff6600», «ff6600», «red»…
static bool hcolor(const char *v, css_color *c)
{
	static const struct {
		const char *n;
		uint32_t c;
	} N[] = {{"black", 0}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
	         {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"navy", 0x000080}, {"maroon", 0x800000}, {"purple", 0x800080},
	         {"teal", 0x008080}, {"olive", 0x808000}, {"lime", 0x00FF00}, {"aqua", 0x00FFFF}, {"fuchsia", 0xFF00FF}, {"orange", 0xFFA500}};
	for (size_t i = 0; i < sizeof N / sizeof N[0]; i++)
		if (!strcasecmp(v, N[i].n)) {
			*c = 0xFF000000 | N[i].c;
			return true;
		}
	const char *h = *v == '#' ? v + 1 : v;
	size_t l = strlen(h);
	if (l != 3 && l != 6)
		return false;
	for (size_t i = 0; i < l; i++)
		if (!isxdigit((unsigned char)h[i]))
			return false;
	unsigned x = (unsigned)strtoul(h, NULL, 16);
	if (l == 3)
		x = ((x >> 8) & 15) * 0x110000 + ((x >> 4) & 15) * 0x1100 + (x & 15) * 0x11;
	*c = 0xFF000000 | x;
	return true;
}

// una medida de atributo: «85%», «200», «200px»
static bool hlen(const char *v, css_hint_length *l)
{
	char *e;
	double d = strtod(v, &e);
	if (e == v || d < 0)
		return false;
	l->value = (css_fixed)(d * 1024);
	l->unit = *e == '%' ? CSS_UNIT_PCT : CSS_UNIT_PX;
	return true;
}

static css_hint HINTS[24];

static css_error node_presentational_hint(void *pw, void *n, uint32_t *nh, css_hint **h)
{
	(void)pw;
	*nh = 0;
	*h = NULL;
	dom_string *nm = NULL;
	char tag[12] = "", v[64];
	if (dom_node_get_node_name((dom_node *)n, &nm) == DOM_NO_ERR && nm) {
		size_t l = dom_string_byte_length(nm);
		if (l < sizeof tag)
			for (size_t i = 0; i < l; i++)
				tag[i] = (char)tolower((unsigned char)dom_string_data(nm)[i]);
		dom_string_unref(nm);
	}
	if (!tag[0])
		return CSS_OK;
	int k = 0;
#define HINT(p, st) (HINTS[k].prop = (p), HINTS[k].status = (st), &HINTS[k++])
	bool tbl = !strcmp(tag, "table"), cell = !strcmp(tag, "td") || !strcmp(tag, "th");
	bool row = !strcmp(tag, "tr") || !strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot");
	css_color c;
	css_hint_length L;
	if ((tbl || cell || row || !strcmp(tag, "body")) && hattr(n, "bgcolor", v, sizeof v) && hcolor(v, &c))
		HINT(CSS_PROP_BACKGROUND_COLOR, CSS_BACKGROUND_COLOR_COLOR)->data.color = c;
	bool media = !strcmp(tag, "img") || !strcmp(tag, "video") || !strcmp(tag, "canvas") || !strcmp(tag, "iframe") || !strcmp(tag, "embed") || !strcmp(tag, "object");
	if ((tbl || cell || media || !strcmp(tag, "hr") || !strcmp(tag, "col")) && hattr(n, "width", v, sizeof v) && hlen(v, &L) && L.value > 0)
		HINT(CSS_PROP_WIDTH, CSS_WIDTH_SET)->data.length = L;
	if ((tbl || cell || row || media) && hattr(n, "height", v, sizeof v) && hlen(v, &L) && L.value > 0)
		HINT(CSS_PROP_HEIGHT, CSS_HEIGHT_SET)->data.length = L;
	if (hattr(n, "align", v, sizeof v)) {
		if (tbl || media) {
			if (!strcasecmp(v, "left") || !strcasecmp(v, "right"))
				HINT(CSS_PROP_FLOAT, !strcasecmp(v, "left") ? CSS_FLOAT_LEFT : CSS_FLOAT_RIGHT);
			else if (!strcasecmp(v, "center") && tbl) {
				HINT(CSS_PROP_MARGIN_LEFT, CSS_MARGIN_AUTO);
				HINT(CSS_PROP_MARGIN_RIGHT, CSS_MARGIN_AUTO);
			}
		} else if (cell || row || !strcmp(tag, "div") || !strcmp(tag, "p") || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) ||
		           !strcmp(tag, "caption")) {
			uint8_t a = !strcasecmp(v, "center") || !strcasecmp(v, "middle") ? (cell || row || !strcmp(tag, "div") ? CSS_TEXT_ALIGN_LIBCSS_CENTER : CSS_TEXT_ALIGN_CENTER)
			            : !strcasecmp(v, "right") ? (cell || row || !strcmp(tag, "div") ? CSS_TEXT_ALIGN_LIBCSS_RIGHT : CSS_TEXT_ALIGN_RIGHT)
			            : !strcasecmp(v, "justify") ? CSS_TEXT_ALIGN_JUSTIFY : CSS_TEXT_ALIGN_LEFT;
			HINT(CSS_PROP_TEXT_ALIGN, a);
		}
	}
	if ((cell || row) && hattr(n, "valign", v, sizeof v))
		HINT(CSS_PROP_VERTICAL_ALIGN, !strcasecmp(v, "top") ? CSS_VERTICAL_ALIGN_TOP : !strcasecmp(v, "bottom") ? CSS_VERTICAL_ALIGN_BOTTOM
		                                                                         : CSS_VERTICAL_ALIGN_MIDDLE);
	if (cell && hattr(n, "nowrap", v, sizeof v))
		HINT(CSS_PROP_WHITE_SPACE, CSS_WHITE_SPACE_NOWRAP);
	if (!strcmp(tag, "font")) {
		if (hattr(n, "color", v, sizeof v) && hcolor(v, &c))
			HINT(CSS_PROP_COLOR, CSS_COLOR_COLOR)->data.color = c;
		if (hattr(n, "size", v, sizeof v) && v[0]) { // 1…7 (o +1, -1 sobre 3)
			int sz = atoi(v[0] == '+' || v[0] == '-' ? v + 1 : v);
			if (v[0] == '+')
				sz = 3 + sz;
			else if (v[0] == '-')
				sz = 3 - sz;
			static const float PX[] = {10, 13, 16, 18, 24, 32, 48};
			sz = sz < 1 ? 1 : sz > 7 ? 7 : sz;
			css_hint *x = HINT(CSS_PROP_FONT_SIZE, CSS_FONT_SIZE_DIMENSION);
			x->data.length.value = (css_fixed)(PX[sz - 1] * 1024), x->data.length.unit = CSS_UNIT_PX;
		}
	}
	if (!strcmp(tag, "body") && hattr(n, "text", v, sizeof v) && hcolor(v, &c))
		HINT(CSS_PROP_COLOR, CSS_COLOR_COLOR)->data.color = c;
	if (tbl) {
		if (hattr(n, "cellspacing", v, sizeof v) && hlen(v, &L)) {
			css_hint *x = HINT(CSS_PROP_BORDER_SPACING, CSS_BORDER_SPACING_SET);
			x->data.position.h = L, x->data.position.v = L;
		}
		if (hattr(n, "border", v, sizeof v) && hlen(v, &L) && L.value > 0) {
			for (int i = 0; i < 4; i++) {
				HINT(CSS_PROP_BORDER_TOP_WIDTH + i, CSS_BORDER_WIDTH_WIDTH)->data.length = L;
				HINT(CSS_PROP_BORDER_TOP_STYLE + i, CSS_BORDER_STYLE_OUTSET);
			}
		}
	}
	if (cell) { // de su tabla: cellpadding y border (un borde de 1 px en las celdas)
		dom_node_internal *t = NI(n)->parent;
		for (int up = 0; t && up < 4; up++, t = t->parent) {
			dom_string *tn = NULL;
			bool is = false;
			if (t->type == DOM_ELEMENT_NODE && dom_node_get_node_name((dom_node *)t, &tn) == DOM_NO_ERR && tn) {
				is = dom_string_byte_length(tn) == 5 && !strncasecmp(dom_string_data(tn), "table", 5);
				dom_string_unref(tn);
			}
			if (!is)
				continue;
			if (hattr((dom_node *)t, "cellpadding", v, sizeof v) && hlen(v, &L))
				for (int i = 0; i < 4; i++)
					HINT(CSS_PROP_PADDING_TOP + i, CSS_PADDING_SET)->data.length = L;
			if (hattr((dom_node *)t, "border", v, sizeof v) && hlen(v, &L) && L.value > 0) {
				L.value = 1024, L.unit = CSS_UNIT_PX;
				for (int i = 0; i < 4 && k < 20; i++) {
					HINT(CSS_PROP_BORDER_TOP_WIDTH + i, CSS_BORDER_WIDTH_WIDTH)->data.length = L;
					HINT(CSS_PROP_BORDER_TOP_STYLE + i, CSS_BORDER_STYLE_INSET);
				}
			}
			break;
		}
	}
#undef HINT
	if (k) {
		*nh = (uint32_t)k;
		*h = HINTS;
	}
	return CSS_OK;
}

static css_error ua_default_for_property(void *pw, uint32_t prop, css_hint *hint)
{
	(void)pw;
	if (prop == CSS_PROP_COLOR) {
		hint->data.color = 0xFF202122;
		hint->status = CSS_COLOR_COLOR;
	} else if (prop == CSS_PROP_FONT_FAMILY) {
		hint->data.strings = NULL;
		hint->status = CSS_FONT_FAMILY_SANS_SERIF;
	} else if (prop == CSS_PROP_QUOTES) {
		hint->data.strings = NULL;
		hint->status = CSS_QUOTES_NONE;
	} else if (prop == CSS_PROP_VOICE_FAMILY) {
		hint->data.strings = NULL;
		hint->status = 0;
	} else
		return CSS_INVALID;
	return CSS_OK;
}

static css_select_handler H;

/* libcss guarda en cada nodo lo que calculó (un filtro con sus antepasados y su estilo) para que los
 * hijos vayan rápido; sin esto, cada elemento repasa todo su camino hasta la raíz */
static dom_string *KEY;

static void user_data_cb(dom_node_operation op, dom_string *key, void *data, struct dom_node *src, struct dom_node *dst)
{
	(void)key;
	if (op == DOM_NODE_DELETED)
		css_libcss_node_data_handler(&H, CSS_NODE_DELETED, NULL, src, NULL, data);
	else if (op == DOM_NODE_CLONED)
		css_libcss_node_data_handler(&H, CSS_NODE_CLONED, NULL, src, dst, data);
}

static css_error set_node_data(void *pw, void *n, void *data)
{
	(void)pw;
	if (!KEY && dom_string_create_interned((const uint8_t *)"__c3css", 7, &KEY) != DOM_NO_ERR)
		return CSS_NOMEM;
	void *old = NULL;
	if (dom_node_set_user_data((dom_node *)n, KEY, data, user_data_cb, &old) != DOM_NO_ERR)
		return CSS_NOMEM;
	if (old && old != data)
		css_libcss_node_data_handler(&H, CSS_NODE_DELETED, NULL, n, NULL, old);
	return CSS_OK;
}

static css_error get_node_data(void *pw, void *n, void **data)
{
	(void)pw;
	*data = NULL;
	if (KEY)
		dom_node_get_user_data((dom_node *)n, KEY, data);
	return CSS_OK;
}

static css_select_handler H = {
    CSS_SELECT_HANDLER_VERSION_1,
    node_name, node_classes, node_id,
    named_ancestor_node, named_parent_node, named_sibling_node, named_generic_sibling_node,
    parent_node, sibling_node,
    node_has_name, node_has_class, node_has_id, node_has_attribute,
    node_has_attribute_equal, node_has_attribute_dashmatch, node_has_attribute_includes,
    node_has_attribute_prefix, node_has_attribute_suffix, node_has_attribute_substring,
    node_is_root, node_count_siblings, node_is_empty,
    node_is_link, no, no, no, no, // visited, hover, active, focus
    no, no, no, no,               // enabled, disabled, checked, target
    node_is_lang, node_presentational_hint, ua_default_for_property,
    set_node_data, get_node_data,
};

/* ------------------------------------------------------------ hojas */
// las url() de una hoja (fondos con imagen), respecto a la dirección de la hoja
static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
	(void)pw;
	const char *r = lwc_string_data(rel);
	size_t rl = lwc_string_length(rel);
	char out[1024];
	if (!base || strstr(base, "about:") == base || memchr(r, ':', rl < 12 ? rl : 12) || !rl) { // ya completa (o data:)
		*abs = lwc_string_ref(rel);
		return CSS_OK;
	}
	const char *sch = strstr(base, "://");
	if (!sch) {
		*abs = lwc_string_ref(rel);
		return CSS_OK;
	}
	size_t hl = (size_t)(sch + 3 - base);
	hl += strcspn(base + hl, "/?#"); // hasta el final del servidor
	if (rl > 1 && r[0] == '/' && r[1] == '/') // //servidor/…
		snprintf(out, sizeof out, "%.*s%.*s", (int)(sch + 1 - base), base, (int)rl, r);
	else if (r[0] == '/')
		snprintf(out, sizeof out, "%.*s%.*s", (int)hl, base, (int)rl, r);
	else {
		size_t bl = strcspn(base, "?#");
		while (bl > hl && base[bl - 1] != '/')
			bl--;
		if (bl <= hl) // sin carpeta: la raíz
			snprintf(out, sizeof out, "%.*s/%.*s", (int)hl, base, (int)rl, r);
		else
			snprintf(out, sizeof out, "%.*s%.*s", (int)bl, base, (int)rl, r);
		// «../» y «./»
		char *p;
		while ((p = strstr(out + hl, "/./")))
			memmove(p, p + 2, strlen(p + 2) + 1);
		while ((p = strstr(out + hl, "/../"))) { // «a/b/../c» → «a/c»
			char *q = p;
			if (q > out + hl) {
				q--;
				while (q > out + hl && *q != '/')
					q--;
			}
			memmove(q, p + 3, strlen(p + 3) + 1);
		}
	}
	return lwc_intern_string(out, strlen(out), abs) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}

static css_stylesheet *sheet_new(const char *url, bool inl)
{
	css_stylesheet_params p = {0};
	p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
	p.level = CSS_LEVEL_3;
	p.charset = "UTF-8";
	p.url = url && url[0] ? url : "about:blank";
	p.title = NULL;
	p.allow_quirks = true;
	p.inline_style = inl;
	p.resolve = resolve_url;
	css_stylesheet *s = NULL;
	return css_stylesheet_create(&p, &s) == CSS_OK ? s : NULL;
}

static css_stylesheet *sheet_parse(const char *css, size_t n, const char *url, bool inl)
{
	css_stylesheet *s = sheet_new(url, inl);
	if (!s)
		return NULL;
	css_error e = css_stylesheet_append_data(s, (const uint8_t *)css, n);
	if (e != CSS_OK && e != CSS_NEEDDATA) {
		css_stylesheet_destroy(s);
		return NULL;
	}
	e = css_stylesheet_data_done(s);
	if (e != CSS_OK && e != CSS_IMPORTS_PENDING) { // los @import se ignoran
		css_stylesheet_destroy(s);
		return NULL;
	}
	return s;
}

BrCss *brcss_new(const char *base_url)
{
	BrCss *c = calloc(1, sizeof *c);
	if (!c)
		return NULL;
	snprintf(c->base, sizeof c->base, "%s", base_url ? base_url : "");
	if (css_select_ctx_create(&c->ctx) != CSS_OK) {
		free(c);
		return NULL;
	}
	css_stylesheet *ua = sheet_parse(UA_CSS, sizeof UA_CSS - 1, "about:ua", false);
	if (ua && css_select_ctx_append_sheet(c->ctx, ua, CSS_ORIGIN_UA, NULL) == CSS_OK)
		c->sheets[c->n++] = ua;
	else if (ua)
		css_stylesheet_destroy(ua);
	return c;
}

void brcss_add(BrCss *c, const char *css, size_t n, const char *url)
{
	if (!c || !n)
		return;
	const char *uu = url ? url : c->base;
	// seguida de otra con la misma dirección (los <style> de la página: Google tiene casi 40), o sin sitio para
	// más: va detrás de la anterior, en la misma
	if (c->nraw && (!strcmp(c->raw[c->nraw - 1].url, uu) || c->n + c->nraw >= MAX_SHEETS)) {
		size_t on = c->raw[c->nraw - 1].n;
		char *np = realloc(c->raw[c->nraw - 1].css, on + n + 2);
		if (!np)
			return;
		np[on] = '\n';
		memcpy(np + on + 1, css, n);
		np[on + 1 + n] = 0;
		c->raw[c->nraw - 1].css = np, c->raw[c->nraw - 1].n = on + 1 + n;
		return;
	}
	if (c->n + c->nraw >= MAX_SHEETS)
		return;
	char *cp = malloc(n + 1), *u = strdup(uu);
	if (!cp || !u) {
		free(cp), free(u);
		return;
	}
	memcpy(cp, css, n);
	cp[n] = 0;
	c->raw[c->nraw].css = cp, c->raw[c->nraw].url = u, c->raw[c->nraw].n = n;
	c->nraw++;
}

// los atributos de un elemento, «nombre=valor\n…»
static void attrs_of(dom_node *el, char *out, size_t on)
{
	out[0] = 0;
	dom_namednodemap *m = NULL;
	if (!el || dom_node_get_attributes(el, &m) != DOM_NO_ERR || !m)
		return;
	uint32_t n = 0;
	dom_namednodemap_get_length(m, &n);
	size_t used = 0;
	for (uint32_t i = 0; i < n; i++) {
		dom_node *a = NULL;
		if (dom_namednodemap_item(m, i, &a) != DOM_NO_ERR || !a)
			continue;
		dom_string *k = NULL, *v = NULL;
		dom_attr_get_name((dom_attr *)a, &k);
		dom_attr_get_value((dom_attr *)a, &v);
		if (k) {
			int w = snprintf(out + used, on - used, "%.*s=%.*s\n", (int)dom_string_byte_length(k), dom_string_data(k), v ? (int)dom_string_byte_length(v) : 0,
			                 v ? dom_string_data(v) : "");
			if (w > 0 && used + (size_t)w < on)
				used += (size_t)w;
			else
				out[used] = 0;
		}
		if (k)
			dom_string_unref(k);
		if (v)
			dom_string_unref(v);
		dom_node_unref(a);
	}
	dom_namednodemap_unref(m);
}

void brcss_finish(BrCss *c, dom_document *doc, float width)
{
	if (!c)
		return;
	dom_document_quirks_mode qm = DOM_DOCUMENT_QUIRKS_MODE_NONE;
	if (doc && dom_document_get_quirks_mode(doc, &qm) == DOM_NO_ERR && qm == DOM_DOCUMENT_QUIRKS_MODE_FULL && c->n < MAX_SHEETS) {
		// sin <!DOCTYPE> (modo «quirks»), como Chrome: una tabla no hereda text-align (Hacker News)
		static const char Q[] = "table{text-align:left}";
		css_stylesheet *q = sheet_parse(Q, sizeof Q - 1, c->base, false);
		if (q && css_select_ctx_append_sheet(c->ctx, q, CSS_ORIGIN_UA, NULL) == CSS_OK)
			c->sheets[c->n++] = q;
		else if (q)
			css_stylesheet_destroy(q);
	}
	char *ha = malloc(4096), *ba = malloc(4096);
	if (ha && ba) {
		ha[0] = ba[0] = 0;
		dom_element *root = NULL;
		if (doc && dom_document_get_document_element(doc, &root) == DOM_NO_ERR && root) {
			attrs_of((dom_node *)root, ha, 4096);
			for (dom_node_internal *ch = NI(root)->first_child; ch; ch = ch->next)
				if (ch->type == DOM_ELEMENT_NODE && ch->name && dom_string_byte_length(ch->name) == 4 &&
				    !strncasecmp(dom_string_data(ch->name), "body", 4)) {
					attrs_of((dom_node *)ch, ba, 4096);
					break;
				}
			dom_node_unref(root);
		}
		c->width = width;
		c->prep = cssprep_new(width, ha, ba);
	}
	free(ha), free(ba);
	for (int i = 0; i < c->nraw && c->prep; i++)
		cssprep_vars(c->prep, c->raw[i].css, c->raw[i].n);
	// las variables de los style="--x: …" de la página (las hojas las usan: anchos de paneles, colores…):
	// como respaldo, para cuando una regla no tiene otra
	lwc_string *ks = NULL;
	if (c->prep && doc && lwc_intern_string("style", 5, &ks) == lwc_error_ok) {
		dom_node_internal *stack[64];
		int sp = 0, found = 0;
		char *buf = malloc(8192);
		dom_element *root = NULL;
		if (buf && dom_document_get_document_element(doc, &root) == DOM_NO_ERR && root) {
			for (dom_node_internal *n = NI(root); n && found < 2000;) {
				if (n->type == DOM_ELEMENT_NODE) {
					dom_string *v = get_attr((dom_node *)n, ks);
					if (v) {
						size_t l = dom_string_byte_length(v);
						const char *d = dom_string_data(v);
						if (l > 4 && l < 8000) {
							int bl = snprintf(buf, 8192, "z{%.*s}", (int)l, d);
							if (strstr(buf, "--")) {
								cssprep_vars(c->prep, buf, (size_t)bl);
								cssprep_inline_vars(c->prep, d, l);
								found++;
							}
						}
						dom_string_unref(v);
					}
					if (n->first_child && sp < 64) {
						stack[sp++] = n;
						n = n->first_child;
						continue;
					}
				}
				while (n && !n->next && sp)
					n = stack[--sp];
				n = n && n != NI(root) ? n->next : NULL;
			}
			dom_node_unref(root);
		}
		free(buf);
		lwc_string_unref(ks);
	}
	c->nscope = c->prep ? cssprep_scopes(c->prep) : 0; // los temas por ámbitos («[data-color-mode=dark]»…)
	for (int i = 0; i < c->nraw; i++) {
		size_t on = 0;
		char *o = c->prep ? cssprep_rewrite(c->prep, c->raw[i].css, c->raw[i].n, &on) : NULL;
		free(c->raw[i].css); // lo crudo ya no hace falta: menos memoria antes de que libcss lea
		c->raw[i].css = NULL;
		css_stylesheet *s = o && on ? sheet_parse(o, on, c->raw[i].url, false) : NULL;
		free(o);
		free(c->raw[i].url);
		if (!s)
			continue;
		if (c->n < MAX_SHEETS && css_select_ctx_append_sheet(c->ctx, s, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK)
			c->sheets[c->n++] = s;
		else
			css_stylesheet_destroy(s);
	}
	c->nraw = 0;
}

int brcss_scope_of(BrCss *c, dom_node *el, int parent)
{
	static char a[2048];
	if (!c || !c->prep || !c->nscope)
		return parent;
	attrs_of(el, a, sizeof a);
	return cssprep_scope_match(c->prep, a, parent);
}

uint32_t brcss_scope_col(const BrCss *c, int scope, const css_computed_style *cs, uint32_t col, int kind)
{
	if (!c || scope < 0)
		return col;
	int32_t vid = 0;
	if (kind == 1 && css_computed_widows(cs, &vid) != CSS_WIDOWS_SET)
		vid = 0;
	else if (kind == 2 && css_computed_orphans(cs, &vid) != CSS_ORPHANS_SET)
		vid = 0;
	cssprep_scope_col(c->prep, scope, &col, kind, kind == 3 ? 0 : FIXTOINT(vid)); // (css_fixed; lo de serie, 2, da 0)
	return col;
}

int brcss_sheets(const BrCss *c)
{
	return c ? c->n - 1 : 0;
}

void brcss_free(BrCss *c)
{
	if (!c)
		return;
	css_select_ctx_destroy(c->ctx);
	for (int i = 0; i < c->n; i++)
		css_stylesheet_destroy(c->sheets[i]);
	for (int i = 0; i < c->nraw; i++)
		free(c->raw[i].css), free(c->raw[i].url);
	cssprep_free(c->prep);
	free(c);
}

/* ------------------------------------------------------------ estilos */
static css_unit_ctx unit_ctx(float vw)
{
	css_unit_ctx u = {0};
	u.viewport_width = INTTOFIX((int)vw);
	u.viewport_height = INTTOFIX(600);
	u.font_size_default = INTTOFIX(16);
	u.font_size_minimum = INTTOFIX(6);
	u.device_dpi = INTTOFIX(96);
	return u;
}

BrcssPseudo brcss_ps;

css_computed_style *brcss_style(BrCss *c, dom_node *el, const css_computed_style *parent, float vw)
{
	return brcss_style_env(c, el, parent, vw, NULL, NULL);
}

// las «--x: …» de un style="", en «--x:…;»
static void own_vars(const char *d, size_t n, char *out, size_t on)
{
	size_t k = 0;
	out[0] = 0;
	for (size_t i = 0; i < n;) {
		while (i < n && (d[i] == ' ' || d[i] == ';' || d[i] == '\n' || d[i] == '\t'))
			i++;
		size_t j = i;
		int dd = 0;
		while (j < n && (dd || d[j] != ';')) {
			if (d[j] == '(')
				dd++;
			else if (d[j] == ')' && dd)
				dd--;
			j++;
		}
		if (j - i > 2 && d[i] == '-' && d[i + 1] == '-' && k + (j - i) + 2 < on) {
			memcpy(out + k, d + i, j - i);
			k += j - i;
			out[k++] = ';';
			out[k] = 0;
		}
		i = j + 1;
	}
}

css_computed_style *brcss_style_env(BrCss *c, dom_node *el, const css_computed_style *parent, float vw, const char *penv, char **env_out)
{
	if (env_out)
		*env_out = NULL;
	if (!c)
		return NULL;
	css_unit_ctx u = unit_ctx(vw);
	css_media media = {.type = CSS_MEDIA_SCREEN, .width = INTTOFIX((int)vw), .height = INTTOFIX(600)};
	// el atributo style="…": una hoja pequeña solo para este elemento
	css_stylesheet *inl = NULL;
	lwc_string *k;
	static char own[2048];
	own[0] = 0;
	char *o = NULL;
	size_t on = 0;
	bool rawst = false;
	dom_string *v = NULL;
	if (lwc_intern_string("style", 5, &k) == lwc_error_ok) {
		v = get_attr(el, k);
		lwc_string_unref(k);
		if (v && dom_string_byte_length(v)) {
			o = c->prep ? cssprep_inline(c->prep, dom_string_data(v), dom_string_byte_length(v), &on) : NULL;
			rawst = !o;
			if (memchr(dom_string_data(v), '-', dom_string_byte_length(v)))
				own_vars(dom_string_data(v), dom_string_byte_length(v), own, sizeof own);
		}
	}
	// las variables de los style="" de él y de sus antepasados: lo de las reglas que las usan, resuelto
	if (c->prep && ((penv && *penv) || own[0])) {
		static char tag[16], at[2048];
		dom_string *nm = NULL;
		tag[0] = 0;
		if (dom_node_get_node_name(el, &nm) == DOM_NO_ERR && nm) {
			snprintf(tag, sizeof tag, "%.*s", (int)dom_string_byte_length(nm), dom_string_data(nm));
			dom_string_unref(nm);
		}
		attrs_of(el, at, sizeof at);
		char *ex = cssprep_env_decls(c->prep, tag, at, penv, own);
		if (ex && !rawst) { // (detrás de lo del style="", que manda)
			size_t xl = strlen(ex);
			char *no = malloc(on + xl + 1);
			if (no) {
				memcpy(no, ex, xl);
				if (o)
					memcpy(no + xl, o, on);
				no[xl + on] = 0;
				free(o);
				o = no, on += xl;
			}
		}
		free(ex);
		if (own[0] && env_out) {
			size_t pl = penv ? strlen(penv) : 0, ol = strlen(own);
			char *e = malloc(pl + ol + 1);
			if (e) {
				if (pl)
					memcpy(e, penv, pl);
				memcpy(e + pl, own, ol + 1);
				*env_out = e;
			}
		}
	}
	if (o)
		inl = on ? sheet_parse(o, on, c->base, true) : NULL;
	else if (rawst && v)
		inl = sheet_parse(dom_string_data(v), dom_string_byte_length(v), c->base, true);
	free(o);
	if (v)
		dom_string_unref(v);
	css_select_results *res = NULL;
	css_error e = css_select_style(c->ctx, el, &u, &media, inl, &H, NULL, &res);
	if (inl)
		css_stylesheet_destroy(inl);
	if (e != CSS_OK || !res)
		return NULL;
	css_computed_style *s = res->styles[CSS_PSEUDO_ELEMENT_NONE];
	res->styles[CSS_PSEUDO_ELEMENT_NONE] = NULL;
	for (int k = 0; k < 2; k++) // (los que nadie tomó)
		if (brcss_ps.st[k])
			css_computed_style_destroy(brcss_ps.st[k]);
	memset(&brcss_ps, 0, sizeof brcss_ps);
	css_computed_style *raw[2] = {NULL, NULL};
	for (int k = 0; k < 2; k++) { // ::before y ::after: su texto (y si el de después cierra los flotantes)
		const css_computed_style *ps = res->styles[k ? CSS_PSEUDO_ELEMENT_AFTER : CSS_PSEUDO_ELEMENT_BEFORE];
		if (!ps || css_computed_display(ps, false) == CSS_DISPLAY_NONE)
			continue;
		const css_computed_content_item *ci = NULL;
		if (css_computed_content(ps, &ci) != CSS_CONTENT_SET || !ci)
			continue;
		uint8_t cl = css_computed_clear(ps);
		if (k && (cl == CSS_CLEAR_BOTH || cl == CSS_CLEAR_LEFT || cl == CSS_CLEAR_RIGHT))
			brcss_ps.after_clear = true;
		char *o = k ? brcss_ps.after : brcss_ps.before;
		size_t on = 0;
		for (; ci->type != CSS_COMPUTED_CONTENT_NONE; ci++)
			if (ci->type == CSS_COMPUTED_CONTENT_STRING && ci->data.string) {
				const char *t = lwc_string_data(ci->data.string);
				size_t tl = lwc_string_length(ci->data.string);
				bool pua = false; // iconos de fuentes propias (U+E000…U+F8FF): sin esa fuente, cuadros
				for (size_t i = 0; i + 1 < tl; i++)
					if ((unsigned char)t[i] == 0xEE || ((unsigned char)t[i] == 0xEF && (unsigned char)t[i + 1] < 0xA4))
						pua = true;
				if (pua)
					brcss_ps.icon[k] = true;
				else if (on + tl < sizeof brcss_ps.after)
					memcpy(o + on, t, tl), on += tl;
			}
		o[on] = 0;
		raw[k] = res->styles[k ? CSS_PSEUDO_ELEMENT_AFTER : CSS_PSEUDO_ELEMENT_BEFORE];
		res->styles[k ? CSS_PSEUDO_ELEMENT_AFTER : CSS_PSEUDO_ELEMENT_BEFORE] = NULL;
	}
	css_select_results_destroy(res);
	if (s && parent) { // lo que se hereda (color, letra…) y los tamaños relativos, del padre
		css_computed_style *cs = NULL;
		if (css_computed_style_compose(parent, s, &u, &cs) == CSS_OK && cs) {
			css_computed_style_destroy(s);
			s = cs;
		}
	}
	for (int k = 0; k < 2; k++) { // ::before y ::after heredan del elemento
		css_computed_style *cs = NULL;
		if (raw[k] && s && css_computed_style_compose(s, raw[k], &u, &cs) == CSS_OK && cs)
			brcss_ps.st[k] = cs;
		if (raw[k])
			css_computed_style_destroy(raw[k]);
	}
	return s;
}

bool brcss_width_calc(const css_computed_style *s, float vw, float *px, float *pct)
{
	css_fixed v;
	css_unit un;
	if (css_computed_width(s, &v, &un) == CSS_WIDTH_SET) // (con calc() dice auto)
		return false;
	css_unit_ctx u = unit_ctx(vw);
	int a = 0, b = 0; // es lineal en el ancho disponible: con 0 y con 10000, sus dos partes
	if (css_computed_width_px(s, &u, 0, &a) != CSS_WIDTH_SET || css_computed_width_px(s, &u, 10000, &b) != CSS_WIDTH_SET)
		return false;
	*px = (float)a;
	*pct = (float)(b - a) / 100;
	return true;
}

float brcss_px(const css_computed_style *s, css_fixed len, css_unit unit, float vw)
{
	// las absolutas, aquí: libcss redondea los px de cada unidad a entero (1pt = 1,33 px quedaba en 1 px)
	float f = FIXTOFLT(len);
	switch (unit) {
	case CSS_UNIT_PT: return f * 96 / 72;
	case CSS_UNIT_PC: return f * 16;
	case CSS_UNIT_IN: return f * 96;
	case CSS_UNIT_CM: return f * 96 / 2.54f;
	case CSS_UNIT_MM: return f * 96 / 25.4f;
	case CSS_UNIT_Q: return f * 96 / 101.6f;
	default: break;
	}
	css_unit_ctx u = unit_ctx(vw);
	return FIXTOFLT(css_unit_len2css_px(s, &u, len, unit));
}

/* olvidar lo que libcss guardó en los nodos (tras cambiar el árbol desde JavaScript: si no, reutiliza
 * estilos calculados con las clases de antes) */
void brcss_forget(dom_node *n)
{
	if (!KEY || !n)
		return;
	void *old = NULL;
	dom_node_set_user_data(n, KEY, NULL, user_data_cb, &old);
	if (old)
		css_libcss_node_data_handler(&H, CSS_NODE_DELETED, NULL, n, NULL, old);
	for (dom_node_internal *c = NI(n)->first_child; c; c = c->next)
		if (c->type == DOM_ELEMENT_NODE || c->type == DOM_DOCUMENT_NODE)
			brcss_forget((dom_node *)c);
}
