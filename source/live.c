/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "live.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define QMAX 64

static struct {
	LightLock lock;
	bool init;
	char path[700], device[48], open_req[700];
	bool on, save_req;
	u64 last;        /* la última vez que la web dio señales */
	LiveOp q[QMAX];
	int qh, qn;
	char *text;      /* lo que publicó la consola */
	u32 rev;
	int cur;
	bool dirty, ok;
} L;

static void lock(void)
{
	if (!L.init) {
		LightLock_Init(&L.lock);
		L.init = true;
	}
	LightLock_Lock(&L.lock);
}
static void unlock(void) { LightLock_Unlock(&L.lock); }

void live_open(const char *path, const char *device)
{
	lock();
	util_strlcpy(L.path, path, sizeof L.path);
	util_strlcpy(L.open_req, path, sizeof L.open_req);
	util_strlcpy(L.device, device, sizeof L.device);
	L.on = true;
	L.last = util_ms();
	free(L.text);
	L.text = NULL;
	L.ok = false;
	while (L.qn) {
		free(L.q[L.qh].text);
		L.qh = (L.qh + 1) % QMAX, L.qn--;
	}
	unlock();
}

void live_close(void)
{
	lock();
	L.on = false;
	unlock();
}

bool live_push(const char *path, int i, int d, const char *text, int cur)
{
	lock();
	bool ok = L.on && !strcmp(path, L.path) && L.qn < QMAX;
	if (ok) {
		LiveOp *o = &L.q[(L.qh + L.qn) % QMAX];
		o->i = i, o->d = d, o->cur = cur;
		o->text = text ? strdup(text) : NULL;
		L.qn++;
		L.last = util_ms();
	}
	unlock();
	return ok;
}

void live_request_save(void)
{
	lock();
	L.save_req = true;
	unlock();
}

bool live_want_save(void)
{
	lock();
	bool r = L.save_req;
	L.save_req = false;
	unlock();
	return r;
}

bool live_take_open(char *path, size_t n)
{
	lock();
	bool r = L.open_req[0];
	if (r) {
		util_strlcpy(path, L.open_req, n);
		L.open_req[0] = 0;
	}
	unlock();
	return r;
}

bool live_pop(LiveOp *op)
{
	lock();
	bool r = L.qn > 0;
	if (r) {
		*op = L.q[L.qh];
		L.qh = (L.qh + 1) % QMAX, L.qn--;
	}
	unlock();
	return r;
}

void live_publish(const char *path, const char *text, int cur, bool dirty, bool ok)
{
	lock();
	if (L.on && !strcmp(path, L.path)) {
		if (!L.text || strcmp(L.text, text) || L.cur != cur || L.dirty != dirty || L.ok != ok) {
			free(L.text);
			L.text = strdup(text);
			L.cur = cur, L.dirty = dirty, L.ok = ok;
			L.rev++;
		}
	}
	unlock();
}

bool live_active(char *device, size_t n)
{
	lock();
	if (L.on && util_ms() - L.last > 60000) // la web se fue sin despedirse
		L.on = false;
	bool r = L.on;
	if (r && device)
		util_strlcpy(device, L.device, n);
	unlock();
	return r;
}

char *live_get(u32 since, u32 *rev, int *cur, bool *dirty, bool *ok, char *path, size_t pn)
{
	lock();
	L.last = util_ms();
	char *r = NULL;
	*rev = L.rev, *cur = L.cur, *dirty = L.dirty, *ok = L.ok;
	util_strlcpy(path, L.on ? L.path : "", pn);
	if (L.on && L.text && L.rev != since)
		r = strdup(L.text);
	unlock();
	return r;
}
