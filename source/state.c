/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "state.h"
#include "util.h"

#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

AppState G;
static LightLock lock;
static int next_id = 1;

void state_init(void)
{
	LightLock_Init(&lock);
	memset(&G, 0, sizeof G);
	G.protect_sys = true;
	pin_regen();
}

void state_lock(void)
{
	LightLock_Lock(&lock);
}

void state_unlock(void)
{
	LightLock_Unlock(&lock);
}

void pin_regen(void)
{
	u8 b[4];
	util_rand_bytes(b, 4);
	u32 v = (b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24) % 10000;
	snprintf(G.pin, sizeof G.pin, "%04u", (unsigned)v);
	util_rand_hex(G.qr_key, 8);
	G.pin_fails = 0;
	G.rev++;
}

void device_revoke(int i)
{
	Device *d = &G.dev[i];
	if (!d->used)
		return;
	util_strlcpy(G.revoked[G.revoked_next], d->token, sizeof G.revoked[0]);
	G.revoked_next = (G.revoked_next + 1) % MAX_REVOKED;
	event_add(EV_REVOKED, "", d->name, 0);
	d->used = false;
	G.dirty_cfg = true;
	G.rev++;
}

int xfer_begin(XferKind k, const char *name, const char *device, u64 total)
{
	/* reutiliza el hueco libre o el terminado más antiguo */
	int best = -1;
	u64 best_t = ~0ull;
	for (int i = 0; i < MAX_XFERS; i++) {
		Xfer *x = &G.xfers[i];
		if (x->st == XS_FREE) {
			best = i;
			break;
		}
		if (x->st != XS_RUNNING && x->t_end < best_t) {
			best = i;
			best_t = x->t_end;
		}
	}
	if (best < 0)
		return 0;
	Xfer *x = &G.xfers[best];
	memset(x, 0, sizeof *x);
	x->st = XS_RUNNING;
	x->kind = k;
	x->id = next_id++;
	util_strlcpy(x->name, name, sizeof x->name);
	util_strlcpy(x->device, device, sizeof x->device);
	x->total = total;
	x->t_start = x->spd_t = util_ms();
	G.rev++;
	return x->id;
}

Xfer *xfer_get(int id)
{
	if (id <= 0)
		return NULL;
	for (int i = 0; i < MAX_XFERS; i++)
		if (G.xfers[i].st != XS_FREE && G.xfers[i].id == id)
			return &G.xfers[i];
	return NULL;
}

void xfer_progress(int id, u64 done)
{
	Xfer *x = xfer_get(id);
	if (!x)
		return;
	x->done = done;
	u64 now = util_ms();
	u64 dt = now - x->spd_t;
	if (dt >= 500) {
		u64 inst = (done - x->spd_done) * 1000 / dt;
		x->speed = x->speed ? (x->speed * 6 + inst * 4) / 10 : inst;
		x->spd_t = now;
		x->spd_done = done;
	}
}

void xfer_end(int id, XferState st)
{
	Xfer *x = xfer_get(id);
	if (!x)
		return;
	x->st = st;
	x->t_end = util_ms();
	if (st == XS_DONE)
		x->done = x->total;
	G.rev++;
}

int xfer_active(void)
{
	int n = 0;
	for (int i = 0; i < MAX_XFERS; i++)
		if (G.xfers[i].st == XS_RUNNING)
			n++;
	return n;
}

void event_add(EventKind k, const char *name, const char *device, u64 size)
{
	G.hist_head = (G.hist_head + 1) % MAX_HISTORY;
	Event *e = &G.hist[G.hist_head];
	memset(e, 0, sizeof *e);
	e->kind = k;
	e->when = util_unix_now();
	e->size = size;
	util_strlcpy(e->name, name ? name : "", sizeof e->name);
	util_strlcpy(e->device, device ? device : "", sizeof e->device);
	if (G.hist_n < MAX_HISTORY)
		G.hist_n++;
	G.rev++;
	G.dirty_cfg = true;
}

const Event *event_at(int i)
{
	if (i < 0 || i >= G.hist_n)
		return NULL;
	return &G.hist[(G.hist_head - i + MAX_HISTORY) % MAX_HISTORY];
}

/* ---- config.json ---- */

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > 512 * 1024) {
		fclose(f);
		return NULL;
	}
	char *b = malloc(n + 1);
	if (b && fread(b, 1, n, f) != (size_t)n) {
		free(b);
		b = NULL;
	}
	if (b)
		b[n] = 0;
	fclose(f);
	return b;
}

static const char *jstr(cJSON *o, const char *k)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(cJSON *o, const char *k, double def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static bool jbool(cJSON *o, const char *k, bool def)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

void config_load(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(APP_DIR, 0777);
	char *txt = read_file(CONFIG_PATH);
	if (!txt)
		return;
	cJSON *r = cJSON_Parse(txt);
	free(txt);
	if (!r)
		return;
	G.lang = (int)jnum(r, "lang", 0);
	G.light = jbool(r, "light", false);
	G.readonly = jbool(r, "readonly", false);
	G.protect_sys = jbool(r, "protect_sys", true);

	cJSON *devs = cJSON_GetObjectItemCaseSensitive(r, "devices"), *d;
	int i = 0;
	cJSON_ArrayForEach(d, devs)
	{
		if (i >= MAX_DEVICES)
			break;
		const char *tok = jstr(d, "token");
		if (strlen(tok) != 32)
			continue;
		Device *v = &G.dev[i++];
		v->used = true;
		util_strlcpy(v->token, tok, sizeof v->token);
		util_strlcpy(v->name, jstr(d, "name"), sizeof v->name);
		util_strlcpy(v->ip, jstr(d, "ip"), sizeof v->ip);
		v->kind = (int)jnum(d, "kind", 0);
		v->created = (u64)jnum(d, "created", 0);
		v->last_seen = (u64)jnum(d, "seen", 0);
	}

	/* historial: del más antiguo al más reciente */
	cJSON *hist = cJSON_GetObjectItemCaseSensitive(r, "history"), *e;
	cJSON_ArrayForEach(e, hist)
	{
		event_add((EventKind)jnum(e, "k", 0), jstr(e, "n"), jstr(e, "d"), (u64)jnum(e, "s", 0));
		G.hist[G.hist_head].when = (u64)jnum(e, "t", 0);
	}
	G.dirty_cfg = false;
	cJSON_Delete(r);
}

void config_save(void)
{
	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "version", 1);
	cJSON_AddNumberToObject(r, "lang", G.lang);
	cJSON_AddBoolToObject(r, "light", G.light);
	cJSON_AddBoolToObject(r, "readonly", G.readonly);
	cJSON_AddBoolToObject(r, "protect_sys", G.protect_sys);
	cJSON *devs = cJSON_AddArrayToObject(r, "devices");
	for (int i = 0; i < MAX_DEVICES; i++) {
		Device *v = &G.dev[i];
		if (!v->used)
			continue;
		cJSON *d = cJSON_CreateObject();
		cJSON_AddStringToObject(d, "token", v->token);
		cJSON_AddStringToObject(d, "name", v->name);
		cJSON_AddStringToObject(d, "ip", v->ip);
		cJSON_AddNumberToObject(d, "kind", v->kind);
		cJSON_AddNumberToObject(d, "created", (double)v->created);
		cJSON_AddNumberToObject(d, "seen", (double)v->last_seen);
		cJSON_AddItemToArray(devs, d);
	}
	cJSON *hist = cJSON_AddArrayToObject(r, "history");
	for (int i = G.hist_n - 1; i >= 0; i--) {
		const Event *e = event_at(i);
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "k", e->kind);
		cJSON_AddNumberToObject(o, "t", (double)e->when);
		cJSON_AddNumberToObject(o, "s", (double)e->size);
		cJSON_AddStringToObject(o, "n", e->name);
		cJSON_AddStringToObject(o, "d", e->device);
		cJSON_AddItemToArray(hist, o);
	}
	char *txt = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	if (!txt)
		return;
	/* escritura atómica: archivo temporal y renombrar */
	const char *tmp = CONFIG_PATH ".tmp";
	FILE *f = fopen(tmp, "wb");
	if (f) {
		size_t n = strlen(txt);
		bool ok = fwrite(txt, 1, n, f) == n;
		ok = fclose(f) == 0 && ok;
		if (ok) {
			remove(CONFIG_PATH);
			rename(tmp, CONFIG_PATH);
		}
	}
	free(txt);
	G.dirty_cfg = false;
}
