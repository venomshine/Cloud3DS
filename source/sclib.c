/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "sclib.h"
#include "cJSON.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCL_FILE APP_DIR "/soundcloud_biblioteca.json"

typedef struct {
	char name[64];
	ScSaved t[SCL_TRACKS];
	int n;
} List_;

static struct {
	bool loaded;
	List_ likes;
	List_ *l[SCL_LISTS];
	int nl;
} L;

static List_ *get(int list) { return list == SCL_LIKES ? &L.likes : list >= 0 && list < L.nl ? L.l[list] : NULL; }

static void put_track(cJSON *arr, const ScSaved *s)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "id", s->id);
	cJSON_AddStringToObject(o, "title", s->title);
	cJSON_AddStringToObject(o, "artist", s->artist);
	cJSON_AddStringToObject(o, "uid", s->uid);
	cJSON_AddNumberToObject(o, "ms", s->dur_ms);
	cJSON_AddItemToArray(arr, o);
}

static void put_list(cJSON *o, const List_ *l)
{
	cJSON *a = cJSON_AddArrayToObject(o, "tracks");
	for (int i = 0; i < l->n; i++)
		put_track(a, &l->t[i]);
}

static void save(void)
{
	cJSON *root = cJSON_CreateObject();
	put_list(cJSON_AddObjectToObject(root, "likes"), &L.likes);
	cJSON *ls = cJSON_AddArrayToObject(root, "lists");
	for (int i = 0; i < L.nl; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", L.l[i]->name);
		put_list(o, L.l[i]);
		cJSON_AddItemToArray(ls, o);
	}
	char *txt = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!txt)
		return;
	FILE *f = fopen(SCL_FILE ".tmp", "w"); // (entero o nada: a uno temporal y luego en su sitio)
	bool ok = f && fputs(txt, f) >= 0;
	if (f)
		ok = fclose(f) == 0 && ok;
	free(txt);
	if (ok) {
		remove(SCL_FILE);
		rename(SCL_FILE ".tmp", SCL_FILE);
	}
}

static const char *js(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static void read_list(const cJSON *o, List_ *l)
{
	const cJSON *it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(o, "tracks"))
	{
		if (l->n >= SCL_TRACKS || !*js(it, "id"))
			continue;
		ScSaved *s = &l->t[l->n++];
		util_strlcpy(s->id, js(it, "id"), sizeof s->id);
		util_strlcpy(s->title, js(it, "title"), sizeof s->title);
		util_strlcpy(s->artist, js(it, "artist"), sizeof s->artist);
		util_strlcpy(s->uid, js(it, "uid"), sizeof s->uid);
		const cJSON *ms = cJSON_GetObjectItemCaseSensitive(it, "ms");
		s->dur_ms = cJSON_IsNumber(ms) ? (u32)ms->valuedouble : 0;
	}
}

void scl_load(void)
{
	if (L.loaded)
		return;
	L.loaded = true;
	FILE *f = fopen(SCL_FILE, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = n > 0 && n < (4 << 20) ? malloc(n + 1) : NULL;
	if (b && fread(b, 1, n, f) == (size_t)n) {
		b[n] = 0;
		cJSON *root = cJSON_Parse(b), *it;
		read_list(cJSON_GetObjectItemCaseSensitive(root, "likes"), &L.likes);
		cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "lists"))
		{
			if (L.nl >= SCL_LISTS)
				break;
			List_ *l = calloc(1, sizeof *l);
			if (!l)
				break;
			util_strlcpy(l->name, js(it, "name"), sizeof l->name);
			read_list(it, l);
			L.l[L.nl++] = l;
		}
		cJSON_Delete(root);
	}
	free(b);
	fclose(f);
}

static int find(const List_ *l, const char *id)
{
	for (int i = 0; l && i < l->n; i++)
		if (!strcmp(l->t[i].id, id))
			return i;
	return -1;
}

bool scl_has(int list, const char *id)
{
	scl_load();
	return find(get(list), id) >= 0;
}
bool scl_liked(const char *id) { return scl_has(SCL_LIKES, id); }

int scl_add(int list, const ScTrack *t)
{
	scl_load();
	List_ *l = get(list);
	if (!l || find(l, t->id) >= 0)
		return 0;
	if (l->n >= SCL_TRACKS)
		return -1;
	memmove(&l->t[1], &l->t[0], sizeof l->t[0] * l->n); // (lo último, arriba)
	ScSaved *s = &l->t[0];
	memset(s, 0, sizeof *s);
	util_strlcpy(s->id, t->id, sizeof s->id);
	util_strlcpy(s->title, t->title, sizeof s->title);
	util_strlcpy(s->artist, t->artist, sizeof s->artist);
	util_strlcpy(s->uid, t->uid, sizeof s->uid);
	s->dur_ms = t->dur_ms;
	l->n++;
	save();
	return 1;
}

void scl_remove(int list, const char *id)
{
	scl_load();
	List_ *l = get(list);
	int k = find(l, id);
	if (k < 0)
		return;
	memmove(&l->t[k], &l->t[k + 1], sizeof l->t[0] * (l->n - k - 1));
	l->n--;
	save();
}

int scl_like(const ScTrack *t, bool on)
{
	if (on)
		return scl_add(SCL_LIKES, t);
	if (!scl_liked(t->id))
		return 0;
	scl_remove(SCL_LIKES, t->id);
	return 1;
}

int scl_nlists(void)
{
	scl_load();
	return L.nl;
}
const char *scl_list_name(int i) { return i >= 0 && i < L.nl ? L.l[i]->name : ""; }
int scl_count(int list)
{
	scl_load();
	const List_ *l = get(list);
	return l ? l->n : 0;
}

int scl_list_new(const char *name)
{
	scl_load();
	List_ *l = L.nl < SCL_LISTS ? calloc(1, sizeof *l) : NULL;
	if (!l)
		return -1;
	util_strlcpy(l->name, name, sizeof l->name);
	L.l[L.nl++] = l;
	save();
	return L.nl - 1;
}

bool scl_list_rename(int i, const char *name)
{
	List_ *l = i >= 0 ? get(i) : NULL;
	if (!l || !name[0])
		return false;
	util_strlcpy(l->name, name, sizeof l->name);
	save();
	return true;
}

void scl_list_delete(int i)
{
	if (i < 0 || i >= L.nl)
		return;
	free(L.l[i]);
	memmove(&L.l[i], &L.l[i + 1], sizeof L.l[0] * (L.nl - i - 1));
	L.nl--;
	save();
}

int scl_tracks(int list, ScSaved *out, int max)
{
	scl_load();
	const List_ *l = get(list);
	int n = l ? (l->n < max ? l->n : max) : 0;
	if (n)
		memcpy(out, l->t, sizeof *out * n);
	return n;
}

void scl_ids(int list, char *out, size_t n)
{
	scl_load();
	const List_ *l = get(list);
	size_t o = 0;
	out[0] = 0;
	for (int i = 0; l && i < l->n; i++)
		o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s", i ? "," : "", l->t[i].id);
	if (o >= n)
		out[n - 1] = 0;
}
