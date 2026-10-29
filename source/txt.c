/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "txt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TXT_ROOT
#define TXT_ROOT "sdmc:"
#endif

static bool utf8_ok(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n;) {
		unsigned c = s[i];
		int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (k < 0 || (k == 1 && c < 0xC2) || i + k >= n)
			return false;
		for (int j = 1; j <= k; j++)
			if ((s[i + j] & 0xC0) != 0x80)
				return false;
		i += k + 1;
	}
	return true;
}

static bool grow(TxtDoc *d, int need)
{
	if (need <= d->cap)
		return true;
	int cap = d->cap ? d->cap * 2 : 64;
	while (cap < need)
		cap *= 2;
	char **ln = realloc(d->ln, cap * sizeof *ln);
	if (!ln)
		return false;
	d->ln = ln;
	d->cap = cap;
	return true;
}

static char *dup_n(const char *s, size_t n)
{
	char *r = malloc(n + 1);
	if (r) {
		memcpy(r, s, n);
		r[n] = 0;
	}
	return r;
}

/* parte s en líneas (sin \r final) y las mete en la posición at; o todas o ninguna */
static int put_lines(TxtDoc *d, int at, const char *s, size_t n)
{
	int cnt = 1;
	for (size_t i = 0; i < n; i++)
		cnt += s[i] == '\n';
	char **nw = calloc(cnt, sizeof *nw);
	if (!nw || !grow(d, d->n + cnt)) {
		free(nw);
		return -1;
	}
	size_t a = 0;
	for (int k = 0, i = 0; k < cnt; i++) {
		if ((size_t)i < n && s[i] != '\n')
			continue;
		size_t e = i;
		if (e > a && s[e - 1] == '\r')
			e--;
		if (!(nw[k++] = dup_n(s + a, e - a))) {
			for (int j = 0; j < k; j++)
				free(nw[j]);
			free(nw);
			return -1;
		}
		a = i + 1;
	}
	memmove(d->ln + at + cnt, d->ln + at, (d->n - at) * sizeof *d->ln);
	memcpy(d->ln + at, nw, cnt * sizeof *nw);
	d->n += cnt;
	free(nw);
	return cnt;
}

int txt_new(TxtDoc *d)
{
	memset(d, 0, sizeof *d);
	return put_lines(d, 0, "", 0) == 1 ? TXT_OK : TXT_ERR_MEM;
}

int txt_load(TxtDoc *d, const char *path)
{
	memset(d, 0, sizeof *d);
	char sd[800];
	snprintf(sd, sizeof sd, TXT_ROOT "%s", path);
	FILE *f = fopen(sd, "rb");
	if (!f)
		return TXT_ERR_IO;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return TXT_ERR_IO;
	}
	d->size = (uint64_t)sz;
	size_t want = (size_t)sz > TXT_VIEW_MAX ? TXT_VIEW_MAX : (size_t)sz;
	d->cut = (size_t)sz > want;
	unsigned char *b = malloc(want + 1);
	if (!b) {
		fclose(f);
		return TXT_ERR_MEM;
	}
	size_t got = fread(b, 1, want, f);
	fclose(f);
	if (got != want) {
		free(b);
		return TXT_ERR_IO;
	}
	b[got] = 0;
	size_t off = 0;
	if (got >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF)
		d->bom = true, off = 3;
	const unsigned char *s = b + off;
	size_t n = got - off;
	// binario: hay bytes nulos al principio. Se ve, pero cambiando lo que no es texto por «·»
	bool bin = memchr(s, 0, n < 8192 ? n : 8192) != NULL;
	// si lo cortamos, el último carácter puede quedar partido: se quita
	if (d->cut) {
		size_t j = n;
		while (j > 0 && n - j < 3 && (s[j - 1] & 0xC0) == 0x80)
			j--;
		if (j > 0 && s[j - 1] >= 0xC0)
			n = j - 1;
	}
	bool utf8 = !bin && utf8_ok(s, n);
	char *txt = (char *)s;
	char *conv = NULL;
	if (bin) {
		for (size_t i = 0; i < n; i++)
			if (b[off + i] < 0x20 && b[off + i] != '\n' && b[off + i] != '\t')
				b[off + i] = '.';
			else if (b[off + i] >= 0x7F)
				b[off + i] = '.';
	} else if (!utf8) {
		// no es UTF-8: casi siempre Latin-1/Windows-1252 (textos viejos en español); se muestra así
		conv = malloc(n * 2 + 1);
		if (!conv) {
			free(b);
			return TXT_ERR_MEM;
		}
		size_t o = 0;
		for (size_t i = 0; i < n; i++) {
			unsigned c = s[i];
			if (c < 0x80)
				conv[o++] = (char)c;
			else if (c < 0xA0)
				conv[o++] = '?';
			else
				conv[o++] = (char)(0xC0 | (c >> 6)), conv[o++] = (char)(0x80 | (c & 0x3F));
		}
		conv[o] = 0;
		txt = conv;
		n = o;
	}
	const char *nl = memchr(txt, '\n', n);
	d->crlf = nl && nl > txt && nl[-1] == '\r';
	d->final_nl = n > 0 && txt[n - 1] == '\n';
	size_t body = d->final_nl ? n - 1 : n;
	if (d->final_nl && body > 0 && txt[body - 1] == '\r')
		body--;
	int r = put_lines(d, 0, txt, body);
	free(conv);
	free(b);
	if (r < 0) {
		txt_free(d);
		return TXT_ERR_MEM;
	}
	d->ro = bin ? TXT_RO_BIN : !utf8 ? TXT_RO_ENC : (d->cut || d->size > TXT_EDIT_MAX) ? TXT_RO_BIG : TXT_RW;
	return TXT_OK;
}

/* deshacer y rehacer: cada cambio es «las líneas [i, i+oldn) pasan a ser el texto newt»
 * (newt NULL: ninguna línea). Se guardan los textos de antes y de después. */
#define TXT_MAX_OPS 200

static void op_free(TxtOp *o)
{
	free(o->oldt);
	free(o->newt);
}

static void hist_clear(TxtDoc *d)
{
	for (int k = 0; k < d->nops; k++)
		op_free(&d->ops[k]);
	d->nops = d->pos = 0;
	d->saved = -1;
}

static int count_lines(const char *s)
{
	int n = 1;
	for (; *s; s++)
		n += *s == '\n';
	return n;
}

/* el cambio en sí, sin apuntarlo */
static bool splice_raw(TxtDoc *d, int i, int oldn, const char *s)
{
	if (i < 0 || oldn < 0 || i + oldn > d->n || (!s && d->n - oldn < 1))
		return false;
	if (s && put_lines(d, i + oldn, s, strlen(s)) < 0) // sin memoria: se queda como estaba
		return false;
	for (int k = 0; k < oldn; k++)
		free(d->ln[i + k]);
	memmove(d->ln + i, d->ln + i + oldn, (d->n - i - oldn) * sizeof *d->ln);
	d->n -= oldn;
	return true;
}

static bool splice(TxtDoc *d, int i, int oldn, const char *s)
{
	char *oldt = oldn ? txt_join(d, i, oldn) : NULL, *newt = s ? strdup(s) : NULL;
	bool rec = (!oldn || oldt) && (!s || newt);
	if (!splice_raw(d, i, oldn, s)) {
		free(oldt);
		free(newt);
		return false;
	}
	for (int k = d->pos; k < d->nops; k++) // lo que se podía rehacer ya no vale
		op_free(&d->ops[k]);
	d->nops = d->pos;
	if (d->saved > d->pos)
		d->saved = -1;
	if (rec && d->nops == TXT_MAX_OPS) { // lleno: se olvida el más antiguo
		op_free(&d->ops[0]);
		memmove(d->ops, d->ops + 1, (TXT_MAX_OPS - 1) * sizeof *d->ops);
		d->nops--, d->pos--;
		d->saved = d->saved > 0 ? d->saved - 1 : -1;
	}
	if (rec && !d->ops)
		d->ops = calloc(TXT_MAX_OPS, sizeof *d->ops);
	if (rec && d->ops) {
		d->ops[d->nops++] = (TxtOp){i, oldn, s ? count_lines(s) : 0, oldt, newt};
		d->pos = d->nops;
	} else { // no se pudo apuntar: mejor sin historial que con uno que no cuadra
		free(oldt);
		free(newt);
		hist_clear(d);
	}
	d->dirty = d->pos != d->saved;
	return true;
}

int txt_undo(TxtDoc *d)
{
	if (d->pos == 0)
		return -1;
	TxtOp *o = &d->ops[d->pos - 1];
	if (!splice_raw(d, o->i, o->newn, o->oldt))
		return -1;
	d->pos--;
	d->dirty = d->pos != d->saved;
	return o->i;
}

int txt_redo(TxtDoc *d)
{
	if (d->pos >= d->nops)
		return -1;
	TxtOp *o = &d->ops[d->pos];
	if (!splice_raw(d, o->i, o->oldn, o->newt))
		return -1;
	d->pos++;
	d->dirty = d->pos != d->saved;
	return o->i;
}

bool txt_can_undo(const TxtDoc *d) { return d->pos > 0; }
bool txt_can_redo(const TxtDoc *d) { return d->pos < d->nops; }

bool txt_replace(TxtDoc *d, int i, int count, const char *s)
{
	if (i < 0 || count < 1 || i + count > d->n)
		return false;
	return splice(d, i, count, s);
}

bool txt_set(TxtDoc *d, int i, const char *s)
{
	if (i < 0 || i >= d->n)
		return false;
	if (!strcmp(d->ln[i], s))
		return true;
	return splice(d, i, 1, s);
}

char *txt_join(const TxtDoc *d, int i, int count)
{
	size_t n = 1;
	for (int k = i; k < i + count && k < d->n; k++)
		n += strlen(d->ln[k]) + 1;
	char *r = malloc(n), *o = r;
	if (!r)
		return NULL;
	for (int k = i; k < i + count && k < d->n; k++) {
		size_t l = strlen(d->ln[k]);
		memcpy(o, d->ln[k], l);
		o += l;
		if (k < i + count - 1 && k < d->n - 1)
			*o++ = '\n';
	}
	*o = 0;
	return r;
}

bool txt_insert(TxtDoc *d, int at, const char *s)
{
	if (at < 0)
		at = 0;
	if (at > d->n)
		at = d->n;
	return splice(d, at, 0, s);
}

void txt_remove(TxtDoc *d, int i)
{
	if (i < 0 || i >= d->n)
		return;
	if (d->n == 1) // siempre queda al menos una línea, vacía
		splice(d, 0, 1, "");
	else
		splice(d, i, 1, NULL);
}

bool txt_move(TxtDoc *d, int i, int dir)
{
	int j = i + dir;
	if (i < 0 || j < 0 || i >= d->n || j >= d->n)
		return false;
	int a = i < j ? i : j;
	size_t la = strlen(d->ln[a]), lb = strlen(d->ln[a + 1]);
	char *t = malloc(la + lb + 2);
	if (!t)
		return false;
	memcpy(t, d->ln[a + 1], lb); // las dos líneas, al revés
	t[lb] = '\n';
	memcpy(t + lb + 1, d->ln[a], la + 1);
	bool ok = splice(d, a, 2, t);
	free(t);
	return ok;
}

int txt_save(TxtDoc *d, const char *path)
{
	if (d->ro != TXT_RW)
		return TXT_ERR_IO;
	char dst[800], tmp[820], old[820];
	snprintf(dst, sizeof dst, TXT_ROOT "%s", path);
	const char *slash = strrchr(path, '/');
	int dl = slash ? (int)(slash - path) : 0;
	snprintf(tmp, sizeof tmp, TXT_ROOT "%.*s/.c3d_edit.part", dl, path);
	snprintf(old, sizeof old, TXT_ROOT "%.*s/.c3d_old.part", dl, path);
	remove(tmp);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return TXT_ERR_IO;
	bool ok = true;
	if (d->bom)
		ok = fwrite("\xEF\xBB\xBF", 1, 3, f) == 3;
	const char *eol = d->crlf ? "\r\n" : "\n";
	size_t el = strlen(eol);
	for (int i = 0; ok && i < d->n; i++) {
		size_t l = strlen(d->ln[i]);
		ok = fwrite(d->ln[i], 1, l, f) == l;
		if (ok && (i < d->n - 1 || d->final_nl))
			ok = fwrite(eol, 1, el, f) == el;
	}
	ok = fflush(f) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(tmp);
		return TXT_ERR_IO;
	}
	// el original se aparta y solo se borra cuando el nuevo ya está en su sitio
	remove(old);
	bool had = rename(dst, old) == 0;
	if (rename(tmp, dst) != 0) {
		if (had)
			rename(old, dst);
		remove(tmp);
		return TXT_ERR_IO;
	}
	if (had)
		remove(old);
	d->dirty = false;
	d->saved = d->pos;
	return TXT_OK;
}

void txt_free(TxtDoc *d)
{
	hist_clear(d);
	free(d->ops);
	for (int i = 0; i < d->n; i++)
		free(d->ln[i]);
	free(d->ln);
	memset(d, 0, sizeof *d);
}
