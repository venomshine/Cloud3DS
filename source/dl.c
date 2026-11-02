/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "dl.h"
#include "cJSON.h"
#include "digest.h"
#include "fsx.h"
#include "install.h"
#include "ops.h"
#include "state.h"
#include "util.h"
#include "zipjob.h"

#include <ctype.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CA_BUNDLE "romfs:/certs/cacert.pem"
#define UA "Cloud3DS/" APP_VERSION " (Nintendo 3DS; libcurl)"

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	DlItem it[DL_MAX];
	int n;
	u32 next, rev;
	volatile u32 cur;           /* la que se está bajando */
	volatile bool pause, cancel; /* para la actual */
	bool ready;
} D;

/* strcasestr no está en newlib */
static const char *ci_find(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++)
		if (!strncasecmp(h, n, l))
			return h;
	return NULL;
}

static void lock(void) { LightLock_Lock(&D.lock); }
static void unlock(void) { LightLock_Unlock(&D.lock); }

static DlItem *find(u32 id)
{
	for (int i = 0; i < D.n; i++)
		if (D.it[i].id == id)
			return &D.it[i];
	return NULL;
}

static void part_path(u32 id, const char *dir, char *out, size_t n)
{
	snprintf(out, n, "%s%s.c3d_dl%08lx.part", dir, strcmp(dir, "/") ? "/" : "", (unsigned long)id);
}

bool dl_url_ok(const char *url)
{
	size_t l = strlen(url);
	if (l < 10 || l >= sizeof D.it[0].url)
		return false;
	if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))
		return false;
	for (const char *p = url; *p; p++)
		if ((unsigned char)*p <= ' ')
			return false;
	return true;
}

static void host_of(const char *url, char *out, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t k = strcspn(p, "/:?#");
	snprintf(out, n, "%.*s", (int)(k < n - 1 ? k : n - 1), p);
}

/* lo último de la ruta de un enlace, sin ?… ni #…, con los %xx traducidos y limpio para FAT */
static void name_from_url(const char *url, char *out, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	const char *path = strchr(p, '/');
	char tmp[512] = "";
	if (path) {
		size_t k = strcspn(path, "?#");
		const char *s = path + k;
		while (s > path && s[-1] == '/')
			s--;
		const char *b = s;
		while (b > path && b[-1] != '/')
			b--;
		snprintf(tmp, sizeof tmp, "%.*s", (int)(s - b), b);
		util_url_decode(tmp);
	}
	size_t o = 0;
	for (const char *q = tmp; *q && o + 1 < n; q++)
		out[o++] = (unsigned char)*q < 32 || strchr("\\/:*?\"<>|", *q) ? '_' : *q;
	out[o] = 0;
	while (o && (out[o - 1] == '.' || out[o - 1] == ' '))
		out[--o] = 0;
}

int dl_add(const char *url, const char *dir, const char *hash, int after, const char *device, u32 *id)
{
	if (!D.ready || !dl_url_ok(url) || !fsx_path_ok(dir))
		return -1;
	char h[80] = "";
	size_t hn = 0;
	for (const char *p = hash ? hash : ""; *p && hn < 64; p++)
		if (isxdigit((unsigned char)*p))
			h[hn++] = (char)tolower((unsigned char)*p);
	h[hn] = 0;
	if (hn && hn != 32 && hn != 64)
		return -2;
	lock();
	if (D.n == DL_MAX) { // llena: se van las terminadas más viejas
		for (int i = 0; i < D.n && D.n == DL_MAX; i++)
			if (D.it[i].st == DL_DONE || D.it[i].st == DL_CANCELED) {
				memmove(&D.it[i], &D.it[i + 1], (D.n - i - 1) * sizeof D.it[0]);
				D.n--;
				i--;
			}
	}
	if (D.n == DL_MAX) {
		unlock();
		return -3;
	}
	DlItem *it = &D.it[D.n++];
	memset(it, 0, sizeof *it);
	it->id = ++D.next;
	it->st = DL_QUEUED;
	it->hash_ok = -1;
	it->after = after;
	util_strlcpy(it->url, url, sizeof it->url);
	util_strlcpy(it->dir, dir, sizeof it->dir);
	util_strlcpy(it->hash, h, sizeof it->hash);
	util_strlcpy(it->device, device ? device : "", sizeof it->device);
	host_of(url, it->host, sizeof it->host);
	name_from_url(url, it->name, sizeof it->name);
	if (id)
		*id = it->id;
	D.rev++;
	unlock();
	LightEvent_Signal(&D.ev);
	return 0;
}

int dl_list(DlItem *out, int max)
{
	lock();
	int n = D.n < max ? D.n : max;
	memcpy(out, D.it, n * sizeof *out);
	unlock();
	return n;
}

u32 dl_rev(void)
{
	return D.rev;
}

int dl_active(void)
{
	int k = 0;
	lock();
	for (int i = 0; i < D.n; i++)
		k += D.it[i].st == DL_QUEUED || D.it[i].st == DL_RUN;
	unlock();
	return k;
}

int dl_op(u32 id, int op)
{
	lock();
	DlItem *it = find(id);
	int r = it ? 0 : -1;
	if (it) {
		char part[700];
		part_path(it->id, it->dir, part, sizeof part);
		switch (op) {
		case DL_PAUSE:
			if (it->st == DL_RUN && D.cur == id)
				D.pause = true;
			else if (it->st == DL_QUEUED)
				it->st = DL_PAUSED;
			break;
		case DL_RESUME:
			if (it->st == DL_PAUSED || it->st == DL_FAIL) {
				it->st = DL_QUEUED;
				it->err[0] = 0;
			}
			break;
		case DL_CANCEL:
			if (it->st == DL_RUN && D.cur == id)
				D.cancel = true;
			else if (it->st == DL_QUEUED || it->st == DL_PAUSED || it->st == DL_FAIL) {
				it->st = DL_CANCELED;
				char sp[720];
				snprintf(sp, sizeof sp, "sdmc:%s", part);
				remove(sp);
			}
			break;
		case DL_REMOVE:
			if (it->st == DL_RUN)
				r = -2;
			else {
				if (it->st != DL_DONE) {
					char sp[720];
					snprintf(sp, sizeof sp, "sdmc:%s", part);
					remove(sp);
				}
				int i = (int)(it - D.it);
				memmove(&D.it[i], &D.it[i + 1], (D.n - i - 1) * sizeof D.it[0]);
				D.n--;
			}
			break;
		}
		D.rev++;
	}
	unlock();
	LightEvent_Signal(&D.ev);
	return r;
}

/* ---------------------------------------------------------------- GitHub */

typedef struct {
	char *p;
	size_t n, max;
} Mem;

static size_t mem_cb(char *d, size_t s, size_t m, void *u)
{
	Mem *b = u;
	size_t k = s * m;
	if (b->n + k > b->max)
		return 0;
	char *np = realloc(b->p, b->n + k + 1);
	if (!np)
		return 0;
	memcpy(np + b->n, d, k);
	b->p = np, b->n += k, b->p[b->n] = 0;
	return k;
}

static int quit_cb(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)u, (void)a, (void)b, (void)c, (void)d;
	return D.quit || D.cancel;
}

/* más búfer de recepción en el socket: con el de serie, la consola pide los datos a sorbos */
static int sockopt_cb(void *u, curl_socket_t fd, curlsocktype purpose)
{
	(void)u, (void)purpose;
	int sz = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	return CURL_SOCKOPT_OK;
}

static void common_opts(CURL *c, char *eb)
{
	curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
	curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE); /* raíces de Mozilla, siempre verificado */
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024);
}

static const char *curl_err(CURLcode r, long code)
{
	if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT || r == CURLE_OPERATION_TIMEDOUT)
		return "offline";
	if (r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CONNECT_ERROR || r == CURLE_SSL_CACERT_BADFILE)
		return "tls";
	if (r != CURLE_OK)
		return "net";
	if (code == 404 || code == 410)
		return "not_found";
	if (code == 403 || code == 429)
		return "denied";
	if (code >= 400)
		return "http";
	return NULL;
}

/* github.com/dueño/repo[/releases[/latest|/tag/x]] → el .cia de esa release
 * (los enlaces directos, /releases/download/…, se bajan tal cual) */
static const char *github_resolve(DlItem *it, char *url, size_t un, char *name, size_t nn)
{
	const char *p = NULL;
	if (!strncasecmp(it->url, "https://github.com/", 19))
		p = it->url + 19;
	else if (!strncasecmp(it->url, "http://github.com/", 18))
		p = it->url + 18;
	if (!p || strstr(p, "/releases/download/") || strstr(p, "/raw/") || strstr(p, "/archive/") || strstr(p, "/blob/"))
		return NULL;
	char owner[100], repo[100], rest[300] = "";
	if (sscanf(p, "%99[^/]/%99[^/?#]%299s", owner, repo, rest) < 2)
		return NULL;
	size_t rl = strlen(repo);
	if (rl > 4 && !strcmp(repo + rl - 4, ".git"))
		repo[rl - 4] = 0;
	char api[400];
	const char *tag = strstr(rest, "/releases/tag/");
	if (tag) {
		char t[120];
		snprintf(t, sizeof t, "%s", tag + 14);
		t[strcspn(t, "/?#")] = 0;
		snprintf(api, sizeof api, "https://api.github.com/repos/%s/%s/releases/tags/%s", owner, repo, t);
	} else if (!rest[0] || rest[0] == '?' || rest[0] == '#' || !strncmp(rest, "/releases", 9))
		snprintf(api, sizeof api, "https://api.github.com/repos/%s/%s/releases/latest", owner, repo);
	else
		return NULL; // otra página de GitHub: se baja tal cual
	CURL *c = curl_easy_init();
	if (!c)
		return "net";
	char eb[CURL_ERROR_SIZE] = "";
	Mem m = {NULL, 0, 512 * 1024};
	struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	common_opts(c, eb);
	curl_easy_setopt(c, CURLOPT_URL, api);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, quit_cb);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	util_log("dl: github %s -> curl %d, HTTP %ld %s", api, (int)r, code, eb);
	const char *e = curl_err(r, code);
	if (e) {
		free(m.p);
		return e;
	}
	cJSON *j = cJSON_Parse(m.p ? m.p : "");
	free(m.p);
	const cJSON *a, *assets = cJSON_GetObjectItemCaseSensitive(j, "assets");
	bool found = false;
	cJSON_ArrayForEach(a, assets)
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name"), *u = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
		if (!cJSON_IsString(n) || !cJSON_IsString(u))
			continue;
		size_t l = strlen(n->valuestring);
		if (l > 4 && !strcasecmp(n->valuestring + l - 4, ".cia")) {
			util_strlcpy(url, u->valuestring, un);
			util_strlcpy(name, n->valuestring, nn);
			found = true;
			break;
		}
	}
	cJSON_Delete(j);
	return found ? NULL : "no_cia";
}

/* ---------------------------------------------------------------- bajar */

typedef struct {
	DlItem *snap;      /* copia de trabajo (fuera del cerrojo) */
	char part[700];
	FILE *f;
	u64 base;          /* lo que ya había (al reanudar) */
	long status;
	bool ranged;       /* el servidor respondió 206 */
	bool accept_ranges;
	char cd_name[256]; /* de Content-Disposition */
	int xid;
	u64 t_last, d_last, speed;
	bool no_space;
} Job;

static void cd_filename(const char *v, char *out, size_t n)
{
	const char *p = ci_find(v, "filename*=");
	char tmp[400] = "";
	if (p) {
		p += 10;
		const char *q = strstr(p, "''");
		p = q ? q + 2 : p;
		snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, ";\r\n"), p);
		util_url_decode(tmp);
	} else if ((p = ci_find(v, "filename="))) {
		p += 9;
		if (*p == '"') {
			p++;
			snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, "\"\r\n"), p);
		} else
			snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, ";\r\n"), p);
	}
	const char *b = strrchr(tmp, '/');
	b = b ? b + 1 : tmp;
	const char *b2 = strrchr(b, '\\');
	b = b2 ? b2 + 1 : b;
	size_t o = 0;
	for (; *b && o + 1 < n; b++)
		out[o++] = (unsigned char)*b < 32 || strchr("\\/:*?\"<>|", *b) ? '_' : *b;
	out[o] = 0;
	while (o && (out[o - 1] == '.' || out[o - 1] == ' '))
		out[--o] = 0;
}

static size_t head_cb(char *d, size_t s, size_t m, void *u)
{
	Job *j = u;
	size_t k = s * m;
	char line[512];
	snprintf(line, sizeof line, "%.*s", (int)(k < sizeof line - 1 ? k : sizeof line - 1), d);
	if (!strncasecmp(line, "HTTP/", 5)) { // empieza otra respuesta (redirección): se olvida lo anterior
		const char *sp = strchr(line, ' ');
		j->status = sp ? strtol(sp + 1, NULL, 10) : 0;
		j->accept_ranges = false;
		j->cd_name[0] = 0;
	} else if (!strncasecmp(line, "Content-Disposition:", 20))
		cd_filename(line + 20, j->cd_name, sizeof j->cd_name);
	else if (!strncasecmp(line, "Accept-Ranges:", 14) && ci_find(line + 14, "bytes"))
		j->accept_ranges = true;
	else if (!strncasecmp(line, "Content-Range:", 14))
		j->accept_ranges = true;
	return k;
}

static size_t body_cb(char *d, size_t s, size_t m, void *u)
{
	Job *j = u;
	size_t k = s * m;
	if (!j->f) { // primeros datos: 206 sigue donde se quedó; 200 empieza de cero
		char sp[720];
		snprintf(sp, sizeof sp, "sdmc:%s", j->part);
		j->ranged = j->status == 206 && j->base > 0;
		if (!j->ranged)
			j->base = 0;
		j->f = fopen(sp, j->ranged ? "ab" : "wb");
		if (!j->f)
			return 0;
		setvbuf(j->f, NULL, _IOFBF, 256 * 1024); // a la SD en bloques grandes
	}
	return fwrite(d, 1, k, j->f) == k ? k : 0;
}

static int prog_cb(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)ut, (void)un;
	Job *j = u;
	if (j->status >= 200 && j->status < 300 && dt > 0) {
		u64 total = (j->status == 206 ? j->base : 0) + (u64)dt;
		if (j->snap->total != total) {
			j->snap->total = total;
			state_lock();
			Xfer *x = xfer_get(j->xid);
			if (x)
				x->total = total;
			state_unlock();
			u64 fr = 0, tot = 0, have = (j->status == 206 ? j->base : 0) + (u64)dn;
			if (fsx_space(&fr, &tot) && fr < total - have + 512 * 1024) {
				j->no_space = true;
				return 1;
			}
		}
	}
	u64 done = (j->ranged || (j->status == 206) ? j->base : 0) + (u64)dn;
	u64 now = util_ms();
	if (now - j->t_last >= 500) {
		u64 dd = done > j->d_last ? done - j->d_last : 0;
		u64 sp = dd * 1000 / (now - j->t_last ? now - j->t_last : 1);
		j->speed = j->speed ? (j->speed * 3 + sp) / 4 : sp;
		j->t_last = now, j->d_last = done;
		lock();
		DlItem *it = find(j->snap->id);
		if (it) {
			it->done = done, it->total = j->snap->total, it->speed = j->speed;
			D.rev++;
		}
		unlock();
		state_lock();
		xfer_progress(j->xid, done);
		state_unlock();
	}
	return D.quit || D.pause || D.cancel;
}

/* comprueba el MD5 o SHA-256 del archivo; 1 coincide, 0 no */
static int check_hash(const char *path, const char *want)
{
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", path);
	FILE *f = fopen(sp, "rb");
	u8 *b = malloc(64 * 1024);
	if (!f || !b) {
		if (f)
			fclose(f);
		free(b);
		return 0;
	}
	bool md5 = strlen(want) == 32;
	Md5 m;
	Sha256 s;
	md5_init(&m);
	sha256_init(&s);
	size_t n;
	while ((n = fread(b, 1, 64 * 1024, f)) > 0 && !D.quit)
		md5 ? md5_update(&m, b, n) : sha256_update(&s, b, n);
	fclose(f);
	free(b);
	u8 d[32];
	char hex[65];
	if (md5) {
		md5_final(&m, d);
		digest_hex(d, 16, hex);
	} else {
		sha256_final(&s, d);
		digest_hex(d, 32, hex);
	}
	return !strcmp(hex, want);
}

static void set_state(u32 id, int st, const char *err, const DlItem *snap)
{
	lock();
	DlItem *it = find(id);
	if (it) {
		it->st = st;
		util_strlcpy(it->err, err ? err : "", sizeof it->err);
		if (snap) {
			it->done = snap->done, it->total = snap->total, it->speed = 0, it->resumable = snap->resumable;
			util_strlcpy(it->name, snap->name, sizeof it->name);
			util_strlcpy(it->path, snap->path, sizeof it->path);
			it->hash_ok = snap->hash_ok;
			it->after_done = snap->after_done;
		}
	}
	D.rev++;
	unlock();
}

static void run(DlItem *s)
{
	Job j = {0};
	j.snap = s;
	part_path(s->id, s->dir, j.part, sizeof j.part);
	char url[1024], gname[256] = "";
	util_strlcpy(url, s->url, sizeof url);
	const char *e = github_resolve(s, url, sizeof url, gname, sizeof gname);
	if (e) {
		set_state(s->id, DL_FAIL, e, NULL);
		return;
	}
	if (gname[0]) {
		util_strlcpy(s->name, gname, sizeof s->name);
		set_state(s->id, DL_RUN, NULL, s);
	}
	if (fsx_mkdirs(s->dir) < 0) {
		set_state(s->id, DL_FAIL, "io", NULL);
		return;
	}
	bool d;
	u64 have = 0;
	if (s->resumable && fsx_stat(j.part, &d, &have) == 0 && !d)
		j.base = have;
	state_lock();
	j.xid = xfer_begin(XF_FETCH, s->name[0] ? s->name : s->host, s->host, s->total);
	state_unlock();
	j.t_last = util_ms();
	j.d_last = j.base;
	CURL *c = curl_easy_init();
	char eb[CURL_ERROR_SIZE] = "";
	CURLcode r = CURLE_FAILED_INIT;
	long code = 0;
	if (c) {
		common_opts(c, eb);
		curl_easy_setopt(c, CURLOPT_URL, url);
		curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, head_cb);
		curl_easy_setopt(c, CURLOPT_HEADERDATA, &j);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_cb);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, &j);
		curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prog_cb);
		curl_easy_setopt(c, CURLOPT_XFERINFODATA, &j);
		curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
		if (j.base)
			curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)j.base);
		r = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		char *eff = NULL;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
		if (!j.cd_name[0] && eff && !gname[0]) // el nombre del enlace final (tras las redirecciones)
			name_from_url(eff, j.cd_name, sizeof j.cd_name);
		curl_easy_cleanup(c);
	}
	bool wrote_ok = true;
	if (j.f)
		wrote_ok = fclose(j.f) == 0;
	j.f = NULL;
	s->resumable = j.accept_ranges;
	u64 got = j.base;
	{
		bool dd;
		u64 sz;
		if (fsx_stat(j.part, &dd, &sz) == 0)
			got = sz;
	}
	s->done = got;
	util_log("dl: %s -> curl %d, HTTP %ld, %llu bytes %s", url, (int)r, code, (unsigned long long)got, eb);
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", j.part);
	int xs = XS_FAILED;
	if (D.cancel) {
		remove(sp);
		set_state(s->id, DL_CANCELED, NULL, s);
		xs = XS_CANCELED;
	} else if (D.pause) {
		set_state(s->id, DL_PAUSED, NULL, s);
		xs = XS_PAUSED;
	} else if (j.no_space) {
		remove(sp);
		set_state(s->id, DL_FAIL, "no_space", s);
	} else if (r != CURLE_OK || !wrote_ok || (code && (code < 200 || code >= 300))) {
		const char *er = !wrote_ok || r == CURLE_WRITE_ERROR ? "io" : curl_err(r == CURLE_HTTP_RETURNED_ERROR ? CURLE_OK : r, code);
		if (!s->resumable)
			remove(sp);
		set_state(s->id, DL_FAIL, er ? er : "net", s);
	} else {
		// listo: su nombre (si ya existe, «x (2)»), la comprobación y lo de después
		char nm[256];
		util_strlcpy(nm, gname[0] ? gname : j.cd_name[0] ? j.cd_name : s->name[0] ? s->name : "descarga", sizeof nm);
		if (!fsx_name_ok(nm))
			util_strlcpy(nm, "descarga", sizeof nm);
		util_strlcpy(s->name, nm, sizeof s->name);
		ops_free_name(s->path, sizeof s->path, s->dir, nm);
		char fp[720];
		snprintf(fp, sizeof fp, "sdmc:%s", s->path);
		if (rename(sp, fp) != 0) {
			remove(sp);
			set_state(s->id, DL_FAIL, "io", s);
		} else {
			s->total = got;
			if (s->hash[0])
				s->hash_ok = check_hash(s->path, s->hash);
			xs = XS_DONE;
			state_lock();
			event_add(EV_FETCH, nm, s->host, got);
			state_unlock();
			size_t l = strlen(nm);
			bool cia = l > 4 && !strcasecmp(nm + l - 4, ".cia"), zip = l > 4 && !strcasecmp(nm + l - 4, ".zip");
			if (s->hash_ok != 0 && (s->after & DLA_INSTALL) && cia && strlen(s->path) < 256) {
				char err[24] = "";
				if (ins_request(s->path, s->device[0] ? s->device : s->host, true, err, sizeof err) < 0)
					util_strlcpy(s->err, err, sizeof s->err);
				s->after_done = true;
			} else if (s->hash_ok != 0 && (s->after & DLA_UNZIP) && zip) {
				for (int k = 0; k < 600 && !D.quit; k++) { // si hay otro ZIP en marcha, se espera a que acabe
					int zr = zipjob_extract(s->path, false, s->device[0] ? s->device : s->host);
					if (zr != ZIP_BUSY) {
						if (zr != ZIP_OK)
							util_strlcpy(s->err, "unzip", sizeof s->err);
						break;
					}
					svcSleepThread(1000 * 1000000ll);
				}
				s->after_done = true;
			}
			set_state(s->id, DL_DONE, s->err[0] ? s->err : NULL, s);
		}
	}
	state_lock();
	xfer_progress(j.xid, got);
	xfer_end(j.xid, xs);
	state_unlock();
}

static void dl_thread(void *arg)
{
	(void)arg;
	while (!D.quit) {
		DlItem s;
		bool have = false;
		lock();
		for (int i = 0; i < D.n; i++)
			if (D.it[i].st == DL_QUEUED) {
				D.it[i].st = DL_RUN;
				D.it[i].err[0] = 0;
				s = D.it[i];
				have = true;
				D.cur = s.id;
				D.pause = D.cancel = false;
				D.rev++;
				break;
			}
		unlock();
		if (!have) {
			LightEvent_WaitTimeout(&D.ev, 1000 * 1000000ll);
			continue;
		}
		if (!util_ip()) { // sin Wi-Fi: se espera un poco antes de darlo por perdido
			for (int k = 0; k < 20 && !util_ip() && !D.quit; k++)
				svcSleepThread(500 * 1000000ll);
		}
		if (!util_ip())
			set_state(s.id, DL_FAIL, "offline", NULL);
		else
			run(&s);
		D.cur = 0;
	}
}

void dl_init(void)
{
	LightLock_Init(&D.lock);
	LightEvent_Init(&D.ev, RESET_ONESHOT);
	D.ready = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// como el servidor: en New 3DS, al núcleo 2; si no, en el de la app pero por delante de la interfaz
	// (casi siempre espera datos de la red; con menos prioridad, cada fotograma le quitaba la CPU y el TLS iba a sorbos)
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	D.thr = n3ds ? threadCreate(dl_thread, NULL, 64 * 1024, prio - 1, 2, false) : NULL;
	if (!D.thr)
		D.thr = threadCreate(dl_thread, NULL, 64 * 1024, prio - 1, 0, false);
}

void dl_exit(void)
{
	D.quit = true;
	LightEvent_Signal(&D.ev);
	if (D.thr) {
		threadJoin(D.thr, 5000000000ull);
		threadFree(D.thr);
		D.thr = NULL;
	}
}
