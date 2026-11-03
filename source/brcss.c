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
};

/* la hoja del navegador: lo que hace cada etiqueta si la página no dice otra cosa (como el anexo D
 * de CSS 2.1, más las etiquetas de HTML5) */
static const char UA_CSS[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,"
    "center,dir,hr,menu,pre,header,footer,nav,main,section,article,aside,figure,figcaption,details,summary,"
    "hgroup,legend,search{display:block}"
    "li{display:list-item}"
    "head,script,style,noscript,template,title,meta,link,base,datalist,param,source,track,area,map,"
    "svg,canvas,iframe,video,audio,object,embed,dialog,input[type=hidden]{display:none}"
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
    "hr{margin:.5em 0}center{text-align:center}"
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

static css_error node_presentational_hint(void *pw, void *n, uint32_t *nh, css_hint **h)
{
	(void)pw, (void)n;
	*nh = 0;
	*h = NULL;
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
static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
	(void)pw, (void)base;
	*abs = lwc_string_ref(rel); // las url() de fondos e imágenes no se usan
	return CSS_OK;
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
	if (!c || c->n >= MAX_SHEETS || !n)
		return;
	css_stylesheet *s = sheet_parse(css, n, url ? url : c->base, false);
	if (!s)
		return;
	if (css_select_ctx_append_sheet(c->ctx, s, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK)
		c->sheets[c->n++] = s;
	else
		css_stylesheet_destroy(s);
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

css_computed_style *brcss_style(BrCss *c, dom_node *el, const css_computed_style *parent, float vw)
{
	if (!c)
		return NULL;
	css_unit_ctx u = unit_ctx(vw);
	css_media media = {.type = CSS_MEDIA_SCREEN, .width = INTTOFIX((int)vw), .height = INTTOFIX(600)};
	// el atributo style="…": una hoja pequeña solo para este elemento
	css_stylesheet *inl = NULL;
	lwc_string *k;
	if (lwc_intern_string("style", 5, &k) == lwc_error_ok) {
		dom_string *v = get_attr(el, k);
		lwc_string_unref(k);
		if (v) {
			if (dom_string_byte_length(v))
				inl = sheet_parse(dom_string_data(v), dom_string_byte_length(v), c->base, true);
			dom_string_unref(v);
		}
	}
	css_select_results *res = NULL;
	css_error e = css_select_style(c->ctx, el, &u, &media, inl, &H, NULL, &res);
	if (inl)
		css_stylesheet_destroy(inl);
	if (e != CSS_OK || !res)
		return NULL;
	css_computed_style *s = res->styles[CSS_PSEUDO_ELEMENT_NONE];
	res->styles[CSS_PSEUDO_ELEMENT_NONE] = NULL;
	css_select_results_destroy(res);
	if (s && parent) { // lo que se hereda (color, letra…) y los tamaños relativos, del padre
		css_computed_style *cs = NULL;
		if (css_computed_style_compose(parent, s, &u, &cs) == CSS_OK && cs) {
			css_computed_style_destroy(s);
			s = cs;
		}
	}
	return s;
}

float brcss_px(const css_computed_style *s, css_fixed len, css_unit unit, float vw)
{
	css_unit_ctx u = unit_ctx(vw);
	return FIXTOFLT(css_unit_len2css_px(s, &u, len, unit));
}
