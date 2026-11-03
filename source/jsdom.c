/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* JavaScript de las páginas. Aquí solo hay lo mínimo en C sobre libdom (moverse por el árbol,
 * atributos, texto, crear, meter y quitar nodos, leer HTML, pedir por la red); el DOM que ve una
 * página (document, Element, classList, style, eventos, temporizadores…) está en romfs:/js/dom.js,
 * escrito en JavaScript encima de esto. */
#include "jsdom.h"

#include "quickjs.h"
#include <dom/bindings/hubbub/parser.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef HOST_TEST
#include <time.h>
typedef unsigned long long u64;
static u64 osGetTime(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
#define DOM_JS getenv("DOM_JS")
#else
#include <3ds.h>
#define DOM_JS "romfs:/js/dom.js"
#endif

#define JS_MEM   (24 * 1024 * 1024)
#define JS_STACK (200 * 1024)
#define SCRIPT_MAX (3 * 1024 * 1024)

static struct {
	dom_document *doc;
	const JsHost *host;
	JsResult *res;
	u64 t0;
	JSClassID cls;
} J;

static u64 now_ms(void)
{
	return osGetTime();
}

/* ------------------------------------------------------------ nodos <-> JavaScript */
static void node_fin(JSRuntime *rt, JSValue v)
{
	(void)rt;
	dom_node *n = JS_GetOpaque(v, J.cls);
	if (n)
		dom_node_unref(n);
}

static JSClassDef NODE_CLASS = {"N", .finalizer = node_fin};

static JSValue wrap(JSContext *ctx, dom_node *n) // se queda una referencia (la suelta el finalizador)
{
	if (!n)
		return JS_NULL;
	JSValue o = JS_NewObjectClass(ctx, J.cls);
	if (JS_IsException(o))
		return o;
	JS_SetOpaque(o, dom_node_ref(n));
	return o;
}

static dom_node *unwrap(JSValueConst v)
{
	return JS_GetOpaque(v, J.cls);
}

static dom_string *dstr(JSContext *ctx, JSValueConst v)
{
	size_t l;
	const char *s = JS_ToCStringLen(ctx, &l, v);
	if (!s)
		return NULL;
	dom_string *d = NULL;
	dom_string_create((const uint8_t *)s, l, &d);
	JS_FreeCString(ctx, s);
	return d;
}

static JSValue jstr(JSContext *ctx, dom_string *d) // suelta d
{
	if (!d)
		return JS_NULL;
	JSValue v = JS_NewStringLen(ctx, dom_string_data(d), dom_string_byte_length(d));
	dom_string_unref(d);
	return v;
}

#define N0 dom_node *n = argc > 0 ? unwrap(argv[0]) : NULL; if (!n) return JS_NULL

static JSValue f_doc(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t, (void)argc, (void)argv;
	return wrap(ctx, (dom_node *)J.doc);
}

static JSValue f_type(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_node_type ty = 0;
	dom_node_get_node_type(n, &ty);
	return JS_NewInt32(ctx, ty);
}

static JSValue f_name(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_string *s = NULL;
	dom_node_get_node_name(n, &s);
	return jstr(ctx, s);
}

static JSValue f_id(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	return JS_NewFloat64(ctx, (double)(uintptr_t)n);
}

enum { R_PARENT, R_FIRST, R_LAST, R_NEXT, R_PREV };
static JSValue f_rel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
	(void)t;
	N0;
	dom_node *r = NULL;
	switch (magic) {
	case R_PARENT: dom_node_get_parent_node(n, &r); break;
	case R_FIRST: dom_node_get_first_child(n, &r); break;
	case R_LAST: dom_node_get_last_child(n, &r); break;
	case R_NEXT: dom_node_get_next_sibling(n, &r); break;
	default: dom_node_get_previous_sibling(n, &r); break;
	}
	JSValue v = wrap(ctx, r);
	if (r)
		dom_node_unref(r);
	return v;
}

static JSValue f_getA(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	if (argc < 2)
		return JS_NULL;
	dom_string *k = dstr(ctx, argv[1]), *v = NULL;
	if (!k)
		return JS_NULL;
	dom_element_get_attribute((dom_element *)n, k, &v);
	dom_string_unref(k);
	return jstr(ctx, v);
}

static JSValue f_setA(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	if (argc < 3)
		return JS_UNDEFINED;
	dom_string *k = dstr(ctx, argv[1]), *v = dstr(ctx, argv[2]);
	// libdom solo recalcula las clases (las que mira el CSS) al crear el atributo class, no al
	// cambiarlo: se quita y se vuelve a poner
	if (k && v && dom_string_byte_length(k) == 5 && !memcmp(dom_string_data(k), "class", 5))
		dom_element_remove_attribute((dom_element *)n, k);
	if (k && v)
		dom_element_set_attribute((dom_element *)n, k, v);
	if (k)
		dom_string_unref(k);
	if (v)
		dom_string_unref(v);
	return JS_UNDEFINED;
}

static JSValue f_remA(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	if (argc < 2)
		return JS_UNDEFINED;
	dom_string *k = dstr(ctx, argv[1]);
	if (k) {
		dom_element_remove_attribute((dom_element *)n, k);
		dom_string_unref(k);
	}
	return JS_UNDEFINED;
}

static JSValue f_attrs(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	JSValue arr = JS_NewArray(ctx);
	dom_namednodemap *m = NULL;
	if (dom_node_get_attributes(n, &m) != DOM_NO_ERR || !m)
		return arr;
	uint32_t len = 0;
	dom_namednodemap_get_length(m, &len);
	for (uint32_t i = 0; i < len; i++) {
		dom_node *a = NULL;
		dom_namednodemap_item(m, i, &a);
		if (!a)
			continue;
		dom_string *nm = NULL, *val = NULL;
		dom_attr_get_name((dom_attr *)a, &nm);
		dom_attr_get_value((dom_attr *)a, &val);
		JSValue pair = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, pair, 0, jstr(ctx, nm));
		JS_SetPropertyUint32(ctx, pair, 1, val ? jstr(ctx, val) : JS_NewString(ctx, ""));
		JS_SetPropertyUint32(ctx, arr, i, pair);
		dom_node_unref(a);
	}
	dom_namednodemap_unref(m);
	return arr;
}

static JSValue f_text(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_string *s = NULL;
	dom_node_get_text_content(n, &s);
	return s ? jstr(ctx, s) : JS_NewString(ctx, "");
}

static JSValue f_setText(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	if (argc < 2)
		return JS_UNDEFINED;
	dom_string *s = dstr(ctx, argv[1]);
	if (s) {
		dom_node_set_text_content(n, s);
		dom_string_unref(s);
	}
	return JS_UNDEFINED;
}

static JSValue f_data(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_string *s = NULL;
	dom_characterdata_get_data((dom_characterdata *)n, &s);
	return s ? jstr(ctx, s) : JS_NewString(ctx, "");
}

static JSValue f_setData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	if (argc < 2)
		return JS_UNDEFINED;
	dom_string *s = dstr(ctx, argv[1]);
	if (s) {
		dom_characterdata_set_data((dom_characterdata *)n, s);
		dom_string_unref(s);
	}
	return JS_UNDEFINED;
}

static JSValue f_create(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	if (argc < 1)
		return JS_NULL;
	dom_string *s = dstr(ctx, argv[0]);
	dom_element *e = NULL;
	if (s) {
		dom_document_create_element(J.doc, s, &e);
		dom_string_unref(s);
	}
	JSValue v = wrap(ctx, (dom_node *)e);
	if (e)
		dom_node_unref(e);
	return v;
}

static JSValue f_createText(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	dom_string *s = argc > 0 ? dstr(ctx, argv[0]) : NULL;
	dom_text *x = NULL;
	if (s) {
		dom_document_create_text_node(J.doc, s, &x);
		dom_string_unref(s);
	}
	JSValue v = wrap(ctx, (dom_node *)x);
	if (x)
		dom_node_unref(x);
	return v;
}

static JSValue f_createFrag(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t, (void)argc, (void)argv;
	dom_document_fragment *f = NULL;
	dom_document_create_document_fragment(J.doc, &f);
	JSValue v = wrap(ctx, (dom_node *)f);
	if (f)
		dom_node_unref(f);
	return v;
}

static JSValue f_append(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_node *c = argc > 1 ? unwrap(argv[1]) : NULL, *r = NULL;
	dom_exception e = c ? dom_node_append_child(n, c, &r) : DOM_NO_ERR;
	if (r)
		dom_node_unref(r);
	(void)ctx;
	return JS_NewInt32(ctx, e);
}

static JSValue f_insert(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_node *c = argc > 1 ? unwrap(argv[1]) : NULL, *ref = argc > 2 ? unwrap(argv[2]) : NULL, *r = NULL;
	if (c && dom_node_insert_before(n, c, ref, &r) == DOM_NO_ERR && r)
		dom_node_unref(r);
	(void)ctx;
	return JS_UNDEFINED;
}

static JSValue f_removeChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_node *c = argc > 1 ? unwrap(argv[1]) : NULL, *r = NULL;
	if (c && dom_node_remove_child(n, c, &r) == DOM_NO_ERR && r)
		dom_node_unref(r);
	(void)ctx;
	return JS_UNDEFINED;
}

static JSValue f_clone(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	N0;
	dom_node *r = NULL;
	dom_node_clone_node(n, argc > 1 && JS_ToBool(ctx, argv[1]), &r);
	JSValue v = wrap(ctx, r);
	if (r)
		dom_node_unref(r);
	return v;
}

// un nodo de otro documento, copiado a este (import_node de libdom no cambia su documento)
static dom_node *copy_in(dom_node *src, int depth)
{
	dom_node_type ty;
	if (depth > 200 || dom_node_get_node_type(src, &ty) != DOM_NO_ERR)
		return NULL;
	dom_node *out = NULL;
	if (ty == DOM_TEXT_NODE || ty == DOM_CDATA_SECTION_NODE) {
		dom_string *d = NULL;
		dom_characterdata_get_data((dom_characterdata *)src, &d);
		if (d) {
			dom_document_create_text_node(J.doc, d, (dom_text **)&out);
			dom_string_unref(d);
		}
		return out;
	}
	if (ty != DOM_ELEMENT_NODE)
		return NULL;
	dom_string *nm = NULL;
	dom_node_get_node_name(src, &nm);
	if (!nm)
		return NULL;
	dom_document_create_element(J.doc, nm, (dom_element **)&out);
	dom_string_unref(nm);
	if (!out)
		return NULL;
	dom_namednodemap *m = NULL;
	if (dom_node_get_attributes(src, &m) == DOM_NO_ERR && m) {
		uint32_t len = 0;
		dom_namednodemap_get_length(m, &len);
		for (uint32_t i = 0; i < len; i++) {
			dom_node *a = NULL;
			dom_namednodemap_item(m, i, &a);
			if (!a)
				continue;
			dom_string *an = NULL, *av = NULL;
			dom_attr_get_name((dom_attr *)a, &an);
			dom_attr_get_value((dom_attr *)a, &av);
			if (an && av)
				dom_element_set_attribute((dom_element *)out, an, av);
			if (an)
				dom_string_unref(an);
			if (av)
				dom_string_unref(av);
			dom_node_unref(a);
		}
		dom_namednodemap_unref(m);
	}
	dom_node *c = NULL;
	dom_node_get_first_child(src, &c);
	while (c) {
		dom_node *k = copy_in(c, depth + 1), *r = NULL;
		if (k) {
			if (dom_node_append_child(out, k, &r) == DOM_NO_ERR && r)
				dom_node_unref(r);
			dom_node_unref(k);
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
	return out;
}

// HTML suelto (innerHTML, insertAdjacentHTML): se lee aparte y sus nodos pasan a este documento
static JSValue f_parse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	JSValue arr = JS_NewArray(ctx);
	if (argc < 1)
		return arr;
	size_t l;
	const char *s = JS_ToCStringLen(ctx, &l, argv[0]);
	if (!s)
		return arr;
	dom_hubbub_parser_params p = {.enc = "UTF-8", .fix_enc = true, .enable_script = false};
	dom_hubbub_parser *ps = NULL;
	dom_document *d = NULL;
	if (dom_hubbub_parser_create(&p, &ps, &d) == DOM_HUBBUB_OK && ps) {
		static const char pre[] = "<!DOCTYPE html><html><head></head><body>";
		dom_hubbub_parser_parse_chunk(ps, (const uint8_t *)pre, sizeof pre - 1);
		dom_hubbub_parser_parse_chunk(ps, (const uint8_t *)s, l);
		dom_hubbub_parser_completed(ps);
		dom_hubbub_parser_destroy(ps);
		dom_html_element *body = NULL;
		dom_html_document_get_body((dom_html_document *)d, &body);
		uint32_t k = 0;
		if (body) {
			dom_node *c = NULL;
			dom_node_get_first_child((dom_node *)body, &c);
			while (c) {
				dom_node *imp = copy_in(c, 0);
				if (imp) {
					JS_SetPropertyUint32(ctx, arr, k++, wrap(ctx, imp));
					dom_node_unref(imp);
				}
				dom_node *nx = NULL;
				dom_node_get_next_sibling(c, &nx);
				dom_node_unref(c);
				c = nx;
			}
			dom_node_unref(body);
		}
		dom_node_unref(d);
	}
	JS_FreeCString(ctx, s);
	return arr;
}

// fetch / XMLHttpRequest (síncrono aquí: el JavaScript de la página va en su propio hilo)
static JSValue f_fetch(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	if (argc < 1 || !J.host->fetch)
		return JS_NULL;
	const char *url = JS_ToCString(ctx, argv[0]);
	const char *method = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
	size_t bl = 0;
	const char *body = argc > 2 && JS_IsString(argv[2]) ? JS_ToCStringLen(ctx, &bl, argv[2]) : NULL;
	char *out = NULL, fin[1024] = "";
	size_t n = 0;
	long code = 0;
	int r = url ? J.host->fetch(url, method ? method : "GET", body, bl, &out, &n, &code, fin, sizeof fin) : -1;
	JSValue res = JS_NULL;
	if (r == 0) {
		res = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, res, 0, JS_NewInt32(ctx, (int)code));
		JS_SetPropertyUint32(ctx, res, 1, JS_NewStringLen(ctx, out ? out : "", n));
		JS_SetPropertyUint32(ctx, res, 2, JS_NewString(ctx, fin[0] ? fin : url));
	}
	free(out);
	if (url)
		JS_FreeCString(ctx, url);
	if (method)
		JS_FreeCString(ctx, method);
	if (body)
		JS_FreeCString(ctx, body);
	return res;
}

static JSValue f_log(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	if (argc < 1 || !J.host->log)
		return JS_UNDEFINED;
	const char *s = JS_ToCString(ctx, argv[0]);
	if (s) {
		J.host->log(s);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

static JSValue f_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t, (void)argc, (void)argv;
	return JS_NewFloat64(ctx, (double)(now_ms() - J.t0));
}

static JSValue f_navigate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
	(void)t;
	if (argc < 1)
		return JS_UNDEFINED;
	const char *s = JS_ToCString(ctx, argv[0]);
	if (s) {
		snprintf(J.res->navigate, sizeof J.res->navigate, "%s", s);
		JS_FreeCString(ctx, s);
	}
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry NATIVE[] = {
    JS_CFUNC_DEF("doc", 0, f_doc),
    JS_CFUNC_DEF("type", 1, f_type),
    JS_CFUNC_DEF("name", 1, f_name),
    JS_CFUNC_DEF("id", 1, f_id),
    JS_CFUNC_MAGIC_DEF("parent", 1, f_rel, R_PARENT),
    JS_CFUNC_MAGIC_DEF("first", 1, f_rel, R_FIRST),
    JS_CFUNC_MAGIC_DEF("last", 1, f_rel, R_LAST),
    JS_CFUNC_MAGIC_DEF("next", 1, f_rel, R_NEXT),
    JS_CFUNC_MAGIC_DEF("prev", 1, f_rel, R_PREV),
    JS_CFUNC_DEF("getA", 2, f_getA),
    JS_CFUNC_DEF("setA", 3, f_setA),
    JS_CFUNC_DEF("remA", 2, f_remA),
    JS_CFUNC_DEF("attrs", 1, f_attrs),
    JS_CFUNC_DEF("text", 1, f_text),
    JS_CFUNC_DEF("setText", 2, f_setText),
    JS_CFUNC_DEF("data", 1, f_data),
    JS_CFUNC_DEF("setData", 2, f_setData),
    JS_CFUNC_DEF("create", 1, f_create),
    JS_CFUNC_DEF("createText", 1, f_createText),
    JS_CFUNC_DEF("createFrag", 0, f_createFrag),
    JS_CFUNC_DEF("append", 2, f_append),
    JS_CFUNC_DEF("insert", 3, f_insert),
    JS_CFUNC_DEF("removeChild", 2, f_removeChild),
    JS_CFUNC_DEF("clone", 2, f_clone),
    JS_CFUNC_DEF("parse", 1, f_parse),
    JS_CFUNC_DEF("fetch", 3, f_fetch),
    JS_CFUNC_DEF("log", 1, f_log),
    JS_CFUNC_DEF("now", 0, f_now),
    JS_CFUNC_DEF("navigate", 1, f_navigate),
};

/* ------------------------------------------------------------ ejecutar */
static int interrupt(JSContext *rt, void *u)
{
	(void)rt, (void)u;
	return now_ms() - J.t0 > J.host->budget_ms; // se acabó el tiempo de la página
}

static void report(JSContext *ctx, const char *where)
{
	JSValue e = JS_GetException(ctx);
	const char *s = JS_ToCString(ctx, e);
	char msg[300];
	snprintf(msg, sizeof msg, "js: error en %s: %s", where, s ? s : "?");
	if (J.host->log)
		J.host->log(msg);
	if (s)
		JS_FreeCString(ctx, s);
	JS_FreeValue(ctx, e);
}

// QuickJS solo acepta UTF-8 válido; los bytes sueltos (archivos en Latin-1) se pasan a UTF-8
static char *fix_utf8(const char *s, size_t n, size_t *out_n)
{
	size_t i = 0;
	while (i < n) {
		unsigned char c = s[i];
		int k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (k < 0 || i + k >= n + (k ? 0 : 1))
			break;
		bool ok = true;
		for (int j = 1; j <= k; j++)
			ok &= ((unsigned char)s[i + j] >> 6) == 2;
		if (!ok)
			break;
		i += k + 1;
	}
	if (i >= n)
		return NULL; // ya es válido
	char *o = malloc(n * 2 + 1);
	if (!o)
		return NULL;
	size_t w = 0;
	for (i = 0; i < n;) {
		unsigned char c = s[i];
		int k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		bool ok = k >= 0 && i + k < n;
		for (int j = 1; ok && j <= k; j++)
			ok = ((unsigned char)s[i + j] >> 6) == 2;
		if (ok) {
			memcpy(o + w, s + i, k + 1);
			w += k + 1, i += k + 1;
		} else { // Latin-1 -> UTF-8
			o[w++] = (char)(0xC0 | (c >> 6));
			o[w++] = (char)(0x80 | (c & 0x3F));
			i++;
		}
	}
	o[w] = 0;
	*out_n = w;
	return o;
}

static bool eval(JSContext *ctx, const char *code, size_t n, const char *name)
{
	size_t fn = 0;
	char *fixed = fix_utf8(code, n, &fn);
	if (fixed)
		code = fixed, n = fn;
	JSValue v = JS_Eval(ctx, code, n, name, JS_EVAL_TYPE_GLOBAL);
	free(fixed);
	bool ok = !JS_IsException(v);
	if (!ok)
		report(ctx, name);
	JS_FreeValue(ctx, v);
	return ok;
}

static void pending_jobs(JSRuntime *rt)
{
	JSContext *c;
	for (int i = 0; i < 10000 && JS_ExecutePendingJob(rt, &c) > 0; i++)
		;
}

static bool call(JSContext *ctx, const char *fn, JSValueConst arg)
{
	JSValue g = JS_GetGlobalObject(ctx), f = JS_GetPropertyStr(ctx, g, fn);
	bool ok = true;
	if (JS_IsFunction(ctx, f)) {
		JSValue r = JS_Call(ctx, f, g, 1, &arg);
		if (JS_IsException(r)) {
			report(ctx, fn);
			ok = false;
		} else
			ok = JS_ToBool(ctx, r);
		JS_FreeValue(ctx, r);
	}
	JS_FreeValue(ctx, f);
	JS_FreeValue(ctx, g);
	return ok;
}

static char *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long l = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = l > 0 ? malloc(l + 1) : NULL;
	if (b) {
		*n = fread(b, 1, l, f);
		b[*n] = 0;
	}
	fclose(f);
	return b;
}

static void tag_lower(dom_node *n, char *tag, size_t sz)
{
	tag[0] = 0;
	dom_string *s = NULL;
	if (dom_node_get_node_name(n, &s) != DOM_NO_ERR || !s)
		return;
	size_t l = dom_string_byte_length(s), i = 0;
	for (; i < l && i < sz - 1; i++)
		tag[i] = (char)tolower((unsigned char)dom_string_data(s)[i]);
	tag[i] = 0;
	dom_string_unref(s);
}

static bool get_attr(dom_node *n, const char *name, char *out, size_t sz)
{
	dom_string *k = NULL, *v = NULL;
	out[0] = 0;
	if (dom_string_create((const uint8_t *)name, strlen(name), &k) != DOM_NO_ERR)
		return false;
	dom_element_get_attribute((dom_element *)n, k, &v);
	dom_string_unref(k);
	if (!v)
		return false;
	snprintf(out, sz, "%.*s", (int)dom_string_byte_length(v), dom_string_data(v));
	dom_string_unref(v);
	return true;
}

/* los <script> en el orden del documento (los que se añadan desde JavaScript no se ejecutan aún) */
typedef struct {
	dom_node **v;
	int n, cap;
} NodeList;

static void collect(dom_node *n, NodeList *l, int depth)
{
	if (depth > 120)
		return;
	dom_node *c = NULL;
	dom_node_get_first_child(n, &c);
	while (c) {
		dom_node_type ty;
		dom_node_get_node_type(c, &ty);
		if (ty == DOM_ELEMENT_NODE) {
			char tag[12];
			tag_lower(c, tag, sizeof tag);
			if (!strcmp(tag, "script")) {
				if (l->n == l->cap) {
					int nc = l->cap ? l->cap * 2 : 32;
					dom_node **nv = realloc(l->v, nc * sizeof *nv);
					if (nv)
						l->v = nv, l->cap = nc;
				}
				if (l->n < l->cap)
					l->v[l->n++] = dom_node_ref(c);
			} else
				collect(c, l, depth + 1);
		}
		dom_node *nx = NULL;
		dom_node_get_next_sibling(c, &nx);
		dom_node_unref(c);
		c = nx;
	}
}

static bool js_type_ok(dom_node *s)
{
	char ty[64];
	if (!get_attr(s, "type", ty, sizeof ty) || !ty[0])
		return true;
	for (char *p = ty; *p; p++)
		*p = (char)tolower((unsigned char)*p);
	return !strcmp(ty, "text/javascript") || !strcmp(ty, "application/javascript") || !strcmp(ty, "application/x-javascript") ||
	       !strcmp(ty, "text/ecmascript");
}

static void resolve(const char *base, const char *href, char *out, size_t n)
{
	if (strstr(href, "://") == href + strcspn(href, ":/") && strstr(href, "://")) {
		snprintf(out, n, "%s", href);
		return;
	}
	const char *sch = strstr(base, "://");
	if (href[0] == '/' && href[1] == '/') {
		snprintf(out, n, "%.*s:%s", sch ? (int)(sch - base) : 5, sch ? base : "https", href);
		return;
	}
	if (!sch) { // archivo de la SD
		const char *sl = strrchr(base, '/');
		if (href[0] == '/')
			snprintf(out, n, "%s", href);
		else
			snprintf(out, n, "%.*s%s", sl ? (int)(sl - base + 1) : 0, base, href);
		return;
	}
	const char *host = sch + 3;
	size_t hl = strcspn(host, "/?#");
	if (href[0] == '/')
		snprintf(out, n, "%.*s%s", (int)(host + hl - base), base, href);
	else {
		size_t bl = strcspn(base, "?#");
		const char *sl = base + bl;
		while (sl > host + hl && sl[-1] != '/')
			sl--;
		if (sl <= host + hl)
			snprintf(out, n, "%.*s/%s", (int)(host + hl - base), base, href);
		else
			snprintf(out, n, "%.*s%s", (int)(sl - base), base, href);
	}
}

void jsdom_run(dom_document *doc, const JsHost *host, JsResult *res)
{
	memset(res, 0, sizeof *res);
	J.doc = doc, J.host = host, J.res = res, J.t0 = now_ms();
	size_t pn = 0;
	char *prelude = read_file(DOM_JS, &pn);
	if (!prelude) {
		if (host->log)
			host->log("js: falta romfs:/js/dom.js");
		return;
	}
	JSRuntime *rt = JS_NewRuntime();
	if (!rt) {
		free(prelude);
		return;
	}
	JS_SetMemoryLimit(rt, JS_MEM);
	JS_SetMaxStackSize(rt, JS_STACK);
	JS_SetInterruptHandler(rt, interrupt, NULL);
	J.cls = 0;
	JS_NewClassID(rt, &J.cls);
	JS_NewClass(rt, J.cls, &NODE_CLASS);
	JSContext *ctx = JS_NewContext(rt);
	if (!ctx) {
		JS_FreeRuntime(rt);
		free(prelude);
		return;
	}
	JSValue g = JS_GetGlobalObject(ctx), nat = JS_NewObject(ctx);
	JS_SetPropertyFunctionList(ctx, nat, NATIVE, sizeof NATIVE / sizeof NATIVE[0]);
	JS_SetPropertyStr(ctx, nat, "url", JS_NewString(ctx, host->url));
	JS_SetPropertyStr(ctx, nat, "ua", JS_NewString(ctx, host->ua));
	JS_SetPropertyStr(ctx, nat, "width", JS_NewFloat64(ctx, host->width));
	JS_SetPropertyStr(ctx, g, "__n", nat);
	JS_FreeValue(ctx, g);
	bool ok = eval(ctx, prelude, pn, "dom.js");
	free(prelude);
	{
		char m[80];
		snprintf(m, sizeof m, "js: dom.js listo en %u ms", (unsigned)(now_ms() - J.t0));
		if (host->log)
			host->log(m);
	}
	if (ok) {
		NodeList l = {0};
		collect((dom_node *)doc, &l, 0);
		size_t total = 0;
		for (int i = 0; i < l.n && now_ms() - J.t0 < host->budget_ms; i++) {
			dom_node *s = l.v[i];
			if (!js_type_ok(s))
				continue;
			char src[1024], name[1100];
			char *code = NULL;
			size_t cn = 0;
			if (get_attr(s, "src", src, sizeof src) && src[0]) {
				char abs[1024], fin[1024];
				resolve(host->url, src, abs, sizeof abs);
				long code_http = 0;
				if (host->fetch(abs, "GET", NULL, 0, &code, &cn, &code_http, fin, sizeof fin) != 0 || code_http >= 400) {
					free(code);
					code = NULL;
				} else if (code) { // QuickJS necesita el código terminado en 0
					char *t = realloc(code, cn + 1);
					if (t)
						code = t, code[cn] = 0;
					else
						free(code), code = NULL;
				}
				snprintf(name, sizeof name, "%s", abs);
			} else {
				dom_string *tx = NULL;
				dom_node_get_text_content(s, &tx);
				if (tx) {
					cn = dom_string_byte_length(tx);
					code = malloc(cn + 1);
					if (code) {
						memcpy(code, dom_string_data(tx), cn);
						code[cn] = 0;
					}
					dom_string_unref(tx);
				}
				snprintf(name, sizeof name, "script %d", i + 1);
			}
			if (!code || !cn || total + cn > SCRIPT_MAX) {
				free(code);
				continue;
			}
			total += cn;
			JSValue sv = wrap(ctx, s);
			call(ctx, "__current", sv);
			JS_FreeValue(ctx, sv);
			res->scripts++;
			u64 ts = now_ms();
			if (!eval(ctx, code, cn, name))
				res->errors++;
			{
				char m[160];
				snprintf(m, sizeof m, "js: %.100s: %u ms", name, (unsigned)(now_ms() - ts));
				if (host->log)
					host->log(m);
			}
			free(code);
			pending_jobs(rt);
		}
		for (int i = 0; i < l.n; i++)
			dom_node_unref(l.v[i]);
		free(l.v);
		// la página está «cargada»: sus eventos y luego los temporizadores (un rato, no para siempre)
		call(ctx, "__current", JS_NULL);
		call(ctx, "__loaded", JS_UNDEFINED);
		pending_jobs(rt);
		for (int round = 0; round < 50 && now_ms() - J.t0 < host->budget_ms; round++) {
			bool more = call(ctx, "__timers", JS_NewInt32(ctx, 3000));
			pending_jobs(rt);
			if (!more)
				break;
		}
	}
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	res->ms = (unsigned)(now_ms() - J.t0);
}
