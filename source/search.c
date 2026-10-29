/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "search.h"
#include "trash.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* letras latinas con acento (U+00C0..U+00FF) → la letra sin él */
static const char LAT1[64] = "aaaaaaaceeeeiiii"
                             "dnooooo*ouuuuyts"
                             "aaaaaaaceeeeiiii"
                             "dnooooo/ouuuuyty";

void search_fold(const char *in, char *out, size_t n)
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char *)in;
	while (*p && o + 1 < n) {
		if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
			out[o++] = LAT1[p[1] - 0x80];
			p += 2;
		} else {
			out[o++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
			p++;
		}
	}
	out[o] = 0;
}

bool search_match(const char *name, const char *qfold)
{
	char f[512];
	search_fold(name, f, sizeof f);
	return strstr(f, qfold) != NULL;
}

typedef struct {
	char q[256];
	volatile bool *cancel;
	SearchCb cb;
	void *u;
	bool stop, skip_n3ds, skip_trash, timeout;
	u64 deadline;
	int dirs;
} Walk;

typedef struct {
	Walk *w;
	const char *dir;
	char *sub; /* nombres de las subcarpetas, uno tras otro con su \0 */
	size_t len, cap;
} Level;

static bool level_cb(const FsxEntry *e, void *u)
{
	Level *l = u;
	Walk *w = l->w;
	if (w->stop || (w->cancel && *w->cancel) || (w->deadline && util_ms() > w->deadline)) {
		w->timeout = !w->stop && w->deadline && util_ms() > w->deadline;
		w->stop = true;
		return false;
	}
	if (search_match(e->name, w->q)) { // la fecha, solo de lo que coincide: pedirla de todo es lento
		FsxEntry m = *e;
		char full[1024];
		snprintf(full, sizeof full, "%s/%s", strcmp(l->dir, "/") ? l->dir : "", e->name);
		m.mtime = fsx_mtime(full);
		if (!w->cb(l->dir, &m, w->u)) {
			w->stop = true;
			return false;
		}
	}
	if (e->dir) {
		size_t n = strlen(e->name) + 1;
		if (l->len + n > l->cap) {
			size_t cap = l->cap ? l->cap * 2 : 2048;
			char *b = realloc(l->sub, cap);
			if (!b)
				return true;
			l->sub = b, l->cap = cap;
		}
		memcpy(l->sub + l->len, e->name, n);
		l->len += n;
	}
	return true;
}

static void walk(Walk *w, const char *dir, int depth)
{
	Level l = {w, dir, NULL, 0, 0};
	w->dirs++;
	if (fsx_list(dir, false, level_cb, &l) < 0)
		return;
	for (size_t i = 0; i < l.len && !w->stop && depth < SEARCH_DEPTH; i += strlen(l.sub + i) + 1) {
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", l.sub + i);
		if (w->skip_n3ds && !strcasecmp(p, "/Nintendo 3DS"))
			continue;
		if (w->skip_trash && !strcasecmp(p, TRASH_DIR))
			continue;
		walk(w, p, depth + 1);
	}
	free(l.sub);
}

int search_walk(const char *root, const char *q, volatile bool *cancel, u32 max_ms, bool *timed_out, SearchCb cb, void *u)
{
	Walk w = {0};
	w.deadline = max_ms ? util_ms() + max_ms : 0;
	search_fold(q, w.q, sizeof w.q);
	w.cancel = cancel;
	w.cb = cb;
	w.u = u;
	w.skip_n3ds = strncasecmp(root, "/Nintendo 3DS", 13) != 0;
	w.skip_trash = strncasecmp(root, TRASH_DIR, strlen(TRASH_DIR)) != 0;
	if (w.q[0])
		walk(&w, root, 0);
	if (timed_out)
		*timed_out = w.timeout;
	return w.dirs;
}
