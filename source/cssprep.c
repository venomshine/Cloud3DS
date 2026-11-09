/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "cssprep.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------ texto de salida */
typedef struct {
	char *p;
	size_t n, cap;
	bool bad;
} Out;

static void put(Out *o, const char *s, size_t n)
{
	if (!o || o->bad || !n)
		return;
	if (o->n + n + 1 > o->cap) {
		size_t c = o->cap ? o->cap : 4096;
		while (c < o->n + n + 1)
			c *= 2;
		char *np = realloc(o->p, c);
		if (!np) {
			o->bad = true;
			return;
		}
		o->p = np, o->cap = c;
	}
	memcpy(o->p + o->n, s, n);
	o->n += n;
	o->p[o->n] = 0;
}
static void puts_(Out *o, const char *s) { put(o, s, strlen(s)); }

/* ------------------------------------------------------------ variables: nombre -> valor */
typedef struct {
	char **k, **v;
	size_t cap, n;
} Map;

static uint32_t hash(const char *s, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h;
}

static void map_free(Map *m)
{
	for (size_t i = 0; i < m->cap; i++)
		free(m->k[i]), free(m->v[i]);
	free(m->k), free(m->v);
	memset(m, 0, sizeof *m);
}

static const char *map_get(const Map *m, const char *k, size_t kn)
{
	if (!m->cap)
		return NULL;
	for (size_t i = hash(k, kn) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->k[i])
			return NULL;
		if (strlen(m->k[i]) == kn && !memcmp(m->k[i], k, kn))
			return m->v[i];
	}
}

static void map_set(Map *m, const char *k, size_t kn, const char *v, size_t vn)
{
	if ((m->n + 1) * 2 > m->cap) { // crecer (y recolocar)
		size_t nc = m->cap ? m->cap * 2 : 256;
		char **nk = calloc(nc, sizeof *nk), **nv = calloc(nc, sizeof *nv);
		if (!nk || !nv) {
			free(nk), free(nv);
			return;
		}
		for (size_t i = 0; i < m->cap; i++)
			if (m->k[i]) {
				size_t j = hash(m->k[i], strlen(m->k[i])) & (nc - 1);
				while (nk[j])
					j = (j + 1) & (nc - 1);
				nk[j] = m->k[i], nv[j] = m->v[i];
			}
		free(m->k), free(m->v);
		m->k = nk, m->v = nv, m->cap = nc;
	}
	size_t i = hash(k, kn) & (m->cap - 1);
	for (; m->k[i]; i = (i + 1) & (m->cap - 1))
		if (strlen(m->k[i]) == kn && !memcmp(m->k[i], k, kn))
			break;
	char *nv = malloc(vn + 1);
	if (!nv)
		return;
	memcpy(nv, v, vn);
	nv[vn] = 0;
	if (!m->k[i]) {
		m->k[i] = malloc(kn + 1);
		if (!m->k[i]) {
			free(nv);
			return;
		}
		memcpy(m->k[i], k, kn);
		m->k[i][kn] = 0;
		m->n++;
	} else
		free(m->v[i]);
	m->v[i] = nv;
}

struct CssPrep {
	float w;
	Map root, any;     /* las de :root/html/body (las de la página) y todas */
	Map areas;         /* rejilla: nombre de zona -> «orden,toda-la-fila,columna,cuántas,fila,cuántas» */
	char *html, *body; /* sus atributos, «nombre=valor\n» */
};

static float G_W = 1024; /* el ancho de la página (para vw) */
static bool G_KEEP_CALC; /* width: calc(… % …) lo resuelve libcss (sabe el ancho de verdad) */

CssPrep *cssprep_new(float width, const char *html_attrs, const char *body_attrs)
{
	CssPrep *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	p->w = width;
	G_W = width;
	p->html = strdup(html_attrs ? html_attrs : "");
	p->body = strdup(body_attrs ? body_attrs : "");
	return p;
}

void cssprep_free(CssPrep *p)
{
	if (!p)
		return;
	map_free(&p->root);
	map_free(&p->any);
	map_free(&p->areas);
	free(p->html), free(p->body);
	free(p);
}

static void util_copy(char *d, const char *s, size_t n)
{
	snprintf(d, n, "%s", s);
}

/* ------------------------------------------------------------ recorrer el texto */
static const char *skip_ws(const char *s, const char *e)
{
	for (;;) {
		while (s < e && isspace((unsigned char)*s))
			s++;
		if (s + 1 < e && s[0] == '/' && s[1] == '*') {
			const char *c = s + 2;
			while (c + 1 < e && !(c[0] == '*' && c[1] == '/'))
				c++;
			s = c + 1 < e ? c + 2 : e;
			continue;
		}
		return s;
	}
}

static const char *skip_str(const char *s, const char *e) // s en la comilla
{
	char q = *s++;
	while (s < e && *s != q) {
		if (*s == '\\' && s + 1 < e)
			s++;
		s++;
	}
	return s < e ? s + 1 : e;
}

/* el primer carácter de set fuera de paréntesis, corchetes, comillas y comentarios (o e) */
static const char *find_top(const char *s, const char *e, const char *set)
{
	int d = 0;
	while (s < e) {
		char c = *s;
		if (c == '"' || c == '\'') {
			s = skip_str(s, e);
			continue;
		}
		if (c == '/' && s + 1 < e && s[1] == '*') {
			s = skip_ws(s, e);
			continue;
		}
		if (c == '(' || c == '[')
			d++;
		else if ((c == ')' || c == ']') && d > 0)
			d--;
		else if (!d && strchr(set, c))
			return s;
		s++;
	}
	return e;
}

/* s justo después de «{»: el «}» que lo cierra (o e) */
static const char *block_end(const char *s, const char *e)
{
	int d = 1;
	while (s < e) {
		char c = *s;
		if (c == '"' || c == '\'') {
			s = skip_str(s, e);
			continue;
		}
		if (c == '/' && s + 1 < e && s[1] == '*') {
			s = skip_ws(s, e);
			continue;
		}
		if (c == '{')
			d++;
		else if (c == '}' && --d == 0)
			return s;
		s++;
	}
	return e;
}

static void trim(const char **s, const char **e)
{
	while (*s < *e && isspace((unsigned char)**s))
		(*s)++;
	while (*e > *s && isspace((unsigned char)(*e)[-1]))
		(*e)--;
}

static bool has_ci(const char *s, size_t n, const char *needle)
{
	size_t l = strlen(needle);
	for (size_t i = 0; i + l <= n; i++)
		if (!strncasecmp(s + i, needle, l))
			return true;
	return false;
}

/* ------------------------------------------------------------ @media */
static bool G_DARK; /* modo oscuro: las webs con tema oscuro lo usan */

void cssprep_set_dark(bool dark)
{
	G_DARK = dark;
}

typedef struct {
	float px, pc;
	bool num; /* un número sin unidad */
} CV;
static bool cv_expr(const char **q, const char *e, CV *r);

static float len_px(const char *v)
{
	if (!strncasecmp(v, "calc(", 5)) { // @media (width >= calc(…))
		const char *q = v, *e = v + strlen(v);
		CV r;
		if (cv_expr(&q, e, &r) && r.pc == 0)
			return r.px;
		return -1;
	}
	char *end;
	float x = strtof(v, &end);
	if (end == v)
		return -1;
	if (!strncasecmp(end, "em", 2) || !strncasecmp(end, "rem", 3))
		return x * 16;
	return x; // px o sin unidad
}

// una condición entre paréntesis («min-width: 768px», «width >= 768px», «prefers-color-scheme: dark»)
static bool feature_ok(const char *f, float w)
{
	char b[160];
	size_t j = 0;
	for (; *f && j + 1 < sizeof b; f++)
		if (!isspace((unsigned char)*f))
			b[j++] = (char)tolower((unsigned char)*f);
	b[j] = 0;
	const float H = 768;
	char *c = strchr(b, ':');
	if (c) {
		*c = 0;
		const char *k = b, *v = c + 1;
		if (!strcmp(k, "min-width"))
			return w >= len_px(v);
		if (!strcmp(k, "max-width"))
			return w <= len_px(v);
		if (!strcmp(k, "min-height"))
			return H >= len_px(v);
		if (!strcmp(k, "max-height"))
			return H <= len_px(v);
		if (!strcmp(k, "prefers-color-scheme"))
			return !strcmp(v, G_DARK ? "dark" : "light");
		if (!strcmp(k, "prefers-reduced-motion"))
			return !strcmp(v, "reduce"); // la 3DS va mejor sin animaciones (no las hay, pero sí sus estilos)
		if (!strcmp(k, "hover") || !strcmp(k, "any-hover"))
			return !strcmp(v, "hover");
		if (!strcmp(k, "pointer") || !strcmp(k, "any-pointer"))
			return !strcmp(v, "fine");
		if (!strcmp(k, "orientation"))
			return !strcmp(v, "landscape");
		if (!strcmp(k, "forced-colors") || !strcmp(k, "inverted-colors"))
			return !strcmp(v, "none");
		if (!strcmp(k, "prefers-contrast") || !strcmp(k, "prefers-reduced-transparency") || !strcmp(k, "prefers-reduced-data"))
			return !strcmp(v, "no-preference");
		if (strstr(k, "device-pixel-ratio") || strstr(k, "resolution"))
			return strstr(k, "max") != NULL;
		if (!strcmp(k, "display-mode"))
			return !strcmp(v, "browser");
		if (!strcmp(k, "scripting"))
			return !strcmp(v, "enabled");
		if (!strcmp(k, "color") || !strcmp(k, "min-color"))
			return true;
		return false;
	}
	if (!strcmp(b, "color") || !strcmp(b, "hover") || !strcmp(b, "pointer"))
		return true;
	// rango: «width>=768px», «768px<=width», «400px<width<=900px»
	const char *dim = strstr(b, "width") ? "width" : strstr(b, "height") ? "height" : NULL;
	if (!dim)
		return false;
	float val = !strcmp(dim, "width") ? w : H;
	char *p = strstr(b, dim);
	bool ok = true;
	// lo que va antes: «X<» o «X<=» (o «>»)
	if (p > b) {
		char *q = p;
		bool eq = q[-1] == '=';
		if (eq)
			q--;
		char op = q > b ? q[-1] : 0;
		*(q - (q > b)) = 0;
		float x = len_px(b);
		if (op == '<')
			ok &= eq ? x <= val : x < val;
		else if (op == '>')
			ok &= eq ? x >= val : x > val;
	}
	char *r = p + strlen(dim);
	if (*r) {
		char op = *r++;
		bool eq = *r == '=';
		if (eq)
			r++;
		float x = len_px(r);
		if (op == '<')
			ok &= eq ? val <= x : val < x;
		else if (op == '>')
			ok &= eq ? val >= x : val > x;
		else if (op == '=')
			ok &= val == x;
	}
	return ok;
}

static bool query_ok(const char *s, const char *e, float w)
{
	trim(&s, &e);
	bool neg = false;
	if (e - s > 4 && !strncasecmp(s, "not ", 4))
		neg = true, s += 4;
	else if (e - s > 5 && !strncasecmp(s, "only ", 5))
		s += 5;
	bool ok = true;
	while (s < e && ok) {
		s = skip_ws(s, e);
		if (s >= e)
			break;
		if (*s == '(') {
			const char *c = find_top(s + 1, e, ")"); // el «)» de este paréntesis
			int d = 0;
			for (c = s; c < e; c++) {
				if (*c == '(')
					d++;
				else if (*c == ')' && --d == 0)
					break;
			}
			char f[200];
			size_t l = (size_t)(c - s - 1) < sizeof f - 1 ? (size_t)(c - s - 1) : sizeof f - 1;
			memcpy(f, s + 1, l);
			f[l] = 0;
			if (f[0] == '(') // anidado: ((a) or (b)) → se mira dentro
				ok = query_ok(f, f + l, w) || has_ci(f, l, " or ");
			else
				ok = feature_ok(f, w);
			s = c < e ? c + 1 : e;
			continue;
		}
		const char *t = s;
		while (s < e && !isspace((unsigned char)*s) && *s != '(')
			s++;
		size_t tl = (size_t)(s - t);
		if ((tl == 3 && !strncasecmp(t, "and", 3)) || (tl == 3 && !strncasecmp(t, "all", 3)) || (tl == 6 && !strncasecmp(t, "screen", 6)))
			continue;
		if (tl == 2 && !strncasecmp(t, "or", 2))
			continue;
		ok = false; // print, speech, tv… o algo que no se entiende
	}
	return neg ? !ok : ok;
}

bool cssprep_media_ok(const char *q, float w)
{
	if (!q)
		return true;
	const char *s = q, *e = q + strlen(q);
	if (skip_ws(s, e) >= e)
		return true;
	while (s < e) {
		const char *c = find_top(s, e, ",");
		if (query_ok(s, c, w))
			return true;
		s = c + 1;
	}
	return false;
}

/* ------------------------------------------------------------ ¿es una regla de la raíz? */
static bool attr_val(const char *attrs, const char *name, size_t nl, char *out, size_t on)
{
	for (const char *p = attrs; *p;) {
		const char *eq = strchr(p, '='), *nlp = strchr(p, '\n');
		if (!nlp)
			nlp = p + strlen(p);
		if (eq && eq < nlp && (size_t)(eq - p) == nl && !strncasecmp(p, name, nl)) {
			size_t l = (size_t)(nlp - eq - 1) < on - 1 ? (size_t)(nlp - eq - 1) : on - 1;
			memcpy(out, eq + 1, l);
			out[l] = 0;
			return true;
		}
		p = *nlp ? nlp + 1 : nlp;
	}
	return false;
}

static bool word_in(const char *list, const char *w, size_t wl)
{
	for (const char *p = list; *p;) {
		while (*p && isspace((unsigned char)*p))
			p++;
		const char *q = p;
		while (*q && !isspace((unsigned char)*q))
			q++;
		if ((size_t)(q - p) == wl && !strncmp(p, w, wl))
			return true;
		p = q;
	}
	return false;
}

// un selector simple compuesto («html[data-x=y].clase»), contra <html> (el) o <body>
static bool compound_ok(const char *s, const char *e, const char *attrs, bool is_html)
{
	char v[512];
	while (s < e) {
		char c = *s;
		if (c == '*') {
			s++;
		} else if (isalpha((unsigned char)c)) {
			const char *t = s;
			while (s < e && (isalnum((unsigned char)*s) || *s == '-'))
				s++;
			size_t l = (size_t)(s - t);
			if (!(l == 4 && !strncasecmp(t, "html", 4) && is_html) && !(l == 4 && !strncasecmp(t, "body", 4) && !is_html))
				return false;
		} else if (c == '.' || c == '#') {
			const char *t = ++s;
			while (s < e && (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '\\'))
				s++;
			if (!attr_val(attrs, c == '.' ? "class" : "id", c == '.' ? 5 : 2, v, sizeof v))
				return false;
			if (c == '.' ? !word_in(v, t, (size_t)(s - t)) : (strlen(v) != (size_t)(s - t) || strncmp(v, t, (size_t)(s - t))))
				return false;
		} else if (c == '[') {
			const char *end = find_top(s + 1, e, "]");
			const char *n0 = s + 1, *n1 = n0;
			while (n1 < end && (isalnum((unsigned char)*n1) || *n1 == '-' || *n1 == '_' || *n1 == ':'))
				n1++;
			bool have = attr_val(attrs, n0, (size_t)(n1 - n0), v, sizeof v);
			const char *op = skip_ws(n1, end);
			if (op < end && *op != ']') {
				char o = *op == '=' ? '=' : *op;
				const char *q = op + (o == '=' ? 1 : 2);
				q = skip_ws(q, end);
				char want[256];
				size_t wl = 0;
				if (q < end && (*q == '"' || *q == '\'')) {
					char qq = *q++;
					while (q < end && *q != qq && wl + 1 < sizeof want)
						want[wl++] = *q++;
				} else
					while (q < end && !isspace((unsigned char)*q) && wl + 1 < sizeof want)
						want[wl++] = *q++;
				want[wl] = 0;
				if (!have)
					return false;
				size_t vl = strlen(v);
				bool ok = o == '=' ? !strcmp(v, want) : o == '~' ? word_in(v, want, wl) : o == '^' ? !strncmp(v, want, wl)
				        : o == '$' ? (vl >= wl && !strcmp(v + vl - wl, want)) : o == '*' ? strstr(v, want) != NULL
				        : o == '|' ? (!strcmp(v, want) || (!strncmp(v, want, wl) && v[wl] == '-')) : false;
				if (!ok)
					return false;
			} else if (!have)
				return false;
			s = end < e ? end + 1 : e;
		} else if (c == ':') {
			const char *t = s;
			s++;
			if (s < e && *s == ':')
				return false; // un pseudoelemento no define variables de la raíz
			while (s < e && (isalnum((unsigned char)*s) || *s == '-'))
				s++;
			size_t l = (size_t)(s - t);
			if (s < e && *s == '(') { // :not(…), :is(…)…: no se mira dentro
				int d = 0;
				for (; s < e; s++) {
					if (*s == '(')
						d++;
					else if (*s == ')' && --d == 0) {
						s++;
						break;
					}
				}
				if (!(l == 4 && !strncasecmp(t, ":not", 4)) && !(l == 3 && !strncasecmp(t, ":is", 3)) && !(l == 6 && !strncasecmp(t, ":where", 6)))
					return false;
				continue;
			}
			if (l == 5 && !strncasecmp(t, ":root", 5)) {
				if (!is_html)
					return false;
			} else if (!(l == 5 && !strncasecmp(t, ":host", 5)))
				return false;
		} else
			return false;
	}
	return true;
}

static bool sel_is_root(CssPrep *p, const char *s, const char *e)
{
	while (s < e) {
		const char *c = find_top(s, e, ",");
		const char *a = s, *b = c;
		trim(&a, &b);
		// lo último tras un combinador (« », >, +, ~)
		const char *last = a;
		int d = 0;
		for (const char *q = a; q < b; q++) {
			if (*q == '(' || *q == '[')
				d++;
			else if ((*q == ')' || *q == ']') && d)
				d--;
			else if (!d && (isspace((unsigned char)*q) || *q == '>' || *q == '+' || *q == '~'))
				last = q + 1;
		}
		last = skip_ws(last, b);
		if (last < b && (compound_ok(last, b, p->html, true) || compound_ok(last, b, p->body, false)))
			return true;
		s = c + 1;
	}
	return false;
}

/* ------------------------------------------------------------ var() */
static bool subst(CssPrep *p, const Map *local, const char *v, size_t vn, Out *o, int depth);

static bool lookup(CssPrep *p, const Map *local, const char *k, size_t kn, const char *fb, size_t fbn, Out *o, int depth)
{
	const char *val = local ? map_get(local, k, kn) : NULL;
	if (!val)
		val = map_get(&p->root, k, kn);
	if (!val && fb)
		return subst(p, local, fb, fbn, o, depth + 1);
	if (!val)
		val = map_get(&p->any, k, kn);
	if (!val)
		return false;
	return subst(p, local, val, strlen(val), o, depth + 1);
}

static bool subst(CssPrep *p, const Map *local, const char *v, size_t vn, Out *o, int depth)
{
	if (depth > 10)
		return false;
	const char *s = v, *e = v + vn;
	while (s < e) {
		const char *f = NULL;
		for (const char *q = s; q + 4 <= e; q++)
			if (!strncasecmp(q, "var(", 4) && (q == v || !(isalnum((unsigned char)q[-1]) || q[-1] == '-'))) {
				f = q;
				break;
			}
		if (!f) {
			put(o, s, (size_t)(e - s));
			return true;
		}
		put(o, s, (size_t)(f - s));
		const char *a = f + 4, *cl = a;
		int d = 1;
		for (; cl < e; cl++) {
			if (*cl == '(')
				d++;
			else if (*cl == ')' && --d == 0)
				break;
		}
		if (cl >= e)
			return false;
		const char *comma = find_top(a, cl, ",");
		const char *n0 = a, *n1 = comma;
		trim(&n0, &n1);
		const char *fb = comma < cl ? comma + 1 : NULL, *fe = cl;
		if (fb)
			trim(&fb, &fe);
		if (!lookup(p, local, n0, (size_t)(n1 - n0), fb, fb ? (size_t)(fe - fb) : 0, o, depth))
			return false;
		s = cl + 1;
	}
	return true;
}

/* ------------------------------------------------------------ valores */
// clamp(a, b, c) → b; min(a, b) / max(a, b): el de px más pequeño / grande (o el primero / último)
/* calc(): px, em, rem, vw, vh y % con + - * /; lo que mezcla % y px se queda en el % (los px suelen ser un
 * margen pequeño). Devuelve false si no lo entiende */
static void cv_ws(const char **q, const char *e)
{
	while (*q < e && isspace((unsigned char)**q))
		(*q)++;
}
static bool cv_atom(const char **q, const char *e, CV *r)
{
	cv_ws(q, e);
	if (*q >= e)
		return false;
	if (**q == '(' || !strncasecmp(*q, "calc(", 5)) {
		*q += **q == '(' ? 1 : 5;
		if (!cv_expr(q, e, r))
			return false;
		cv_ws(q, e);
		if (*q >= e || **q != ')')
			return false;
		(*q)++;
		return true;
	}
	char *u;
	float x = strtof(*q, &u);
	if (u == *q)
		return false;
	*q = u;
	r->px = r->pc = 0, r->num = false;
	if (*q < e && **q == '%')
		r->pc = x, (*q)++;
	else if (e - *q >= 3 && !strncasecmp(*q, "rem", 3))
		r->px = x * 16, *q += 3;
	else if (e - *q >= 2 && !strncasecmp(*q, "em", 2))
		r->px = x * 16, *q += 2;
	else if (e - *q >= 2 && !strncasecmp(*q, "px", 2))
		r->px = x, *q += 2;
	else if (e - *q >= 2 && !strncasecmp(*q, "vw", 2))
		r->px = x * G_W / 100, *q += 2;
	else if (e - *q >= 2 && !strncasecmp(*q, "vh", 2))
		r->px = x * 6, *q += 2;
	else if (*q < e && isalpha((unsigned char)**q))
		return false;
	else
		r->px = x, r->num = true;
	return true;
}
static bool cv_term(const char **q, const char *e, CV *r)
{
	if (!cv_atom(q, e, r))
		return false;
	for (;;) {
		cv_ws(q, e);
		if (*q >= e || (**q != '*' && **q != '/'))
			return true;
		char op = *(*q)++;
		CV b;
		if (!cv_atom(q, e, &b))
			return false;
		if (op == '*') {
			if (b.num)
				r->px *= b.px, r->pc *= b.px;
			else if (r->num)
				b.px *= r->px, b.pc *= r->px, *r = b;
			else
				return false;
		} else {
			if (!b.num || b.px == 0)
				return false;
			r->px /= b.px, r->pc /= b.px;
		}
	}
}
static bool cv_expr(const char **q, const char *e, CV *r)
{
	if (!cv_term(q, e, r))
		return false;
	for (;;) {
		cv_ws(q, e);
		if (*q >= e || (**q != '+' && **q != '-'))
			return true;
		char op = *(*q)++;
		CV b;
		if (!cv_term(q, e, &b))
			return false;
		float k = op == '+' ? 1 : -1;
		r->px += k * b.px, r->pc += k * b.pc;
		r->num = r->num && b.num;
	}
}

/* un color sencillo (#hex, rgb()/rgba(), transparent, black, white) a r,g,b,a; false si es otra cosa */
static bool simple_color(const char *q, const char *e, float c[4])
{
	while (q < e && *q == ' ')
		q++;
	while (e > q && e[-1] == ' ')
		e--;
	size_t l = (size_t)(e - q);
	c[3] = 1;
	if (l == 11 && !strncasecmp(q, "transparent", 11)) {
		c[0] = c[1] = c[2] = c[3] = 0;
		return true;
	}
	if (l == 5 && !strncasecmp(q, "black", 5)) {
		c[0] = c[1] = c[2] = 0;
		return true;
	}
	if (l == 5 && !strncasecmp(q, "white", 5)) {
		c[0] = c[1] = c[2] = 255;
		return true;
	}
	if (*q == '#' && (l == 4 || l == 7 || l == 9)) {
		for (size_t i = 1; i < l; i++)
			if (!isxdigit((unsigned char)q[i]))
				return false;
		unsigned x = (unsigned)strtoul(q + 1, NULL, 16);
		if (l == 4)
			c[0] = ((x >> 8) & 15) * 17, c[1] = ((x >> 4) & 15) * 17, c[2] = (x & 15) * 17;
		else if (l == 7)
			c[0] = (x >> 16) & 255, c[1] = (x >> 8) & 255, c[2] = x & 255;
		else
			c[0] = (x >> 24) & 255, c[1] = (x >> 16) & 255, c[2] = (x >> 8) & 255, c[3] = (x & 255) / 255.0f;
		return true;
	}
	if (l > 5 && !strncasecmp(q, "rgb", 3)) {
		const char *t = memchr(q, '(', l);
		if (!t)
			return false;
		t++;
		for (int k = 0; k < 4 && t < e; k++) {
			char *z;
			float v = strtof(t, &z);
			if (z == t)
				break;
			t = z;
			if (*t == '%')
				v = k < 3 ? v * 2.55f : v / 100, t++;
			c[k] = v;
			while (t < e && (*t == ',' || *t == ' ' || *t == '/'))
				t++;
		}
		return true;
	}
	return false;
}

static void funcs(const char *v, size_t vn, Out *o)
{
	const char *s = v, *e = v + vn;
	while (s < e) {
		const char *f = NULL;
		int kind = 0;
		for (const char *q = s; q < e; q++) {
			if (q > v && (isalnum((unsigned char)q[-1]) || q[-1] == '-'))
				continue;
			if (!strncasecmp(q, "clamp(", 6))
				f = q, kind = 1;
			else if (!strncasecmp(q, "min(", 4))
				f = q, kind = 2;
			else if (!strncasecmp(q, "max(", 4))
				f = q, kind = 3;
			else if (!strncasecmp(q, "light-dark(", 11))
				f = q, kind = 4;
			else if (!strncasecmp(q, "color-mix(", 10))
				f = q, kind = 5;
			else if (!strncasecmp(q, "calc(", 5) && !G_KEEP_CALC)
				f = q, kind = 6;
			if (f)
				break;
		}
		if (!f) {
			put(o, s, (size_t)(e - s));
			return;
		}
		put(o, s, (size_t)(f - s));
		const char *a = f + (kind == 1 ? 6 : kind == 4 ? 11 : kind == 5 ? 10 : kind == 6 ? 5 : 4), *cl = a;
		int d = 1;
		for (; cl < e; cl++) {
			if (*cl == '(')
				d++;
			else if (*cl == ')' && --d == 0)
				break;
		}
		if (cl >= e) {
			put(o, f, (size_t)(e - f));
			return;
		}
		if (kind == 6) { // calc(): su valor
			Out t = {0};
			funcs(a, (size_t)(cl - a), &t); // (min(), var… de dentro, primero)
			const char *q = t.p ? t.p : a, *qe = t.p ? t.p + t.n : cl;
			CV r;
			char b[32];
			if (cv_expr(&q, qe, &r) && (cv_ws(&q, qe), q == qe)) {
				if (r.pc != 0)
					snprintf(b, sizeof b, "%.3g%%", r.pc);
				else if (r.num)
					snprintf(b, sizeof b, "%.4g", r.px);
				else
					snprintf(b, sizeof b, "%.4gpx", r.px);
				puts_(o, b);
			} else
				put(o, f, (size_t)(cl + 1 - f));
			free(t.p);
			s = cl + 1;
			continue;
		}
		const char *args[8], *ends[8];
		int na = 0;
		for (const char *q = a; q <= cl && na < 8;) {
			const char *c = find_top(q, cl, ",");
			args[na] = q, ends[na] = c;
			trim(&args[na], &ends[na]);
			na++;
			q = c + 1;
		}
		int pick = 0;
		if (kind == 4) // light-dark(claro, oscuro): el del tema
			pick = G_DARK && na > 1 ? 1 : 0;
		else if (kind == 5 && na >= 3) { // color-mix(in srgb, a 40%, b 60%): mezclados, si son sencillos
			float ca[4], cb[4], pa = -1, pb = -1;
			const char *ae = ends[1], *be = ends[2];
			char *z;
			for (const char *t = ae; t > args[1]; t--) // «… 40%» al final
				if (t[-1] == ' ') {
					float x = strtof(t, &z);
					if (z != t && *z == '%')
						pa = x, ae = t;
					break;
				}
			for (const char *t = be; t > args[2]; t--)
				if (t[-1] == ' ') {
					float x = strtof(t, &z);
					if (z != t && *z == '%')
						pb = x, be = t;
					break;
				}
			Out ta = {0}, tb = {0};
			funcs(args[1], (size_t)(ae - args[1]), &ta); // (puede llevar otro color-mix dentro)
			funcs(args[2], (size_t)(be - args[2]), &tb);
			if (ta.p && tb.p && simple_color(ta.p, ta.p + ta.n, ca) && simple_color(tb.p, tb.p + tb.n, cb)) {
				if (pa < 0 && pb < 0)
					pa = pb = 50;
				else if (pa < 0)
					pa = 100 - pb;
				else if (pb < 0)
					pb = 100 - pa;
				float k = pa + pb > 0 ? pa / (pa + pb) : 0.5f;
				char b2[64];
				snprintf(b2, sizeof b2, "rgba(%d,%d,%d,%.2f)", (int)(ca[0] * k + cb[0] * (1 - k)), (int)(ca[1] * k + cb[1] * (1 - k)),
				         (int)(ca[2] * k + cb[2] * (1 - k)), ca[3] * k + cb[3] * (1 - k));
				puts_(o, b2);
				free(ta.p), free(tb.p);
				s = cl + 1;
				continue;
			}
			free(ta.p), free(tb.p);
		}
		if (kind == 4) {
		} else if (kind == 5 && na >= 2) { // color-mix(in srgb, a 40%, b): el primer color, sin su %
			const char *q = args[1], *z = ends[1];
			while (z > q && z[-1] != ' ' && memchr(z - 1, '%', 1) == NULL)
				z--;
			const char *sp2 = z;
			while (sp2 > q && sp2[-1] != ' ')
				sp2--;
			const char *stop = ends[1];
			if (stop > q && stop[-1] == '%') { // quita el « 40%» del final (no el de un color-mix de dentro)
				while (stop > q && stop[-1] != ' ' && stop[-1] != ')')
					stop--;
				while (stop > q && stop[-1] == ' ')
					stop--;
			}
			(void)sp2;
			funcs(q, (size_t)(stop - q), o);
			s = cl + 1;
			continue;
		} else if (kind == 1)
			pick = na >= 3 ? 1 : 0;
		else {
			float best = 0;
			bool any = false;
			pick = kind == 2 ? 0 : na - 1;
			for (int i = 0; i < na; i++) {
				char tmp[32];
				size_t l = (size_t)(ends[i] - args[i]) < sizeof tmp - 1 ? (size_t)(ends[i] - args[i]) : sizeof tmp - 1;
				memcpy(tmp, args[i], l);
				tmp[l] = 0;
				char *u;
				float x = strtof(tmp, &u);
				if (u != tmp && (!strcmp(u, "px") || !strcmp(u, "rem") || !strcmp(u, "em"))) {
					if (strcmp(u, "px"))
						x *= 16;
					if (!any || (kind == 2 ? x < best : x > best))
						best = x, pick = i, any = true;
				}
			}
		}
		funcs(args[pick], (size_t)(ends[pick] - args[pick]), o); // puede llevar otra dentro
		s = cl + 1;
	}
}

// rgb(1 2 3 / 50%) → rgba(1,2,3,.5): la sintaxis nueva, a la de siempre
static void colors(const char *v, size_t vn, Out *o)
{
	const char *s = v, *e = v + vn;
	while (s < e) {
		const char *f = NULL;
		for (const char *q = s; q + 4 < e; q++)
			if ((!strncasecmp(q, "rgb(", 4) || !strncasecmp(q, "hsl(", 4) || !strncasecmp(q, "rgba(", 5) || !strncasecmp(q, "hsla(", 5)) &&
			    (q == v || !isalnum((unsigned char)q[-1]))) {
				f = q;
				break;
			}
		if (!f) {
			put(o, s, (size_t)(e - s));
			return;
		}
		const char *a = strchr(f, '(') + 1, *cl = memchr(a, ')', (size_t)(e - a));
		if (!cl || memchr(a, ',', (size_t)(cl - a))) { // ya con comas: tal cual
			const char *nx = cl ? cl + 1 : e;
			put(o, s, (size_t)(nx - s));
			s = nx;
			continue;
		}
		put(o, s, (size_t)(f - s));
		char parts[4][24];
		int np = 0;
		for (const char *q = a; q < cl && np < 4;) {
			while (q < cl && (isspace((unsigned char)*q) || *q == '/'))
				q++;
			const char *t = q;
			while (q < cl && !isspace((unsigned char)*q) && *q != '/')
				q++;
			if (q > t) {
				size_t l = (size_t)(q - t) < 23 ? (size_t)(q - t) : 23;
				memcpy(parts[np], t, l);
				parts[np++][l] = 0;
			}
		}
		bool hsl = tolower((unsigned char)f[0]) == 'h';
		if (np == 4 && strchr(parts[3], '%')) // alfa en %
			snprintf(parts[3], sizeof parts[3], "%.3g", strtof(parts[3], NULL) / 100);
		char b[128];
		if (np == 4)
			snprintf(b, sizeof b, "%s(%s,%s,%s,%s)", hsl ? "hsla" : "rgba", parts[0], parts[1], parts[2], parts[3]);
		else if (np == 3)
			snprintf(b, sizeof b, "%s(%s,%s,%s)", hsl ? "hsl" : "rgb", parts[0], parts[1], parts[2]);
		else
			snprintf(b, sizeof b, "%.*s", (int)(cl + 1 - f), f);
		puts_(o, b);
		s = cl + 1;
	}
}

/* ------------------------------------------------------------ declaraciones */
static void decl_out(Out *o, const char *name, const char *val)
{
	puts_(o, name);
	put(o, ":", 1);
	puts_(o, val);
	put(o, ";", 1);
}

// las propiedades lógicas, a las físicas (de izquierda a derecha, horizontal)
static void decl_out(Out *o, const char *name, const char *val);
static void grid_pos(Out *o, int start, int span);
static int grid_areas_n(const char *s, const char *e, char names[24][48], int pos[24][4], bool full[24], int *ncols);

static float px_of(const char *v)
{
	char *u;
	float x = strtof(v, &u);
	if (u == v)
		return -1;
	if (!strncmp(u, "px", 2))
		return x;
	if (!strncmp(u, "rem", 3) || !strncmp(u, "em", 2))
		return x * 16;
	return -1;
}

/* rejilla (libcss no la conoce): se pasa a propiedades de columnas que sí conoce y que el navegador
 * lee en las rejillas: column-count = cuántas columnas, column-width = ancho fijo de la primera,
 * column-rule-width = el de la última; la zona (grid-area) da el orden (order) y column-span: all
 * si ocupa toda la fila */
static bool grid_emit(CssPrep *p, Out *o, const char *name, const char *val)
{
	if (!strcmp(name, "grid-template-columns") || !strcmp(name, "grid-template")) {
		const char *v = val;
		if (!strcmp(name, "grid-template")) {
			const char *sl = strchr(val, '/');
			if (!sl)
				return true;
			v = sl + 1;
		}
		/* las pistas (con repeat() desplegado) y su ancho: px fijos, o el peso de fr en negativo
		 * (-10 = 1fr); los nombres de línea ([content-start]…) se guardan para grid-column */
		int wv[16], nt = 0;
		for (const char *q = v; *q && nt < 16;) {
			while (*q == ' ')
				q++;
			if (!*q)
				break;
			if (*q == '[') { // nombres de la línea que viene: la nt+1
				const char *e2 = strchr(q, ']');
				if (!e2)
					break;
				for (const char *t = q + 1; t < e2;) {
					while (t < e2 && *t == ' ')
						t++;
					const char *t0 = t;
					while (t < e2 && *t != ' ')
						t++;
					if (t > t0) {
						char key[64], val2[8];
						snprintf(key, sizeof key, "|%.*s", (int)(t - t0), t0);
						snprintf(val2, sizeof val2, "%d", nt + 1);
						map_set(&p->areas, key, strlen(key), val2, strlen(val2));
					}
				}
				q = e2 + 1;
				continue;
			}
			const char *t0 = q;
			int d = 0;
			while (*q && (d || (*q != ' ' && *q != '['))) {
				if (*q == '(')
					d++;
				else if (*q == ')')
					d--;
				q++;
			}
			char t[160];
			snprintf(t, sizeof t, "%.*s", (int)(q - t0), t0);
			int reps = 1;
			char *tr = t;
			if (!strncasecmp(t, "repeat(", 7)) {
				char *c = strchr(t, ',');
				if (!c)
					continue;
				*c = 0;
				tr = c + 1;
				char *end = strrchr(tr, ')');
				if (end)
					*end = 0;
				while (*tr == ' ')
					tr++;
				reps = atoi(t + 7);
				if (reps <= 0) { // auto-fill / auto-fit: las que quepan de su mínimo
					char *mm = strstr(tr, "minmax(");
					float m = mm ? px_of(mm + 7) : px_of(tr);
					reps = m > 0 ? (int)(p->w * 0.92f / (m + 16)) : 1;
					if (reps < 1)
						reps = 1;
				}
			}
			int w;
			float x;
			char *fr = strstr(tr, "fr");
			if (!strncasecmp(tr, "minmax(", 7)) {
				char *c = strchr(tr, ',');
				float hi = c ? px_of(c + 1 + strspn(c + 1, " ")) : -1;
				w = hi > 0 ? (int)hi : -10; // hasta un máximo en px: ese; si no, flexible
			} else if ((x = px_of(tr)) > 0)
				w = (int)x;
			else if (strchr(tr, '%'))
				w = (int)(p->w * strtof(tr, NULL) / 100);
			else if (fr)
				w = -(int)(strtof(tr, NULL) * 10 + 0.5f);
			else
				w = -10; // auto, min-content…: flexible
			if (w == 0)
				w = -10;
			for (int i = 0; i < reps && nt < 16; i++)
				wv[nt++] = w;
		}
		if (!nt)
			return true;
		char b2[24];
		snprintf(b2, sizeof b2, "%d", nt);
		decl_out(o, "column-count", b2);
		char cr[256];
		size_t cl = 0;
		for (int i = 0; i < nt && cl + 16 < sizeof cr; i++)
			cl += (size_t)snprintf(cr + cl, sizeof cr - cl, "%sg%d %d", i ? " " : "", i + 1, wv[i]);
		decl_out(o, "counter-reset", cr);
		return true;
	}
	if (!strcmp(name, "grid-template-areas")) { // cuántas columnas (si no lo dice grid-template-columns)
		char names[24][48];
		int pos[24][4], nc = 0;
		bool full[24];
		grid_areas_n(val, val + strlen(val), names, pos, full, &nc);
		if (nc > 0) {
			char b2[16];
			snprintf(b2, sizeof b2, "%d", nc);
			decl_out(o, "column-count", b2);
		}
		return true;
	}
	if (!strcmp(name, "grid-area") && (isdigit((unsigned char)val[0]) || !strncmp(val, "span", 4))) { // «fila / col / fila-fin / col-fin»
		int a[4] = {0}, k = 0;
		for (const char *q = val; *q && k < 4; k++) {
			a[k] = atoi(q);
			q = strchr(q, '/');
			if (!q)
				break;
			q++;
		}
		if (k >= 3 && a[3] < 0)
			decl_out(o, "column-span", "all");
		else if (k >= 3 && a[3] > a[1] && a[1] > 0)
			grid_pos(o, a[1], a[3] - a[1]);
		return true;
	}
	if (!strcmp(name, "grid-area")) {
		char nm[48];
		snprintf(nm, sizeof nm, "%.*s", (int)strcspn(val, " /"), val);
		const char *a = map_get(&p->areas, nm, strlen(nm));
		if (!a) { // no es una zona: quizá unas líneas con nombre («content» = content-start…content-end)
			char k1[80], k2[80];
			snprintf(k1, sizeof k1, "|%s-start", nm);
			snprintf(k2, sizeof k2, "|%s-end", nm);
			const char *s1 = map_get(&p->areas, k1, strlen(k1)), *s2 = map_get(&p->areas, k2, strlen(k2));
			if (s1 && s2 && atoi(s2) > atoi(s1))
				grid_pos(o, atoi(s1), atoi(s2) - atoi(s1));
			return true;
		}
		if (a) {
			int v[6] = {0};
			sscanf(a, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
			char b[96];
			snprintf(b, sizeof b, "%d", v[0]);
			decl_out(o, "order", b);
			if (v[2] > 0) { // su sitio: columna y fila (en counter-increment, que el navegador lee)
				snprintf(b, sizeof b, "gc %d gp %d gr %d gq %d", v[2], v[3], v[4], v[5]);
				decl_out(o, "counter-increment", b);
			} else if (v[1])
				decl_out(o, "column-span", "all");
		}
		return true;
	}
	if (!strcmp(name, "grid-column") && isalpha((unsigned char)val[0]) && strncmp(val, "span", 4)) {
		// con nombres de línea: «content», «content-start / content-end», «full»
		char a1[64], a2[64] = "";
		snprintf(a1, sizeof a1, "%.*s", (int)strcspn(val, " /"), val);
		const char *sl = strchr(val, '/');
		if (sl) {
			const char *b3 = sl + 1;
			while (*b3 == ' ')
				b3++;
			snprintf(a2, sizeof a2, "%.*s", (int)strcspn(b3, " "), b3);
		}
		char k1[80], k2[80];
		if (!a2[0]) {
			snprintf(k1, sizeof k1, "|%s-start", a1);
			snprintf(k2, sizeof k2, "|%s-end", a1);
		} else {
			snprintf(k1, sizeof k1, "|%s", a1);
			snprintf(k2, sizeof k2, "|%s", a2);
		}
		const char *s1 = map_get(&p->areas, k1, strlen(k1)), *s2 = map_get(&p->areas, k2, strlen(k2));
		if (s1 && s2 && atoi(s2) > atoi(s1))
			grid_pos(o, atoi(s1), atoi(s2) - atoi(s1));
		return true;
	}
	if (!strcmp(name, "grid-column")) {
		// «span N», «a / b», «a / span N», «a», «1 / -1»: columna de inicio y cuántas ocupa
		int start = 0, span = 0;
		const char *sl = strchr(val, '/');
		if (!strncmp(val, "span", 4))
			span = atoi(val + 4);
		else
			start = atoi(val);
		if (sl) {
			const char *b = sl + 1;
			while (*b == ' ')
				b++;
			if (!strncmp(b, "span", 4))
				span = atoi(b + 4);
			else {
				int end = atoi(b);
				if (end < 0) { // hasta el final: toda la fila
					decl_out(o, "column-span", "all");
					return true;
				}
				if (end > start && start > 0)
					span = end - start;
			}
		}
		grid_pos(o, start, span);
		return true;
	}
	if (!strcmp(name, "grid-column-start") || !strcmp(name, "grid-column-end"))
		return true; // se tratan juntas en decls_out
	return !strncmp(name, "grid-", 5); // las demás de la rejilla: nada (libcss tampoco)
}

static void grid_pos(Out *o, int start, int span)
{
	// en z-index (libcss lo guarda; el navegador no lo usa para nada): 100000 + inicio*100 + cuántas
	if (start <= 0 && span <= 0)
		return;
	char b[16];
	snprintf(b, sizeof b, "%d", 100000 + (start > 0 ? start : 0) * 100 + (span > 0 ? span : 1));
	decl_out(o, "z-index", b);
}

// el primer color de un degradado («linear-gradient(90deg, #123 0%, rgba(…) 100%)»)
/* un degradado (libcss no los conoce) como un color: el de la parada más opaca; si alguna es
 * transparente (el degradado que oscurece una foto para leer su texto), a medias */
static bool grad_color(const char *v, char *out, size_t n)
{
	const char *g = strstr(v, "gradient(");
	if (!g)
		return false;
	float br = 0, bgc = 0, bb = 0, ba = -1;
	bool clear = false, named = false;
	char bname[16] = "";
	for (const char *q = g + 9; *q && *q != ';';) {
		while (*q == ' ' || *q == ',')
			q++;
		float r = 0, gg = 0, b = 0, a = -1;
		const char *nx = q;
		if (*q == '#') {
			size_t l = 1;
			while (isxdigit((unsigned char)q[l]))
				l++;
			unsigned x = (unsigned)strtoul(q + 1, NULL, 16);
			if (l == 4)
				r = ((x >> 8) & 15) * 17, gg = ((x >> 4) & 15) * 17, b = (x & 15) * 17, a = 1;
			else if (l == 7)
				r = (x >> 16) & 255, gg = (x >> 8) & 255, b = x & 255, a = 1;
			else if (l == 9)
				r = (x >> 24) & 255, gg = (x >> 16) & 255, b = (x >> 8) & 255, a = (x & 255) / 255.0f;
			nx = q + l;
		} else if (!strncasecmp(q, "rgb", 3)) {
			const char *op = strchr(q, '('), *cl = op ? strchr(op, ')') : NULL;
			if (!cl)
				return false;
			float c[4] = {0, 0, 0, 1};
			int k = 0;
			for (const char *t = op + 1; t < cl && k < 4; k++) {
				c[k] = strtof(t, (char **)&t);
				if (*t == '%')
					c[k] = k < 3 ? c[k] * 2.55f : c[k] / 100, t++;
				while (t < cl && (*t == ',' || *t == ' ' || *t == '/'))
					t++;
			}
			r = c[0], gg = c[1], b = c[2], a = k >= 4 ? c[3] : 1;
			nx = cl + 1;
		} else if (!strncasecmp(q, "transparent", 11)) {
			clear = true;
			nx = q + 11;
		} else {
			static const char *const NAMED[] = {"white", "black", "red", "blue", "green", "gray", "grey", "orange", "purple", "navy",
			                                    "teal", "silver", "maroon", "olive", "yellow", "aqua", "fuchsia", "lime", NULL};
			for (int i = 0; NAMED[i]; i++) {
				size_t l = strlen(NAMED[i]);
				if (!strncasecmp(q, NAMED[i], l) && !isalnum((unsigned char)q[l])) {
					if (ba < 1)
						util_copy(bname, NAMED[i], sizeof bname), named = true, ba = 1;
					nx = q + l;
					break;
				}
			}
		}
		if (a >= 0) {
			if (a < 0.05f)
				clear = true;
			if (a > ba)
				br = r, bgc = gg, bb = b, ba = a, named = false;
		}
		// a la siguiente coma (lo de detrás de la parada: su posición) o al final
		q = nx;
		int d = 0;
		while (*q && (d || (*q != ',' && *q != ')'))) {
			if (*q == '(')
				d++;
			else if (*q == ')')
				d--;
			q++;
		}
		if (*q == ')')
			break;
	}
	if (ba < 0)
		return false;
	if (named && !clear) {
		snprintf(out, n, "%s", bname);
		return true;
	}
	if (named) // (con nombre y algo transparente: negro o blanco a medias, lo habitual)
		br = bgc = bb = !strcmp(bname, "white") ? 255 : 0;
	snprintf(out, n, "rgba(%d,%d,%d,%.2f)", (int)br, (int)bgc, (int)bb, clear ? ba * 0.55f : ba);
	return true;
}

/* background-size (libcss no lo conoce): en outline-style, que el navegador no pinta. cover = dashed,
 * contain = dotted, 100% 100% = groove (estirada), un ancho = double con outline-width */
static void bgsize_emit(Out *o, const char *v)
{
	while (*v == ' ')
		v++;
	if (!strncasecmp(v, "cover", 5))
		decl_out(o, "outline-style", "dashed");
	else if (!strncasecmp(v, "contain", 7))
		decl_out(o, "outline-style", "dotted");
	else if (!strncmp(v, "100%", 4))
		decl_out(o, "outline-style", "groove");
	else if (isdigit((unsigned char)*v) || *v == '.') {
		float x = len_px(v);
		if (x > 0 && !strchr(v, '%')) {
			char b[32];
			snprintf(b, sizeof b, "%gpx", x);
			decl_out(o, "outline-style", "double");
			decl_out(o, "outline-width", b);
		}
	}
}

/* translate(x[, y]) / translateX / translateY / translate3d: en counter-increment (c3x/c3y en px·10,
 * c3xp/c3yp en %·10), que el navegador aplica como un desplazamiento al pintar */
static bool translate_emit(Out *o, const char *v)
{
	const char *t = strstr(v, "translate");
	if (!t)
		return false;
	t += 9;
	int axis = 0; // 0 los dos, 1 x, 2 y
	if (*t == 'X' || *t == 'x')
		axis = 1, t++;
	else if (*t == 'Y' || *t == 'y')
		axis = 2, t++;
	else if (!strncmp(t, "3d", 2))
		t += 2;
	if (*t != '(')
		return false;
	t++;
	char b[96];
	size_t bl = 0;
	for (int k = 0; k < 2; k++) {
		while (*t == ' ')
			t++;
		char *z;
		float x = strtof(t, &z);
		if (z == t)
			break;
		bool pc = *z == '%';
		float px = pc ? x : len_px(t);
		if (!pc && px == -1 && x != -1)
			break;
		const char *nm = (axis == 2 || k == 1) ? (pc ? "c3yp" : "c3y") : (pc ? "c3xp" : "c3x");
		bl += (size_t)snprintf(b + bl, sizeof b - bl, "%s%s %d", bl ? " " : "", nm, (int)(px * 10));
		t = z + pc;
		while (*t && *t != ',' && *t != ')')
			t++;
		if (*t != ',' || axis)
			break;
		t++;
	}
	if (!bl)
		return false;
	decl_out(o, "counter-increment", b);
	return true;
}

static void decl_emit(Out *o, const char *name, const char *val)
{
	if (!strcmp(name, "overflow") && strchr(val, ' ') && strncmp(strchr(val, ' ') + 1, "!important", 10)) { // «hidden auto»: x e y
		char a1[24], a2[24];
		if (sscanf(val, "%23s %23s", a1, a2) == 2) {
			const char *imp = strstr(val, "!important") ? " !important" : "";
			char b[48];
			for (int k = 0; k < 2; k++) {
				const char *v = k ? a2 : a1;
				if (!strcmp(v, "clip"))
					v = "hidden";
				snprintf(b, sizeof b, "%s%s", v, imp);
				decl_out(o, k ? "overflow-y" : "overflow-x", b);
			}
			return;
		}
	}
	if (!strcmp(name, "transform") || !strcmp(name, "translate")) {
		if (!strcmp(name, "translate")) { // la propiedad suelta: «x y»
			char b[96];
			snprintf(b, sizeof b, "translate(%s)", val);
			for (char *q = b + 10; *q; q++)
				if (*q == ' ' && q[1] && q[1] != ')')
					*q = ',';
			translate_emit(o, b);
		} else
			translate_emit(o, val);
		return;
	}
	if (!strcmp(name, "aspect-ratio")) { // en column-width: 1000000 + ancho/alto·1000, en px
		char *z;
		float a = strtof(val, &z), bq = 1;
		while (*z == ' ')
			z++;
		if (*z == '/')
			bq = strtof(z + 1, NULL);
		if (a > 0 && bq > 0) {
			char b[32];
			snprintf(b, sizeof b, "%dpx", 1000000 + (int)(a / bq * 1000)); // (+1000000: no es un column-width de verdad)
			decl_out(o, "column-width", b);
		}
		return;
	}
	if (!strcmp(name, "object-fit")) { // en column-rule-style
		const char *m = !strncmp(val, "contain", 7) || !strncmp(val, "scale-down", 10) ? "dotted" : !strncmp(val, "fill", 4) ? "solid"
		                : !strncmp(val, "cover", 5) ? "dashed" : NULL;
		if (m)
			decl_out(o, "column-rule-style", m);
		return;
	}
	if (!strcmp(name, "border-radius")) { // en column-rule-width: px, o 100000 + % (50% → 100050px)
		char *z;
		float x = strtof(val, &z);
		if (z != val && x > 0) {
			char b[32];
			if (*z == '%')
				snprintf(b, sizeof b, "%dpx", 100000 + (int)(x > 50 ? 50 : x));
			else {
				float px = len_px(val);
				if (px <= 0)
					return;
				snprintf(b, sizeof b, "%dpx", (int)(px > 9000 ? 9000 : px));
			}
			decl_out(o, "column-rule-width", b);
		}
		return;
	}
	if (!strcmp(name, "background-size")) {
		bgsize_emit(o, val);
		return;
	}
	if (!strcmp(name, "background")) { // «… center / cover …»: el tamaño fuera (libcss tira todo si lo lleva)
		const char *sl = NULL;
		int d = 0;
		for (const char *q = val; *q; q++) {
			if (*q == '(')
				d++;
			else if (*q == ')')
				d--;
			else if (*q == '/' && !d) {
				sl = q;
				break;
			}
		}
		if (sl) {
			const char *z = sl + 1;
			while (*z == ' ')
				z++;
			const char *e2 = z;
			for (int k = 0; k < 2 && *e2; k++) { // una o dos palabras de tamaño
				const char *w0 = e2;
				while (*e2 && *e2 != ' ' && *e2 != ',')
					e2++;
				bool sz = !strncasecmp(w0, "cover", 5) || !strncasecmp(w0, "contain", 7) || !strncasecmp(w0, "auto", 4) || isdigit((unsigned char)*w0) || *w0 == '.';
				if (!sz) {
					e2 = w0;
					break;
				}
				if (!strncasecmp(w0, "cover", 5) || !strncasecmp(w0, "contain", 7))
					break;
				while (*e2 == ' ')
					e2++;
			}
			char b[1024];
			snprintf(b, sizeof b, "%.*s %s", (int)(sl - val), val, e2);
			decl_out(o, "background", b);
			bgsize_emit(o, z);
			return;
		}
	}
	if (!strcmp(name, "display")) { // lo que libcss no conoce: contents (marcado con column-fill: auto), flow-root, dos palabras
		const char *imp = strstr(val, "!important");
		char v[48];
		snprintf(v, sizeof v, "%.*s", (int)(imp ? (size_t)(imp - val) : strlen(val)), val);
		for (size_t l = strlen(v); l && v[l - 1] == ' '; l--)
			v[l - 1] = 0;
		static const char *const DM[][2] = {{"contents", "block"}, {"flow-root", "block"}, {"block flow-root", "block"}, {"inline flow-root", "inline-block"},
		                                    {"inline flex", "inline-flex"}, {"block flex", "flex"}, {"inline grid", "inline-grid"}, {"block grid", "grid"},
		                                    {"block flow", "block"}, {"inline flow", "inline"}, {"inline flow-root", "inline-block"}, {"list-item block", "list-item"}};
		for (size_t i = 0; i < sizeof DM / sizeof DM[0]; i++)
			if (!strcasecmp(v, DM[i][0])) {
				char b2[64];
				snprintf(b2, sizeof b2, "%s%s", DM[i][1], imp ? " !important" : "");
				decl_out(o, "display", b2);
				decl_out(o, "column-fill", !strcmp(DM[i][0], "contents") ? (imp ? "auto !important" : "auto") : "balance");
				return;
			}
	}
	static const char *const MAP1[][2] = {
	    {"margin-inline-start", "margin-left"}, {"margin-inline-end", "margin-right"}, {"margin-block-start", "margin-top"},
	    {"margin-block-end", "margin-bottom"}, {"padding-inline-start", "padding-left"}, {"padding-inline-end", "padding-right"},
	    {"padding-block-start", "padding-top"}, {"padding-block-end", "padding-bottom"}, {"inset-inline-start", "left"},
	    {"inset-inline-end", "right"}, {"inset-block-start", "top"}, {"inset-block-end", "bottom"}, {"inline-size", "width"},
	    {"block-size", "height"}, {"min-inline-size", "min-width"}, {"max-inline-size", "max-width"}, {"min-block-size", "min-height"},
	    {"max-block-size", "max-height"}, {"border-inline-start", "border-left"}, {"border-inline-end", "border-right"},
	    {"border-block-start", "border-top"}, {"border-block-end", "border-bottom"}, {"border-start-start-radius", "border-top-left-radius"},
	    {"border-start-end-radius", "border-top-right-radius"}, {"border-end-start-radius", "border-bottom-left-radius"},
	    {"border-end-end-radius", "border-bottom-right-radius"}};
	for (size_t i = 0; i < sizeof MAP1 / sizeof MAP1[0]; i++) {
		size_t l = strlen(MAP1[i][0]);
		if (!strncmp(name, MAP1[i][0], l) && (!name[l] || name[l] == '-')) { // también border-inline-start-color…
			char nn[64];
			snprintf(nn, sizeof nn, "%s%s", MAP1[i][1], name + l);
			decl_out(o, nn, val);
			return;
		}
	}
	// dos lados: «margin-inline: a b», «inset: a b c d»
	static const char *const MAP2[][3] = {{"margin-inline", "margin-left", "margin-right"}, {"margin-block", "margin-top", "margin-bottom"},
	                                      {"padding-inline", "padding-left", "padding-right"}, {"padding-block", "padding-top", "padding-bottom"},
	                                      {"inset-inline", "left", "right"}, {"inset-block", "top", "bottom"},
	                                      {"border-inline", "border-left", "border-right"}, {"border-block", "border-top", "border-bottom"}};
	for (size_t i = 0; i < sizeof MAP2 / sizeof MAP2[0]; i++)
		if (!strcmp(name, MAP2[i][0]) || (!strncmp(name, MAP2[i][0], strlen(MAP2[i][0])) && name[strlen(MAP2[i][0])] == '-')) {
			const char *suf = name + strlen(MAP2[i][0]);
			char a[160], b[160], n1[64], n2[64];
			snprintf(n1, sizeof n1, "%s%s", MAP2[i][1], suf);
			snprintf(n2, sizeof n2, "%s%s", MAP2[i][2], suf);
			const char *sp = !strncmp(name, "border", 6) ? NULL : strchr(val, ' ');
			if (sp && !strchr(val, '(')) {
				snprintf(a, sizeof a, "%.*s", (int)(sp - val), val);
				snprintf(b, sizeof b, "%s", sp + 1);
			} else {
				snprintf(a, sizeof a, "%s", val);
				snprintf(b, sizeof b, "%s", val);
			}
			decl_out(o, n1, a);
			decl_out(o, n2, b);
			return;
		}
	if (!strcmp(name, "inset")) {
		char v[4][64];
		int n = 0;
		for (const char *q = val; *q && n < 4;) {
			while (*q == ' ')
				q++;
			const char *t = q;
			while (*q && *q != ' ')
				q++;
			if (q > t)
				snprintf(v[n++], sizeof v[0], "%.*s", (int)(q - t), t);
		}
		if (!n)
			return;
		const char *t = v[0], *r = n > 1 ? v[1] : t, *bt = n > 2 ? v[2] : t, *l = n > 3 ? v[3] : r;
		decl_out(o, "top", t), decl_out(o, "right", r), decl_out(o, "bottom", bt), decl_out(o, "left", l);
		return;
	}
	decl_out(o, name, val);
}

/* los nombres de línea de una lista de pistas («[a-start] 1fr [a-end b-start] 200px [b-end]»): cada
 * nombre, su número de línea (para grid-column / grid-area con nombre) */
static void grid_lines(CssPrep *p, const char *q, const char *e)
{
	const char *sl = memchr(q, '/', (size_t)(e - q)); // grid-template: filas / columnas
	if (sl && memchr(sl, '[', (size_t)(e - sl)))
		q = sl + 1;
	int nt = 0;
	while (q < e) {
		while (q < e && isspace((unsigned char)*q))
			q++;
		if (q >= e)
			break;
		if (*q == '[') {
			const char *z = memchr(q, ']', (size_t)(e - q));
			if (!z)
				return;
			for (const char *t = q + 1; t < z;) {
				while (t < z && *t == ' ')
					t++;
				const char *t0 = t;
				while (t < z && *t != ' ')
					t++;
				if (t > t0) {
					char key[64], v[8];
					snprintf(key, sizeof key, "|%.*s", (int)(t - t0), t0);
					snprintf(v, sizeof v, "%d", nt + 1);
					map_set(&p->areas, key, strlen(key), v, strlen(v));
				}
			}
			q = z + 1;
			continue;
		}
		const char *t0 = q;
		int d = 0;
		while (q < e && (d || (!isspace((unsigned char)*q) && *q != '['))) {
			if (*q == '(')
				d++;
			else if (*q == ')')
				d--;
			q++;
		}
		nt += !strncasecmp(t0, "repeat(", 7) && atoi(t0 + 7) > 0 ? atoi(t0 + 7) : 1;
	}
}

/* grid-template-areas: «'a a' 'b c' 'd d'»: cada zona, su orden (por filas), si ocupa toda su fila, y su
 * columna y fila (de inicio y cuántas) */
static int grid_areas_n(const char *s, const char *e, char names[24][48], int pos[24][4], bool full[24], int *ncols)
{
	int nn = 0, row = 0;
	*ncols = 0;
	while (s < e) {
		while (s < e && *s != '"' && *s != '\'')
			s++;
		if (s >= e)
			break;
		char q = *s++;
		const char *re = memchr(s, q, (size_t)(e - s));
		if (!re)
			break;
		char tok[16][48];
		int nt = 0;
		for (const char *t = s; t < re && nt < 16;) {
			while (t < re && isspace((unsigned char)*t))
				t++;
			const char *t0 = t;
			while (t < re && !isspace((unsigned char)*t))
				t++;
			if (t > t0)
				snprintf(tok[nt++], sizeof tok[0], "%.*s", (int)(t - t0), t0);
		}
		if (nt > *ncols)
			*ncols = nt;
		bool all_same = nt > 0;
		for (int i = 1; i < nt; i++)
			all_same &= !strcmp(tok[i], tok[0]);
		for (int i = 0; i < nt; i++) {
			if (tok[i][0] == '.')
				continue;
			int k;
			for (k = 0; k < nn && strcmp(names[k], tok[i]); k++)
				;
			if (k == nn) {
				if (nn == 24)
					continue;
				util_copy(names[nn], tok[i], sizeof names[0]), full[nn] = all_same;
				pos[nn][0] = i, pos[nn][1] = i, pos[nn][2] = row, pos[nn][3] = row;
				nn++;
			} else {
				if (i < pos[k][0])
					pos[k][0] = i;
				if (i > pos[k][1])
					pos[k][1] = i;
				if (row > pos[k][3])
					pos[k][3] = row;
			}
		}
		row++;
		s = re + 1;
	}
	return nn;
}

static void grid_areas(CssPrep *p, const char *s, const char *e)
{
	char names[24][48];
	int pos[24][4], nc;
	bool full[24] = {0};
	int nn = grid_areas_n(s, e, names, pos, full, &nc);
	for (int k = 0; k < nn; k++) {
		char v[48];
		snprintf(v, sizeof v, "%d,%d,%d,%d,%d,%d", k + 1, full[k] ? 1 : 0, pos[k][0] + 1, pos[k][1] - pos[k][0] + 1, pos[k][2] + 1, pos[k][3] - pos[k][2] + 1);
		map_set(&p->areas, names[k], strlen(names[k]), v, strlen(v));
	}
}

// las de una regla: las variables (pasada 1) o las declaraciones ya traducidas (pasada 2)
static void decls(CssPrep *p, const char *s, const char *e, bool root, Map *local, Out *o, bool inl)
{
	while (s < e) {
		const char *c = find_top(s, e, ";{");
		if (c < e && *c == '{') { // anidada (CSS nesting): se salta
			const char *be = block_end(c + 1, e);
			s = be < e ? be + 1 : e;
			continue;
		}
		const char *colon = find_top(s, c, ":");
		if (colon < c) {
			const char *n0 = s, *n1 = colon, *v0 = colon + 1, *v1 = c;
			trim(&n0, &n1);
			trim(&v0, &v1);
			if (!o && ((n1 - n0 == 19 && !strncasecmp(n0, "grid-template-areas", 19)) ||
			           (((n1 - n0 == 13 && !strncasecmp(n0, "grid-template", 13)) || (n1 - n0 == 4 && !strncasecmp(n0, "grid", 4))) &&
			            (memchr(v0, '"', (size_t)(v1 - v0)) || memchr(v0, '\'', (size_t)(v1 - v0))))))
				grid_areas(p, v0, v1);
			if (!o && memchr(v0, '[', (size_t)(v1 - v0)) &&
			    ((n1 - n0 > 2 && n0[0] == '-' && n0[1] == '-') || (n1 - n0 >= 13 && !strncasecmp(n0, "grid-template", 13))))
				grid_lines(p, v0, v1);
			if (n1 - n0 > 2 && n0[0] == '-' && n0[1] == '-') {
				if (!o) { // pasada 1: guardarla
					map_set(&p->any, n0, (size_t)(n1 - n0), v0, (size_t)(v1 - v0));
					if (root)
						map_set(&p->root, n0, (size_t)(n1 - n0), v0, (size_t)(v1 - v0));
				} else if (local)
					map_set(local, n0, (size_t)(n1 - n0), v0, (size_t)(v1 - v0));
			}
		}
		s = c + 1;
	}
	(void)o, (void)inl;
}

// grid-column-start y grid-column-end de una regla, juntas
static void grid_start_end(const char *s, const char *e, Out *o)
{
	int start = 0, end = 0, span = 0;
	bool any = false;
	for (const char *q = s; q < e;) {
		const char *c = find_top(q, e, ";");
		const char *colon = find_top(q, c, ":");
		if (colon < c) {
			const char *n0 = skip_ws(q, colon), *n1 = colon;
			trim(&n0, &n1);
			const char *v = skip_ws(colon + 1, c);
			size_t nl = (size_t)(n1 - n0);
			if (nl == 17 && !strncasecmp(n0, "grid-column-start", 17)) {
				any = true;
				if (!strncmp(v, "span", 4))
					span = atoi(v + 4);
				else
					start = atoi(v);
			} else if (nl == 15 && !strncasecmp(n0, "grid-column-end", 15)) {
				any = true;
				if (!strncmp(v, "span", 4))
					span = atoi(v + 4);
				else
					end = atoi(v);
			}
		}
		q = c + 1;
	}
	if (!any)
		return;
	if (end < 0) {
		decl_out(o, "column-span", "all");
		return;
	}
	if (!span && end > 0 && start > 0 && end > start)
		span = end - start;
	grid_pos(o, start, span);
}

static void decls_out(CssPrep *p, const char *s, const char *e, const Map *local, Out *o)
{
	// ¿la regla ya dice su color de fondo? (entonces un degradado en background-image no lo pisa)
	bool has_bgc = has_ci(s, (size_t)(e - s), "background-color") || has_ci(s, (size_t)(e - s), "background:");
	if (has_ci(s, (size_t)(e - s), "grid-column-"))
		grid_start_end(s, e, o);
	Out a = {0}, b = {0};
	while (s < e) {
		const char *c = find_top(s, e, ";{");
		if (c < e && *c == '{') {
			const char *be = block_end(c + 1, e);
			s = be < e ? be + 1 : e;
			continue;
		}
		const char *colon = find_top(s, c, ":");
		if (colon < c) {
			const char *n0 = skip_ws(s, colon), *n1 = colon, *v0 = colon + 1, *v1 = c; // sin comentarios delante
			trim(&n0, &n1);
			trim(&v0, &v1);
			if (n1 > n0 && !(n1 - n0 > 1 && n0[0] == '-' && n0[1] == '-') && v1 > v0) {
				char name[64];
				size_t nl = (size_t)(n1 - n0) < sizeof name - 1 ? (size_t)(n1 - n0) : sizeof name - 1;
				for (size_t i = 0; i < nl; i++)
					name[i] = (char)tolower((unsigned char)n0[i]);
				name[nl] = 0;
				a.n = 0, a.bad = false;
				bool ok = true;
				// el valor sin comentarios («1px /* fino */ solid»)
				if (memchr(v0, '/', (size_t)(v1 - v0))) {
					b.n = 0, b.bad = false;
					for (const char *q = v0; q < v1;) {
						if (q + 1 < v1 && q[0] == '/' && q[1] == '*') {
							const char *z = skip_ws(q, v1);
							put(&b, " ", 1);
							q = z;
						} else if (*q == '"' || *q == '\'') {
							const char *z = skip_str(q, v1);
							put(&b, q, (size_t)(z - q));
							q = z;
						} else
							put(&b, q++, 1);
					}
					if (b.p)
						v0 = b.p, v1 = b.p + b.n;
				}
				if (has_ci(v0, (size_t)(v1 - v0), "var("))
					ok = subst(p, local, v0, (size_t)(v1 - v0), &a, 0);
				else
					put(&a, v0, (size_t)(v1 - v0));
				if (ok && !a.bad && a.n) {
					b.n = 0, b.bad = false;
					G_KEEP_CALC = !strcmp(name, "width") && memchr(a.p, '%', a.n) && has_ci(a.p, a.n, "calc(");
					funcs(a.p, a.n, &b);
					G_KEEP_CALC = false;
					a.n = 0;
					if (!b.bad && b.n) {
						colors(b.p, b.n, &a);
						char gc[48];
						if (!a.bad && a.n && !strcmp(name, "background-image") && has_bgc && strstr(a.p, "gradient(")) {
							// (un degradado sobre el color de la regla: se queda el color)
						} else if (!a.bad && a.n && (!strcmp(name, "background") || !strcmp(name, "background-image")) && strstr(a.p, "gradient(")) {
							// un degradado (libcss no los conoce): de fondo, un color. Con varias capas, el
							// color base es el de la última («…gradient(…), url(…) #1b2838»)
							const char *lc = NULL;
							int dd = 0;
							for (const char *q = a.p; *q; q++) {
								if (*q == '(')
									dd++;
								else if (*q == ')')
									dd--;
								else if (*q == ',' && !dd)
									lc = q + 1;
							}
							const char *hx = NULL;
							if (lc && !strstr(lc, "gradient(")) {
								int d2 = 0;
								for (const char *q = lc; *q && !hx; q++) {
									if (*q == '(')
										d2++;
									else if (*q == ')')
										d2--;
									else if (!d2 && (*q == '#' || !strncasecmp(q, "rgb", 3)) && (q == lc || q[-1] == ' '))
										hx = q;
								}
							}
							size_t hl = 0;
							if (hx)
								hl = *hx == '#' ? strcspn(hx, " ;!") : (strchr(hx, ')') ? (size_t)(strchr(hx, ')') + 1 - hx) : 0);
							if (hl && hl < sizeof gc) {
								memcpy(gc, hx, hl);
								gc[hl] = 0;
								decl_emit(o, "background-color", gc);
							} else if (grad_color(a.p, gc, sizeof gc))
								decl_emit(o, "background-color", gc);
						} else if (!a.bad && a.n && !grid_emit(p, o, name, a.p))
							decl_emit(o, name, a.p);
					}
				}
			}
		}
		s = c + 1;
	}
	free(a.p), free(b.p);
}

/* ------------------------------------------------------------ selectores */
static bool sel_unsupported(const char *s, size_t n)
{
	static const char *const BAD[] = {":has(", "::-webkit-", "::-moz-", ":-moz-", ":-webkit-", "::-ms-", ":-ms-", ":host", "::slotted",
	                                  "::part", ":focus-visible", ":focus-within", "::placeholder", "::selection", "::backdrop",
	                                  "::marker", "::file-selector-button", ":popover-open", ":modal", ":state(", ":dir(",
	                                  ":user-invalid", ":user-valid", ":autofill", ":defined", ":open", ":closed", "&", ":nth-child(odd of",
	                                  "::view-transition", ":lang(", NULL};
	for (int i = 0; BAD[i]; i++)
		if (has_ci(s, n, BAD[i]))
			return true;
	return false;
}

// :is(a, b) / :where(…) / :matches(…): un selector por cada opción (como mucho 24 en total)
static void sel_expand(const char *s, size_t n, Out *o, bool *first, int *budget)
{
	static const char *const FN[] = {":is(", ":where(", ":matches(", ":-webkit-any(", ":any(", NULL};
	const char *f = NULL;
	size_t fl = 0;
	for (size_t i = 0; i < n && !f; i++)
		for (int k = 0; FN[k]; k++) {
			size_t l = strlen(FN[k]);
			if (i + l <= n && !strncasecmp(s + i, FN[k], l)) {
				f = s + i, fl = l;
				break;
			}
		}
	if (!f) {
		if (*budget <= 0 || sel_unsupported(s, n))
			return;
		(*budget)--;
		if (!*first)
			put(o, ",", 1);
		*first = false;
		// :not(a, b) → :not(a):not(b)
		const char *nt = NULL;
		for (size_t i = 0; i + 5 <= n; i++)
			if (!strncasecmp(s + i, ":not(", 5)) {
				nt = s + i;
				break;
			}
		if (nt) {
			const char *a = nt + 5, *cl = a;
			int d = 1;
			for (; cl < s + n; cl++) {
				if (*cl == '(')
					d++;
				else if (*cl == ')' && --d == 0)
					break;
			}
			if (cl < s + n && find_top(a, cl, ",") < cl) {
				put(o, s, (size_t)(nt - s));
				for (const char *q = a; q < cl;) {
					const char *c = find_top(q, cl, ",");
					const char *x = q, *y = c;
					trim(&x, &y);
					puts_(o, ":not(");
					put(o, x, (size_t)(y - x));
					puts_(o, ")");
					q = c + 1;
				}
				put(o, cl + 1, (size_t)(s + n - cl - 1));
				return;
			}
		}
		put(o, s, n);
		return;
	}
	const char *a = f + fl, *cl = a, *e = s + n;
	int d = 1;
	for (; cl < e; cl++) {
		if (*cl == '(')
			d++;
		else if (*cl == ')' && --d == 0)
			break;
	}
	if (cl >= e)
		return;
	for (const char *q = a; q < cl;) {
		const char *c = find_top(q, cl, ",");
		const char *x = q, *y = c;
		trim(&x, &y);
		char *tmp = malloc(n + 1);
		if (!tmp)
			return;
		size_t tn = 0;
		memcpy(tmp, s, (size_t)(f - s)), tn = (size_t)(f - s);
		memcpy(tmp + tn, x, (size_t)(y - x)), tn += (size_t)(y - x);
		memcpy(tmp + tn, cl + 1, (size_t)(e - cl - 1)), tn += (size_t)(e - cl - 1);
		sel_expand(tmp, tn, o, first, budget);
		free(tmp);
		q = c + 1;
	}
}

/* ------------------------------------------------------------ las reglas */
static void rules(CssPrep *p, const char *s, const char *e, Out *o)
{
	while (s < e) {
		s = skip_ws(s, e);
		if (s >= e)
			break;
		if (*s == '}' || *s == ';') {
			s++;
			continue;
		}
		if (*s == '<' && e - s >= 4 && !strncmp(s, "<!--", 4)) {
			s += 4;
			continue;
		}
		if (*s == '-' && e - s >= 3 && !strncmp(s, "-->", 3)) {
			s += 3;
			continue;
		}
		if (*s == '@') {
			const char *n0 = s + 1, *n1 = n0;
			while (n1 < e && (isalnum((unsigned char)*n1) || *n1 == '-'))
				n1++;
			const char *pe = find_top(n1, e, "{;");
			if (pe >= e)
				break;
			size_t nl = (size_t)(n1 - n0);
			if (*pe == ';') { // @import, @charset, @layer a, b;…: nada que hacer
				s = pe + 1;
				continue;
			}
			const char *b0 = pe + 1, *b1 = block_end(b0, e);
			char pre[512];
			const char *p0 = n1, *p1 = pe;
			trim(&p0, &p1);
			size_t pl = (size_t)(p1 - p0) < sizeof pre - 1 ? (size_t)(p1 - p0) : sizeof pre - 1;
			memcpy(pre, p0, pl);
			pre[pl] = 0;
#define IS(x) (nl == strlen(x) && !strncasecmp(n0, x, nl))
			if (IS("media")) {
				if (cssprep_media_ok(pre, p->w))
					rules(p, b0, b1, o);
			} else if (IS("supports")) {
				// lo que se pide con «not» suele ser lo de respaldo para navegadores viejos, y lo demás,
				// mejoras: se aplican las mejoras (sin selector(), que es para :has() y parecidos)
				if (strncasecmp(pre, "not", 3) && !has_ci(pre, pl, "selector("))
					rules(p, b0, b1, o);
			} else if (IS("layer") || IS("scope") || IS("starting-style")) {
				if (!IS("starting-style"))
					rules(p, b0, b1, o);
			}
			// @font-face, @keyframes, @container, @page, @property…: fuera
#undef IS
			s = b1 < e ? b1 + 1 : e;
			continue;
		}
		const char *se = find_top(s, e, "{;");
		if (se >= e)
			break;
		if (*se == ';') {
			s = se + 1;
			continue;
		}
		const char *b0 = se + 1, *b1 = block_end(b0, e);
		const char *s0 = s, *s1 = se;
		trim(&s0, &s1);
		if (!o) { // pasada 1: las variables
			bool vars = has_ci(b0, (size_t)(b1 - b0), "--");
			if (vars || has_ci(b0, (size_t)(b1 - b0), "grid-template"))
				decls(p, b0, b1, vars && sel_is_root(p, s0, s1), NULL, NULL, false);
		} else {
			Out sel = {0};
			bool first = true;
			int total = 0;
			for (const char *q = s0; q < s1 && total < 400;) {
				const char *c = find_top(q, s1, ",");
				const char *x = q, *y = c;
				trim(&x, &y);
				int budget = 24; // lo que salga de desdoblar :is() en este selector
				if (y > x)
					sel_expand(x, (size_t)(y - x), &sel, &first, &budget);
				total += 24 - budget;
				q = c + 1;
			}
			if (!first && !sel.bad) {
				Map local = {0};
				// las de la raíz (html, :root) ya están en su valor final (una regla de @media de después
				// puede cambiarlas): las propias de la regla, solo en las demás
				if (has_ci(b0, (size_t)(b1 - b0), "--") && !sel_is_root(p, s0, s1))
					decls(p, b0, b1, false, &local, o, false);
				size_t mark = o->n;
				put(o, sel.p, sel.n);
				put(o, "{", 1);
				size_t inner = o->n;
				decls_out(p, b0, b1, local.cap ? &local : NULL, o);
				if (o->n == inner) // sin nada: fuera la regla
					o->n = mark, o->p[mark] = 0;
				else
					put(o, "}", 1);
				map_free(&local);
			}
			free(sel.p);
		}
		s = b1 < e ? b1 + 1 : e;
	}
}

void cssprep_vars(CssPrep *p, const char *css, size_t n)
{
	if (p && css && n)
		rules(p, css, css + n, NULL);
}

char *cssprep_rewrite(CssPrep *p, const char *css, size_t n, size_t *out_n)
{
	Out o = {0};
	if (!p || !css)
		return NULL;
	o.cap = n + 64;
	o.p = malloc(o.cap);
	if (!o.p)
		return NULL;
	o.p[0] = 0;
	rules(p, css, css + n, &o);
	if (o.bad) {
		free(o.p);
		return NULL;
	}
	*out_n = o.n;
	return o.p;
}

char *cssprep_inline(CssPrep *p, const char *d, size_t n, size_t *out_n)
{
	Out o = {0};
	if (!p || !d)
		return NULL;
	Map local = {0};
	if (has_ci(d, n, "--"))
		decls(p, d, d + n, false, &local, &o, true);
	decls_out(p, d, d + n, local.cap ? &local : NULL, &o);
	map_free(&local);
	if (o.bad) {
		free(o.p);
		return NULL;
	}
	*out_n = o.n;
	if (!o.p) {
		o.p = malloc(1);
		if (o.p)
			o.p[0] = 0;
	}
	return o.p;
}
