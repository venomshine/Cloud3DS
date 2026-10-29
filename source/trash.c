/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "trash.h"
#include "cJSON.h"
#include "fsx.h"
#include "ops.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct {
	LightLock lock;
	TrashItem it[TRASH_MAX];
	int n;
	u32 next;
	bool ready;
} TR;

static void item_dir(u32 id, char *out, size_t n)
{
	snprintf(out, n, TRASH_DIR "/%08lx", (unsigned long)id);
}

static void item_path(const TrashItem *t, char *out, size_t n)
{
	snprintf(out, n, TRASH_DIR "/%08lx/%s", (unsigned long)t->id, t->name);
}

/* el índice, como JSON, escrito aparte y luego reemplazado */
static void save(void)
{
	cJSON *r = cJSON_CreateArray();
	for (int i = 0; i < TR.n; i++) {
		const TrashItem *t = &TR.it[i];
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "id", t->id);
		cJSON_AddStringToObject(o, "n", t->name);
		cJSON_AddStringToObject(o, "f", t->from);
		cJSON_AddStringToObject(o, "d", t->device);
		cJSON_AddNumberToObject(o, "t", (double)t->when);
		cJSON_AddNumberToObject(o, "s", (double)t->size);
		cJSON_AddBoolToObject(o, "dir", t->dir);
		cJSON_AddNumberToObject(o, "c", t->color);
		cJSON_AddItemToArray(r, o);
	}
	char *txt = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	if (!txt)
		return;
	FILE *f = fopen("sdmc:" TRASH_DIR "/index.json.part", "wb");
	if (f) {
		bool ok = fputs(txt, f) >= 0;
		ok = fclose(f) == 0 && ok;
		if (ok) {
			remove("sdmc:" TRASH_DIR "/index.json");
			rename("sdmc:" TRASH_DIR "/index.json.part", "sdmc:" TRASH_DIR "/index.json");
		}
	}
	free(txt);
}

void trash_init(void)
{
	LightLock_Init(&TR.lock);
	fsx_mkdirs(TRASH_DIR);
	FILE *f = fopen("sdmc:" TRASH_DIR "/index.json", "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *txt = sz > 0 && sz < 512 * 1024 ? malloc(sz + 1) : NULL;
		if (txt && fread(txt, 1, sz, f) == (size_t)sz) {
			txt[sz] = 0;
			cJSON *r = cJSON_Parse(txt), *o;
			cJSON_ArrayForEach(o, r)
			{
				if (TR.n == TRASH_MAX)
					break;
				TrashItem *t = &TR.it[TR.n];
				memset(t, 0, sizeof *t);
				cJSON *v;
				t->id = (v = cJSON_GetObjectItem(o, "id")) ? (u32)v->valuedouble : 0;
				util_strlcpy(t->name, (v = cJSON_GetObjectItem(o, "n")) && cJSON_IsString(v) ? v->valuestring : "", sizeof t->name);
				util_strlcpy(t->from, (v = cJSON_GetObjectItem(o, "f")) && cJSON_IsString(v) ? v->valuestring : "", sizeof t->from);
				util_strlcpy(t->device, (v = cJSON_GetObjectItem(o, "d")) && cJSON_IsString(v) ? v->valuestring : "", sizeof t->device);
				t->when = (v = cJSON_GetObjectItem(o, "t")) ? (u64)v->valuedouble : 0;
				t->size = (v = cJSON_GetObjectItem(o, "s")) ? (u64)v->valuedouble : 0;
				t->dir = cJSON_IsTrue(cJSON_GetObjectItem(o, "dir"));
				t->color = (v = cJSON_GetObjectItem(o, "c")) ? (u8)v->valuedouble : 0;
				char p[700];
				bool d;
				u64 s;
				item_path(t, p, sizeof p);
				if (t->id && t->name[0] && fsx_stat(p, &d, &s) == 0) // lo que ya no está, fuera del índice
					TR.n++;
				if (t->id >= TR.next)
					TR.next = t->id + 1;
			}
			cJSON_Delete(r);
		}
		free(txt);
		fclose(f);
	}
	if (!TR.next)
		TR.next = 1;
	TR.ready = true;
	trash_expire();
}

typedef struct {
	const char *dir;
	u64 total;
	int depth;
} SizeCtx;

static bool size_cb(const FsxEntry *e, void *u)
{
	SizeCtx *c = u;
	if (!e->dir) {
		c->total += e->size;
	} else if (c->depth < 24) {
		char sub[1024];
		snprintf(sub, sizeof sub, "%s/%s", c->dir, e->name);
		SizeCtx s = {sub, 0, c->depth + 1};
		fsx_list(sub, false, size_cb, &s);
		c->total += s.total;
	}
	return true;
}

static void remove_at(int i)
{
	memmove(&TR.it[i], &TR.it[i + 1], (TR.n - i - 1) * sizeof TR.it[0]);
	TR.n--;
}

static int find(u32 id)
{
	for (int i = 0; i < TR.n; i++)
		if (TR.it[i].id == id)
			return i;
	return -1;
}

static void purge_at(int i)
{
	char d[64];
	item_dir(TR.it[i].id, d, sizeof d);
	fsx_rmrf(d);
	remove_at(i);
}

int trash_put(const char *path, const char *device, u8 color)
{
	if (!TR.ready || !strncasecmp(path, TRASH_DIR, strlen(TRASH_DIR)))
		return -1;
	bool dir;
	u64 sz;
	if (fsx_stat(path, &dir, &sz) < 0)
		return -1;
	const char *nm = strrchr(path, '/');
	nm = nm ? nm + 1 : path;
	if (dir) {
		SizeCtx c = {path, 0, 0};
		fsx_list(path, false, size_cb, &c);
		sz = c.total;
	}
	LightLock_Lock(&TR.lock);
	if (TR.n == TRASH_MAX) { // llena: se va lo más viejo
		int old = 0;
		for (int i = 1; i < TR.n; i++)
			if (TR.it[i].when < TR.it[old].when)
				old = i;
		purge_at(old);
	}
	TrashItem t = {0};
	t.id = TR.next++;
	util_strlcpy(t.name, nm, sizeof t.name);
	util_strlcpy(t.from, path, sizeof t.from);
	util_strlcpy(t.device, device ? device : "", sizeof t.device);
	t.when = util_unix_now();
	t.size = sz;
	t.dir = dir;
	t.color = color;
	char d[64], dst[700];
	item_dir(t.id, d, sizeof d);
	item_path(&t, dst, sizeof dst);
	int r = -2;
	if (fsx_mkdirs(d) == 0 && ops_move(path, dst) == OPS_OK) {
		memmove(&TR.it[1], &TR.it[0], TR.n * sizeof TR.it[0]); // lo más reciente, primero
		TR.it[0] = t;
		TR.n++;
		save();
		r = 0;
	} else {
		fsx_rmrf(d);
	}
	LightLock_Unlock(&TR.lock);
	return r;
}

int trash_list(TrashItem *out, int max)
{
	LightLock_Lock(&TR.lock);
	int n = TR.n < max ? TR.n : max;
	memcpy(out, TR.it, n * sizeof *out);
	LightLock_Unlock(&TR.lock);
	return n;
}

bool trash_get(u32 id, TrashItem *out)
{
	LightLock_Lock(&TR.lock);
	int i = find(id);
	if (i >= 0)
		*out = TR.it[i];
	LightLock_Unlock(&TR.lock);
	return i >= 0;
}

int trash_restore(u32 id, char *dst, size_t n)
{
	LightLock_Lock(&TR.lock);
	int i = find(id), r = -1;
	if (i >= 0) {
		TrashItem *t = &TR.it[i];
		char src[700], parent[512];
		item_path(t, src, sizeof src);
		util_strlcpy(parent, t->from, sizeof parent);
		char *sl = strrchr(parent, '/');
		if (sl && sl != parent)
			*sl = 0;
		else
			strcpy(parent, "/");
		fsx_mkdirs(parent); // la carpeta de antes pudo desaparecer
		util_strlcpy(dst, t->from, n);
		bool d;
		u64 s;
		if (fsx_stat(dst, &d, &s) == 0) // ya hay algo con ese nombre: se guarda con otro
			ops_free_name(dst, n, parent, t->name);
		if (ops_move(src, dst) == OPS_OK) {
			char dd[64];
			item_dir(t->id, dd, sizeof dd);
			fsx_rmrf(dd);
			remove_at(i);
			save();
			r = 0;
		}
	}
	LightLock_Unlock(&TR.lock);
	return r;
}

int trash_purge(u32 id)
{
	LightLock_Lock(&TR.lock);
	int i = find(id);
	if (i >= 0) {
		purge_at(i);
		save();
	}
	LightLock_Unlock(&TR.lock);
	return i >= 0 ? 0 : -1;
}

int trash_empty(void)
{
	LightLock_Lock(&TR.lock);
	int n = TR.n;
	while (TR.n)
		purge_at(TR.n - 1);
	save();
	LightLock_Unlock(&TR.lock);
	return n;
}

void trash_expire(void)
{
	u64 now = util_unix_now();
	if (now < 1000000000ull) // la hora de la consola no es fiable: mejor no borrar nada
		return;
	LightLock_Lock(&TR.lock);
	bool changed = false;
	for (int i = TR.n - 1; i >= 0; i--)
		if (TR.it[i].when + TRASH_DAYS * 86400ull < now) {
			purge_at(i);
			changed = true;
		}
	if (changed)
		save();
	LightLock_Unlock(&TR.lock);
}

u64 trash_size(int *count)
{
	LightLock_Lock(&TR.lock);
	u64 t = 0;
	for (int i = 0; i < TR.n; i++)
		t += TR.it[i].size;
	if (count)
		*count = TR.n;
	LightLock_Unlock(&TR.lock);
	return t;
}

u64 trash_make_room(u64 need)
{
	LightLock_Lock(&TR.lock);
	u64 freed = 0;
	while (TR.n && freed < need) { // lo más viejo es lo último
		freed += TR.it[TR.n - 1].size;
		purge_at(TR.n - 1);
	}
	if (freed)
		save();
	LightLock_Unlock(&TR.lock);
	return freed;
}
