/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "sc.h"
#include "cJSON.h"
#include "dl.h"
#include "fsx.h"
#include "img.h"
#include "schls.h"
#include "util.h"

#include <neaacdec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_FILE APP_DIR "/soundcloud.json"
#define API "https://api-v2.soundcloud.com"
#define UA_WEB "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"

/* ---------------------------------------------------------------- HTTP (en el hilo que llama) */

typedef struct {
	char *p;
	size_t n, cap, max;
} Buf;

static size_t buf_wr(char *d, size_t sz, size_t nm, void *u)
{
	Buf *b = u;
	size_t k = sz * nm;
	if (b->n + k + 1 > b->max)
		return 0;
	if (b->n + k + 1 > b->cap) {
		size_t c = b->cap ? b->cap : 16 * 1024;
		while (c < b->n + k + 1)
			c *= 2;
		char *q = realloc(b->p, c);
		if (!q)
			return 0;
		b->p = q, b->cap = c;
	}
	memcpy(b->p + b->n, d, k);
	b->n += k;
	b->p[b->n] = 0;
	return k;
}

// GET a memoria (sin comprobar el certificado: todo es público). code: el estado HTTP; NULL si no conectó
static char *get(const char *url, size_t max, long *code, size_t *len)
{
	Buf b = {NULL, 0, 0, max};
	long c = 0;
	dl_set_thread_noverify(true);
	dl_set_thread_ua(UA_WEB);
	int r = dl_fetch(url, "*/*", buf_wr, NULL, &b, &c);
	if (r != DLF_OK || (!b.n && c < 300)) { // httpc no pudo (o llegó vacío): con libcurl
		util_log("soundcloud: httpc %d (HTTP %ld) en %.60s: con libcurl", r, c, url);
		c = 0;
		free(b.p);
		b = (Buf){NULL, 0, 0, max};
		r = dl_fetch_curl(url, "*/*", buf_wr, &b, &c);
	}
	dl_set_thread_ua(NULL);
	if (code)
		*code = c;
	if (r != DLF_OK || !b.p) {
		free(b.p);
		return NULL;
	}
	if (len)
		*len = b.n;
	return b.p;
}

/* ---------------------------------------------------------------- el identificador de cliente (de la web) */

static LightLock CL;
static char CID[40];
static bool cl_init;

static void cid_save(void)
{
	FILE *f = fopen(SC_FILE, "w");
	if (f)
		fprintf(f, "{\"client_id\":\"%s\"}", CID), fclose(f);
}

static void cid_load(void)
{
	FILE *f = fopen(SC_FILE, "r");
	char b[256] = "";
	if (f) {
		size_t n = fread(b, 1, sizeof b - 1, f);
		b[n] = 0;
		fclose(f);
	}
	const char *p = strstr(b, "\"client_id\":\"");
	if (p) {
		p += 13;
		const char *e = strchr(p, '"');
		if (e && e - p < (int)sizeof CID)
			snprintf(CID, sizeof CID, "%.*s", (int)(e - p), p);
	}
}

// el que usa la web de SoundCloud: en uno de sus scripts (los últimos de la página), «client_id:"…"»
static bool cid_scrape(void)
{
	long code;
	char *html = get("https://soundcloud.com/", 2u << 20, &code, NULL);
	if (!html) {
		util_log("soundcloud: no se pudo abrir su web (HTTP %ld)", code);
		return false;
	}
	char urls[24][160];
	int n = 0;
	for (const char *p = html; (p = strstr(p, "https://a-v2.sndcdn.com/assets/")) && n < 24; p++) {
		const char *e = strchr(p, '"');
		if (e && e - p < 160 && !strncmp(e - 3, ".js", 3))
			snprintf(urls[n++], sizeof urls[0], "%.*s", (int)(e - p), p);
	}
	free(html);
	bool ok = false;
	for (int i = n - 1; i >= 0 && !ok; i--) {
		char *js = get(urls[i], 8u << 20, &code, NULL);
		const char *p = js ? strstr(js, "client_id:\"") : NULL;
		if (p) {
			p += 11;
			const char *e = strchr(p, '"');
			if (e && e - p > 10 && e - p < (int)sizeof CID) {
				LightLock_Lock(&CL);
				snprintf(CID, sizeof CID, "%.*s", (int)(e - p), p);
				LightLock_Unlock(&CL);
				ok = true;
			}
		}
		free(js);
	}
	util_log("soundcloud: identificador de la web %s", ok ? "renovado" : "no encontrado");
	if (ok)
		cid_save();
	return ok;
}

// GET a la API con el identificador (si no vale, se renueva una vez). JSON o NULL (err)
static cJSON *api(const char *path, char *err, size_t en)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		LightLock_Lock(&CL);
		bool have = CID[0];
		LightLock_Unlock(&CL);
		if (!have && !cid_scrape()) {
			snprintf(err, en, "noid");
			return NULL;
		}
		char url[1400];
		LightLock_Lock(&CL);
		snprintf(url, sizeof url, "%s%s%s%sclient_id=%s", strncmp(path, "http", 4) ? API : "", path, strchr(path, '?') ? "&" : "?", "", CID);
		LightLock_Unlock(&CL);
		long code = 0;
		char *b = get(url, 4u << 20, &code, NULL);
		if (code == 401 || code == 403) { // el identificador caducó: otro de la web
			free(b);
			LightLock_Lock(&CL);
			CID[0] = 0;
			LightLock_Unlock(&CL);
			continue;
		}
		if (!b || code >= 400) {
			free(b);
			snprintf(err, en, b || code ? "http" : "offline");
			util_log("soundcloud: %.80s -> HTTP %ld", path, code);
			return NULL;
		}
		cJSON *j = cJSON_Parse(b);
		free(b);
		if (!j)
			snprintf(err, en, "json");
		return j;
	}
	snprintf(err, en, "noid");
	return NULL;
}

/* la descarga en curso (abajo) */
static struct {
	Thread thr;
	volatile bool busy, stop;
	volatile int st, pct; // st: SCD_*
	ScTrack t;
	char path[300];
} D;

/* ---------------------------------------------------------------- canciones */

static const char *js(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

// la transcodificación AAC (HLS) completa de más calidad; si solo hay la de 30 s, esa
static bool parse_track(const cJSON *t, ScTrack *o)
{
	memset(o, 0, sizeof *o);
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
	if (!cJSON_IsNumber(id))
		return false;
	snprintf(o->id, sizeof o->id, "%.0f", id->valuedouble);
	const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(t, "media"), "transcodings"), *tc;
	int best = 0;
	cJSON_ArrayForEach(tc, tcs)
	{
		const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(tc, "format");
		const char *proto = js(fmt, "protocol"), *mime = js(fmt, "mime_type"), *url = js(tc, "url"), *preset = js(tc, "preset");
		if (!proto || !mime || !url || strcmp(proto, "hls") || strncmp(mime, "audio/mp4", 9))
			continue;
		bool snip = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(tc, "snipped"));
		int score = (snip ? 1 : 10) + (preset && strstr(preset, "160") ? 2 : 1);
		if (score > best && strlen(url) < sizeof o->tc) {
			best = score;
			util_strlcpy(o->tc, url, sizeof o->tc);
			o->preview = snip;
		}
	}
	if (!best)
		return false;
	const cJSON *user = cJSON_GetObjectItemCaseSensitive(t, "user");
	util_strlcpy(o->title, js(t, "title") ? js(t, "title") : "?", sizeof o->title);
	util_strlcpy(o->artist, js(user, "username") ? js(user, "username") : "", sizeof o->artist);
	const cJSON *d = cJSON_GetObjectItemCaseSensitive(t, "full_duration");
	if (!cJSON_IsNumber(d))
		d = cJSON_GetObjectItemCaseSensitive(t, "duration");
	o->dur_ms = cJSON_IsNumber(d) ? (u32)d->valuedouble : 0;
	const char *art = js(t, "artwork_url");
	if (!art)
		art = js(user, "avatar_url");
	if (art)
		util_strlcpy(o->art, art, sizeof o->art);
	const char *au = js(t, "track_authorization");
	if (au && strlen(au) < sizeof o->auth)
		util_strlcpy(o->auth, au, sizeof o->auth);
	o->downloadable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "downloadable")) &&
	                  !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(t, "has_downloads_left"));
	return true;
}

// las canciones vistas, para el reproductor (las últimas 200)
#define REG_N 200
static ScTrack *REG;
static int reg_next;

static void reg_put(const ScTrack *t)
{
	LightLock_Lock(&CL);
	if (!REG)
		REG = calloc(REG_N, sizeof *REG);
	if (REG) {
		int k = 0;
		while (k < REG_N && strcmp(REG[k].id, t->id))
			k++;
		if (k == REG_N)
			k = reg_next, reg_next = (reg_next + 1) % REG_N;
		REG[k] = *t;
	}
	LightLock_Unlock(&CL);
}

bool sc_track(const char *id, ScTrack *out)
{
	bool ok = false;
	LightLock_Lock(&CL);
	for (int k = 0; REG && k < REG_N && !ok; k++)
		if (!strcmp(REG[k].id, id))
			*out = REG[k], ok = true;
	LightLock_Unlock(&CL);
	return ok;
}

/* ---------------------------------------------------------------- búsquedas (su hilo) */

static struct {
	Thread thr;
	LightEvent ev;
	volatile bool quit;
	int job; // 1 buscar, 2 abrir una playlist
	char q[200], list_title[128];
	ScTrack t[SC_MAX];
	ScList l[SC_MAX];
	int nt, nl, st;
	char err[32];
	u32 rev;
} Q;

static void pct(const char *in, char *out, size_t n)
{
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
			out[o++] = (char)*p;
		else
			o += (size_t)snprintf(out + o, n - o, "%%%02X", *p);
	}
	out[o] = 0;
}

static void do_search(const char *q)
{
	char e[200], path[300], err[32] = "";
	pct(q, e, sizeof e);
	static ScTrack t[SC_MAX];
	static ScList l[SC_MAX];
	int nt = 0, nl = 0;
	snprintf(path, sizeof path, "/search/tracks?q=%s&limit=40&linked_partitioning=1", e);
	cJSON *j = api(path, err, sizeof err), *it;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "collection"))
	{
		if (nt < SC_MAX && parse_track(it, &t[nt]))
			reg_put(&t[nt]), nt++;
	}
	cJSON_Delete(j);
	if (!err[0] || nt) {
		snprintf(path, sizeof path, "/search/playlists_without_albums?q=%s&limit=15", e);
		char err2[32] = "";
		j = api(path, err2, sizeof err2);
		cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "collection"))
		{
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id"), *tc = cJSON_GetObjectItemCaseSensitive(it, "track_count");
			if (nl >= SC_MAX || !cJSON_IsNumber(id))
				continue;
			ScList *o = &l[nl++];
			memset(o, 0, sizeof *o);
			snprintf(o->id, sizeof o->id, "%.0f", id->valuedouble);
			util_strlcpy(o->title, js(it, "title") ? js(it, "title") : "?", sizeof o->title);
			const char *u = js(cJSON_GetObjectItemCaseSensitive(it, "user"), "username");
			util_strlcpy(o->user, u ? u : "", sizeof o->user);
			o->count = cJSON_IsNumber(tc) ? tc->valueint : 0;
		}
		cJSON_Delete(j);
	}
	if (!Q.quit) {
		memcpy(Q.t, t, sizeof t[0] * nt);
		memcpy(Q.l, l, sizeof l[0] * nl);
		Q.nt = nt, Q.nl = nl;
		util_strlcpy(Q.err, err, sizeof Q.err);
		Q.st = nt || nl || !err[0] ? SCQ_DONE : SCQ_ERR;
		Q.rev++;
	}
	util_log("soundcloud: «%s»: %d canciones, %d playlists %s", q, nt, nl, err);
}

static void do_list(const char *id)
{
	char path[160], err[32] = "";
	static ScTrack t[SC_MAX];
	int nt = 0;
	snprintf(path, sizeof path, "/playlists/%s", id);
	cJSON *j = api(path, err, sizeof err), *it;
	// las primeras vienen enteras; las demás, solo con su número: se piden de 50 en 50
	char ids[SC_MAX][24];
	int nids = 0;
	cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "tracks"))
	{
		if (nt + nids >= SC_MAX)
			break;
		if (cJSON_GetObjectItemCaseSensitive(it, "media")) {
			if (parse_track(it, &t[nt]))
				reg_put(&t[nt]), nt++;
		} else {
			const cJSON *tid = cJSON_GetObjectItemCaseSensitive(it, "id");
			if (cJSON_IsNumber(tid))
				snprintf(ids[nids++], sizeof ids[0], "%.0f", tid->valuedouble);
		}
	}
	cJSON_Delete(j);
	for (int k = 0; k < nids && !Q.quit; k += 50) {
		char q[1400] = "/tracks?ids=";
		for (int i = k; i < nids && i < k + 50; i++) {
			if (i > k)
				strcat(q, "%2C");
			strcat(q, ids[i]);
		}
		char err2[32];
		cJSON *a = api(q, err2, sizeof err2);
		cJSON_ArrayForEach(it, a)
		{
			if (nt < SC_MAX && parse_track(it, &t[nt]))
				reg_put(&t[nt]), nt++;
		}
		cJSON_Delete(a);
	}
	if (!Q.quit) {
		memcpy(Q.t, t, sizeof t[0] * nt);
		Q.nt = nt, Q.nl = 0;
		util_strlcpy(Q.err, err, sizeof Q.err);
		Q.st = nt || !err[0] ? SCQ_DONE : SCQ_ERR;
		Q.rev++;
	}
}

static void thread_main(void *arg)
{
	(void)arg;
	while (!Q.quit) {
		LightEvent_Wait(&Q.ev);
		for (;;) {
			LightLock_Lock(&CL);
			int job = Q.job;
			char q[200];
			util_strlcpy(q, Q.q, sizeof q);
			Q.job = 0;
			LightLock_Unlock(&CL);
			if (Q.quit || !job)
				break;
			if (job == 1)
				do_search(q);
			else if (job == 2)
				do_list(q);
		}
	}
}

void sc_init(void)
{
	if (cl_init)
		return;
	cl_init = true;
	LightLock_Init(&CL);
	LightEvent_Init(&Q.ev, RESET_ONESHOT);
	cid_load();
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Q.thr = threadCreate(thread_main, NULL, 48 * 1024, prio + 1, -2, false);
}

void sc_exit(void)
{
	if (D.thr) {
		D.stop = true;
		threadJoin(D.thr, 5000000000ull);
		threadFree(D.thr);
		D.thr = NULL;
	}
	if (!Q.thr)
		return;
	Q.quit = true;
	LightEvent_Signal(&Q.ev);
	threadJoin(Q.thr, 3000000000ull);
	threadFree(Q.thr);
	Q.thr = NULL;
}

static void push(int job, const char *q)
{
	sc_init();
	LightLock_Lock(&CL);
	Q.job = job;
	util_strlcpy(Q.q, q, sizeof Q.q);
	Q.st = SCQ_BUSY, Q.rev++;
	LightLock_Unlock(&CL);
	LightEvent_Signal(&Q.ev);
}

void sc_search(const char *q) { push(1, q); }
void sc_open_list(const char *id, const char *title)
{
	util_strlcpy(Q.list_title, title ? title : "", sizeof Q.list_title);
	push(2, id);
}

u32 sc_results(ScTrack *t, int *nt, ScList *l, int *nl, int *st, char *err, size_t en)
{
	if (!cl_init) {
		*nt = *nl = 0, *st = SCQ_IDLE;
		return 0;
	}
	LightLock_Lock(&CL);
	if (t)
		memcpy(t, Q.t, sizeof *t * Q.nt);
	if (l)
		memcpy(l, Q.l, sizeof *l * Q.nl);
	*nt = Q.nt, *nl = Q.nl, *st = Q.st;
	util_strlcpy(err, Q.err, en);
	u32 r = Q.rev;
	LightLock_Unlock(&CL);
	return r;
}

/* ================================================================ streaming */

#define QMAX 4 // trozos en cola (unos 40 s)

typedef struct {
	u8 *data;
	size_t len;
	size_t *off; // tramas
	u32 *flen;
	int n, at;
} Seg;

struct ScStream {
	ScTrack t;
	Thread thr;
	LightLock lk;
	volatile bool stop;
	volatile int st; // 0 preparando, 1 listo, -1 error
	char err[32];
	Fmp4Cfg cfg;
	u32 dur_ms;
	Seg q[QMAX];
	int qh, qn;
	volatile bool eof;
	NeAACDecHandle dec;
	bool dec_ok;
	s16 pend[4096]; // lo que sobró de la última trama decodificada (muestras de 16 bits, estéreo)
	int npend, pend_at;
	u8 *cover;
	int cw, ch;
	volatile bool cover_ready;
};

typedef struct {
	Seg *s;
	int cap;
} FrCtx;

static bool fr_cb(const uint8_t *f, size_t len, void *u)
{
	FrCtx *c = u;
	Seg *s = c->s;
	if (s->n == c->cap) {
		int nc = c->cap ? c->cap * 2 : 512;
		size_t *o = realloc(s->off, sizeof *o * nc);
		u32 *l = o ? realloc(s->flen, sizeof *l * nc) : NULL;
		if (o)
			s->off = o;
		if (!l)
			return false;
		s->flen = l, c->cap = nc;
	}
	s->off[s->n] = (size_t)(f - s->data), s->flen[s->n] = (u32)len, s->n++;
	return true;
}

static void seg_free(Seg *s)
{
	free(s->data), free(s->off), free(s->flen);
	memset(s, 0, sizeof *s);
}

// la carátula (JPEG) de 300×300: la «large» de la API es de 100×100
static u8 *art_get(const ScTrack *t, size_t *n)
{
	if (!t->art[0])
		return NULL;
	char url[300];
	util_strlcpy(url, t->art, sizeof url);
	char *lg = strstr(url, "-large.");
	if (lg && strlen(url) + 8 < sizeof url) {
		char tail[80];
		util_strlcpy(tail, lg + 6, sizeof tail);
		snprintf(lg, sizeof url - (size_t)(lg - url), "-t300x300%s", tail);
	}
	long code;
	return (u8 *)get(url, 1u << 20, &code, n);
}

static void cover_fetch(ScStream *s)
{
	size_t n = 0;
	u8 *b = art_get(&s->t, &n);
	u8 *rgba = NULL;
	int w, h;
	if (b && img_mem_fit(b, n, 160, 160, &rgba, &w, &h) == IMG_OK) {
		LightLock_Lock(&s->lk);
		s->cover = rgba, s->cw = w, s->ch = h, s->cover_ready = true;
		LightLock_Unlock(&s->lk);
	}
	free(b);
}

static void fail(ScStream *s, const char *e)
{
	util_strlcpy(s->err, e, sizeof s->err);
	s->st = -1;
	util_log("soundcloud: %s: %s", s->t.id, e);
}

// la lista HLS de una canción (la de medios) y la configuración de su AAC. err: por qué no
static bool hls_open(const ScTrack *t, HlsList *pl, Fmp4Cfg *cfg, char *err, size_t en)
{
	char path[1300];
	snprintf(path, sizeof path, "%s%s%s", t->tc, t->auth[0] ? "?track_authorization=" : "", t->auth);
	cJSON *j = api(path, err, en);
	const char *u = js(j, "url");
	char m3u[1500] = "";
	if (u)
		util_strlcpy(m3u, u, sizeof m3u);
	cJSON_Delete(j);
	if (!m3u[0]) {
		if (!err[0])
			snprintf(err, en, "stream");
		return false;
	}
	long code;
	char *txt = get(m3u, 1u << 20, &code, NULL);
	bool ok = txt && hls_parse(txt, m3u, pl);
	free(txt);
	if (ok && pl->master) { // (una lista maestra: la de medios)
		char v[1500];
		util_strlcpy(v, pl->variant, sizeof v);
		hls_free(pl);
		txt = get(v, 1u << 20, &code, NULL);
		ok = txt && hls_parse(txt, v, pl);
		free(txt);
	}
	if (!ok || !pl->init) {
		if (ok)
			hls_free(pl);
		snprintf(err, en, "playlist");
		return false;
	}
	size_t n = 0;
	u8 *init = (u8 *)get(pl->init, 1u << 20, &code, &n);
	ok = init && fmp4_init(init, n, cfg);
	free(init);
	if (!ok) {
		hls_free(pl);
		snprintf(err, en, "format");
	}
	return ok;
}

static void stream_thread(void *arg)
{
	ScStream *s = arg;
	dl_set_thread_stop(&s->stop, NULL);
	char err[32] = "";
	HlsList pl;
	long code;
	size_t n = 0;
	if (!hls_open(&s->t, &pl, &s->cfg, err, sizeof err)) {
		fail(s, err);
		return;
	}
	s->dec = NeAACDecOpen();
	NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(s->dec);
	c->outputFormat = FAAD_FMT_16BIT;
	c->downMatrix = 1; // (más de dos canales: a estéreo)
	NeAACDecSetConfiguration(s->dec, c);
	unsigned long rate;
	unsigned char ch;
	if (NeAACDecInit2(s->dec, s->cfg.asc, (unsigned long)s->cfg.asc_len, &rate, &ch) < 0) {
		hls_free(&pl);
		fail(s, "format");
		return;
	}
	s->cfg.rate = (int)rate, s->cfg.chans = ch, s->dec_ok = true;
	s->dur_ms = pl.total_ms ? pl.total_ms : s->t.dur_ms;
	s->st = 1;
	util_log("soundcloud: %s: %d trozos, %u ms, %d Hz", s->t.id, pl.n, (unsigned)s->dur_ms, s->cfg.rate);
	for (int i = 0; i < pl.n && !s->stop; i++) {
		if (i == 1)
			cover_fetch(s); // (después del primer trozo: que empiece a sonar cuanto antes)
		while (!s->stop) { // hueco en la cola
			LightLock_Lock(&s->lk);
			bool room = s->qn < QMAX;
			LightLock_Unlock(&s->lk);
			if (room)
				break;
			svcSleepThread(50 * 1000000ll);
		}
		Seg sg = {0};
		u8 *d = NULL;
		for (int tries = 0; tries < 3 && !d && !s->stop; tries++)
			d = (u8 *)get(pl.seg[i].uri, 8u << 20, &code, &n);
		if (!d) {
			if (!s->stop)
				util_log("soundcloud: %s: no llegó el trozo %d", s->t.id, i);
			break;
		}
		sg.data = d, sg.len = n;
		FrCtx fc = {&sg, 0};
		if (fmp4_frames(d, n, &s->cfg, fr_cb, &fc) <= 0) {
			seg_free(&sg);
			continue;
		}
		LightLock_Lock(&s->lk);
		s->q[(s->qh + s->qn) % QMAX] = sg;
		s->qn++;
		LightLock_Unlock(&s->lk);
	}
	hls_free(&pl);
	s->eof = true;
}

ScStream *sc_stream_open(const char *id)
{
	ScStream *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	if (!sc_track(id, &s->t)) {
		free(s);
		return NULL;
	}
	LightLock_Init(&s->lk);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s->thr = threadCreate(stream_thread, s, 48 * 1024, prio + 1, -2, false); // (la red, por detrás del audio)
	if (!s->thr) {
		free(s);
		return NULL;
	}
	return s;
}

int sc_stream_ready(ScStream *s, int *rate, int *chans, u32 *dur_ms, char *err, size_t en)
{
	if (s->st == 1) {
		*rate = s->cfg.rate, *chans = 2, *dur_ms = s->dur_ms;
		return 1;
	}
	if (s->st < 0)
		util_strlcpy(err, s->err, en);
	return s->st;
}

int sc_stream_read(ScStream *s, s16 *out, int frames)
{
	int got = 0; // muestras estéreo
	while (got < frames) {
		if (s->pend_at < s->npend) { // lo que sobró de la última trama
			int k = (s->npend - s->pend_at) / 2;
			if (k > frames - got)
				k = frames - got;
			memcpy(out + got * 2, s->pend + s->pend_at, (size_t)k * 4);
			s->pend_at += k * 2, got += k;
			continue;
		}
		LightLock_Lock(&s->lk);
		Seg *sg = s->qn ? &s->q[s->qh] : NULL;
		LightLock_Unlock(&s->lk);
		if (!sg) {
			if (got)
				return got;
			return s->eof ? 0 : -1;
		}
		if (sg->at >= sg->n) { // trozo acabado: fuera de la cola
			LightLock_Lock(&s->lk);
			seg_free(sg);
			s->qh = (s->qh + 1) % QMAX, s->qn--;
			LightLock_Unlock(&s->lk);
			continue;
		}
		NeAACDecFrameInfo fi;
		s16 *pcm = NeAACDecDecode(s->dec, &fi, sg->data + sg->off[sg->at], sg->flen[sg->at]);
		sg->at++;
		if (fi.error || !pcm || !fi.samples)
			continue;
		int ch = fi.channels ? fi.channels : 2, n = (int)(fi.samples / ch);
		if (n > 2048)
			n = 2048;
		for (int i = 0; i < n; i++) { // a estéreo
			s->pend[i * 2] = pcm[i * ch];
			s->pend[i * 2 + 1] = ch > 1 ? pcm[i * ch + 1] : pcm[i * ch];
		}
		s->npend = n * 2, s->pend_at = 0;
	}
	return got;
}

bool sc_stream_cover(ScStream *s, u8 **rgba, int *w, int *h)
{
	if (!s->cover_ready)
		return false;
	LightLock_Lock(&s->lk);
	*rgba = s->cover, *w = s->cw, *h = s->ch;
	s->cover = NULL, s->cover_ready = false;
	LightLock_Unlock(&s->lk);
	return *rgba != NULL;
}

void sc_stream_close(ScStream *s)
{
	if (!s)
		return;
	s->stop = true;
	if (s->thr) {
		threadJoin(s->thr, 10000000000ull);
		threadFree(s->thr);
	}
	for (int i = 0; i < QMAX; i++)
		seg_free(&s->q[i]);
	if (s->dec)
		NeAACDecClose(s->dec);
	free(s->cover);
	free(s);
}

/* ================================================================ descargar (solo si el autor lo permite)
 * La descarga oficial (el archivo original) pide iniciar sesión; sin cuenta, se guarda el mismo audio del streaming:
 * AAC en ADTS (.aac), con una etiqueta ID3v2.4 delante (título, artista, álbum «SoundCloud», duración y carátula). */


static void id3_frame(FILE *f, const char *id, const u8 *body, u32 n)
{
	u8 h[10];
	memcpy(h, id, 4);
	h[4] = (u8)(n >> 21 & 127), h[5] = (u8)(n >> 14 & 127), h[6] = (u8)(n >> 7 & 127), h[7] = (u8)(n & 127); // (2.4: sincronizado)
	h[8] = h[9] = 0;
	fwrite(h, 1, 10, f);
	fwrite(body, 1, n, f);
}

static void id3_text(FILE *f, const char *id, const char *txt)
{
	u8 b[260];
	size_t n = strlen(txt);
	if (n > sizeof b - 2)
		n = sizeof b - 2;
	b[0] = 3; // UTF-8
	memcpy(b + 1, txt, n);
	id3_frame(f, id, b, (u32)n + 1);
}

static void id3_write(FILE *f, const ScTrack *t, const u8 *jpg, size_t jn, u32 ms)
{
	long start = ftell(f);
	u8 h[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
	fwrite(h, 1, 10, f);
	id3_text(f, "TIT2", t->title);
	id3_text(f, "TPE1", t->artist);
	id3_text(f, "TALB", "SoundCloud");
	char len[16];
	snprintf(len, sizeof len, "%lu", (unsigned long)ms);
	id3_text(f, "TLEN", len); // (la duración: en ADTS no se sabe sin recorrer todo el archivo)
	if (jpg && jn < (1u << 20)) { // APIC: codificación, tipo MIME, portada (3), sin descripción y la imagen
		static const u8 pre[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 0};
		u8 *b = malloc(sizeof pre + jn);
		if (b) {
			memcpy(b, pre, sizeof pre);
			memcpy(b + sizeof pre, jpg, jn);
			id3_frame(f, "APIC", b, (u32)(sizeof pre + jn));
			free(b);
		}
	}
	long end = ftell(f);
	u32 n = (u32)(end - start - 10);
	u8 sz[4] = {(u8)(n >> 21 & 127), (u8)(n >> 14 & 127), (u8)(n >> 7 & 127), (u8)(n & 127)};
	fseek(f, start + 6, SEEK_SET);
	fwrite(sz, 1, 4, f);
	fseek(f, end, SEEK_SET);
}

typedef struct {
	FILE *f;
	u8 hdr[7]; // la cabecera ADTS sin la longitud
	bool ok;
} AdtsCtx;

static bool adts_cb(const uint8_t *fr, size_t len, void *u)
{
	AdtsCtx *a = u;
	u32 fl = (u32)len + 7;
	u8 h[7];
	memcpy(h, a->hdr, 7);
	h[3] |= (u8)(fl >> 11 & 3);
	h[4] = (u8)(fl >> 3);
	h[5] |= (u8)((fl & 7) << 5);
	if (fwrite(h, 1, 7, a->f) != 7 || fwrite(fr, 1, len, a->f) != len)
		return a->ok = false;
	return true;
}

static void dl_thread(void *arg)
{
	(void)arg;
	dl_set_thread_stop(&D.stop, NULL);
	char err[32] = "";
	HlsList pl;
	Fmp4Cfg cfg;
	bool ok = false;
	char part[310];
	snprintf(part, sizeof part, "%s.part", D.path);
	if (hls_open(&D.t, &pl, &cfg, err, sizeof err)) {
		// de la configuración del AAC (objeto, frecuencia y canales) a la cabecera ADTS de cada trama
		int obj = cfg.asc[0] >> 3, sri = (cfg.asc[0] & 7) << 1 | cfg.asc[1] >> 7, chc = cfg.asc[1] >> 3 & 15;
		AdtsCtx a = {NULL, {0xFF, 0xF1, (u8)((obj - 1) << 6 | sri << 2 | chc >> 2), (u8)((chc & 3) << 6), 0, 0x1F, 0xFC}, true};
		char sp[320];
		snprintf(sp, sizeof sp, "sdmc:%s", part);
		a.f = obj >= 1 && obj <= 4 ? fopen(sp, "wb") : NULL;
		if (a.f) {
			size_t jn = 0;
			u8 *jpg = art_get(&D.t, &jn);
			id3_write(a.f, &D.t, jpg, jn, pl.total_ms ? pl.total_ms : D.t.dur_ms);
			free(jpg);
			ok = true;
			for (int i = 0; i < pl.n && ok && !D.stop; i++) {
				size_t n = 0;
				long code;
				u8 *d = NULL;
				for (int tries = 0; tries < 3 && !d && !D.stop; tries++)
					d = (u8 *)get(pl.seg[i].uri, 8u << 20, &code, &n);
				ok = d && fmp4_frames(d, n, &cfg, adts_cb, &a) > 0 && a.ok;
				free(d);
				D.pct = (i + 1) * 100 / pl.n;
			}
			ok = fclose(a.f) == 0 && ok && !D.stop;
		}
		hls_free(&pl);
	}
	char sp[320], dp[320];
	snprintf(sp, sizeof sp, "sdmc:%s", part);
	snprintf(dp, sizeof dp, "sdmc:%s", D.path);
	if (ok) {
		remove(dp);
		ok = rename(sp, dp) == 0;
	}
	if (!ok)
		remove(sp);
	util_log("soundcloud: descarga de %s en %s: %s", D.t.id, D.path, ok ? "hecha" : err[0] ? err : "falló");
	D.st = ok ? SCD_DONE : SCD_FAIL;
	D.busy = false;
}

int sc_download(const char *id)
{
	if (D.busy)
		return 1;
	if (D.thr) {
		threadJoin(D.thr, U64_MAX);
		threadFree(D.thr);
		D.thr = NULL;
	}
	if (!sc_track(id, &D.t) || !D.t.downloadable || D.t.preview)
		return 2;
	char nm[200];
	snprintf(nm, sizeof nm, "%s%s%s", D.t.artist, D.t.artist[0] ? " - " : "", D.t.title);
	for (char *c = nm; *c; c++) // (lo que no vale en un nombre de archivo)
		if (strchr("\\/:*?\"<>|", *c) || (unsigned char)*c < 32)
			*c = '_';
	fsx_mkdirs(SC_DL_DIR);
	snprintf(D.path, sizeof D.path, "%s/%.180s.aac", SC_DL_DIR, nm);
	D.busy = true, D.stop = false, D.pct = 0, D.st = SCD_RUN;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	D.thr = threadCreate(dl_thread, NULL, 48 * 1024, prio + 2, -2, false);
	if (!D.thr) {
		D.busy = false, D.st = SCD_FAIL;
		return 3;
	}
	return 0;
}

bool sc_download_busy(char *title, size_t n, int *pct)
{
	if (D.st != SCD_RUN)
		return false;
	util_strlcpy(title, D.t.title, n);
	*pct = D.pct;
	return true;
}

int sc_download_state(char *title, size_t n, int *pct)
{
	int st = D.st;
	if (title)
		util_strlcpy(title, D.t.title, n);
	if (pct)
		*pct = D.pct;
	if (st == SCD_DONE || st == SCD_FAIL) // (el final, una vez)
		D.st = SCD_IDLE;
	return st;
}
