/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "netfile.h"
#include "dl.h"
#include "util.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#define CH (64 * 1024)         /* los trozos que se piden aparte */
#define AHEAD (1024 * 1024)    /* si lo que falta está a menos de esto de lo que se baja, se espera */

typedef struct {
	bool used;
	char name[128], url[1024];
	u8 *buf;
	u64 size;
	u8 *ok;                    /* un byte por trozo: ya está (pedido aparte) */
	volatile u64 seq;          /* lo que llegó seguido desde el principio (el primer trozo) */
	int np;                    /* en cuántas partes llega a la vez (1 a NP) */
	volatile u64 pat[4], pend[4]; /* de cada parte: por dónde va y dónde acaba */
	volatile bool done, fail, stop;
	int refs;
	u32 gen;
	Thread thr;
	u64 used_at;
} Nf;

static Nf S[NF_SLOTS];
static LightLock L;
static u32 GEN;

typedef struct {
	int slot;
	u32 gen;
	u64 pos;
} Fd;

/* ---------------------------------------------------------------- bajar
 * Como las descargas (y 3dsDL): desde 1 MB llega en NP partes a la vez, cada una con su conexión; la primera es «seq»
 * y de las demás se marcan los trozos de 64 KB que se completan */
#define NP 4

typedef struct {
	Nf *f;
	int j;
	CURL *c;
	int tries;
	bool done;
} Part;

static size_t part_cb(char *d, size_t sz, size_t nm, void *u)
{
	Part *p = u;
	Nf *f = p->f;
	size_t n = sz * nm;
	if (f->stop)
		return 0;
	u64 at = f->pat[p->j], end = f->pend[p->j];
	if (at + n > end)
		n = at >= end ? 0 : (size_t)(end - at);
	memcpy(f->buf + at, d, n);
	u64 na = at + n, start = p->j ? f->pend[p->j - 1] : 0;
	for (u64 k = at / CH;; k++) { // (los trozos de 64 KB que ya están enteros)
		u64 ce = (k + 1) * CH < f->size ? (k + 1) * CH : f->size;
		if (ce > na || k * CH >= f->size)
			break;
		if (k * CH >= start)
			f->ok[k] = 1;
	}
	f->pat[p->j] = na;
	if (!p->j)
		f->seq = na;
	return sz * nm;
}

static CURL *part_easy(Part *p)
{
	Nf *f = p->f;
	CURL *c = curl_easy_init();
	if (!c)
		return NULL;
	char rg[48];
	snprintf(rg, sizeof rg, "%llu-%llu", (unsigned long long)f->pat[p->j], (unsigned long long)(f->pend[p->j] - 1));
	curl_easy_setopt(c, CURLOPT_URL, f->url);
	curl_easy_setopt(c, CURLOPT_RANGE, rg);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // (el enlace de OneDrive ya lleva el permiso)
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, part_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, p);
	p->c = c;
	return c;
}

static void seq_thread(void *arg)
{
	Nf *f = arg;
	u64 t0 = util_ms();
	Part pt[NP];
	memset(pt, 0, sizeof pt);
	CURLM *mh = curl_multi_init();
	int running = mh ? 1 : 0;
	for (int j = 0; j < f->np && mh; j++) {
		pt[j].f = f, pt[j].j = j;
		if (!part_easy(&pt[j]) || curl_multi_add_handle(mh, pt[j].c) != CURLM_OK)
			running = 0;
	}
	bool bad = !running;
	while (running && !f->stop) {
		if (curl_multi_perform(mh, &running) != CURLM_OK)
			break;
		CURLMsg *m;
		int left;
		while ((m = curl_multi_info_read(mh, &left))) {
			if (m->msg != CURLMSG_DONE)
				continue;
			Part *p = NULL;
			for (int j = 0; j < f->np; j++)
				if (pt[j].c == m->easy_handle)
					p = &pt[j];
			if (!p)
				continue;
			long code = 0;
			curl_easy_getinfo(p->c, CURLINFO_RESPONSE_CODE, &code);
			curl_multi_remove_handle(mh, p->c);
			curl_easy_cleanup(p->c);
			p->c = NULL;
			if (f->pat[p->j] >= f->pend[p->j])
				p->done = true;
			else if (++p->tries <= 3 && part_easy(p) && curl_multi_add_handle(mh, p->c) == CURLM_OK) // (desde donde iba)
				running = 1;
			else {
				util_log("nube: %s: la parte %d no llegó (%d, HTTP %ld)", f->name, p->j, (int)m->data.result, code);
				bad = true;
			}
		}
		if (running)
			curl_multi_poll(mh, NULL, 0, 100, NULL);
	}
	for (int j = 0; j < f->np; j++)
		if (pt[j].c) {
			curl_multi_remove_handle(mh, pt[j].c);
			curl_easy_cleanup(pt[j].c);
		}
	if (mh)
		curl_multi_cleanup(mh);
	bool all = !bad && !f->stop;
	for (int j = 0; j < f->np && all; j++)
		all = pt[j].done;
	if (all) {
		f->seq = f->size, f->done = true;
		u64 el = util_ms() - t0;
		util_log("nube: %s: llegó entero (%llu bytes en %llu ms, %llu KB/s, %d conexiones)", f->name, (unsigned long long)f->size,
		         (unsigned long long)el, (unsigned long long)(f->size * 1000 / (el ? el : 1) / 1024), f->np);
	} else if (!f->stop) {
		util_log("nube: %s: no llegó entero: lo que falte se pedirá por trozos", f->name);
		f->fail = true;
	}
}

typedef struct {
	Nf *f;
	u64 at, end;
} RangeCtx;

static size_t range_cb(char *d, size_t sz, size_t nm, void *u)
{
	RangeCtx *c = u;
	size_t n = sz * nm;
	if (c->at + n > c->end)
		n = c->at >= c->end ? 0 : (size_t)(c->end - c->at);
	memcpy(c->f->buf + c->at, d, n);
	c->at += n;
	return sz * nm;
}

// un trozo, aparte (lo pide quien lee)
static bool fetch_chunk(Nf *f, u64 k)
{
	u64 a = k * CH, e = a + CH < f->size ? a + CH : f->size;
	CURL *c = curl_easy_init();
	if (!c)
		return false;
	char rg[48];
	snprintf(rg, sizeof rg, "%llu-%llu", (unsigned long long)a, (unsigned long long)(e - 1));
	RangeCtx rc = {f, a, e};
	curl_easy_setopt(c, CURLOPT_URL, f->url);
	curl_easy_setopt(c, CURLOPT_RANGE, rg);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L); // (como las descargas: el enlace de OneDrive ya lleva el permiso)
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, range_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &rc);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	bool ok = r == CURLE_OK && code == 206 && rc.at == e;
	if (ok)
		f->ok[k] = 1;
	else
		util_log("nube: %s: el trozo %llu no llegó (%d, HTTP %ld)", f->name, (unsigned long long)k, (int)r, code);
	return ok;
}

static void slot_free(Nf *f)
{
	f->stop = true;
	if (f->thr) {
		threadJoin(f->thr, 10000000000ull);
		threadFree(f->thr);
		f->thr = NULL;
	}
	free(f->buf), free(f->ok);
	memset(f, 0, sizeof *f);
}

bool nf_open_url(const char *url, u64 size, const char *name, char *path, size_t pn)
{
	if (!size || size > (64ull << 20) || strlen(url) >= sizeof S[0].url)
		return false;
	LightLock_Lock(&L);
	int k = -1;
	for (int i = 0; i < NF_SLOTS; i++) // (uno libre o, si no, el más viejo que nadie tenga abierto)
		if (!S[i].used) {
			k = i;
			break;
		} else if (!S[i].refs && (k < 0 || S[i].used_at < S[k].used_at))
			k = i;
	if (k >= 0 && S[k].used)
		slot_free(&S[k]);
	LightLock_Unlock(&L);
	if (k < 0)
		return false;
	Nf *f = &S[k];
	f->buf = malloc((size_t)size);
	f->ok = calloc((size_t)((size + CH - 1) / CH), 1);
	if (!f->buf || !f->ok) {
		free(f->buf), free(f->ok);
		f->buf = f->ok = NULL;
		util_log("nube: %s: sin memoria para %llu bytes", name, (unsigned long long)size);
		return false;
	}
	util_strlcpy(f->url, url, sizeof f->url);
	util_strlcpy(f->name, name, sizeof f->name);
	for (char *c = f->name; *c; c++)
		if (*c == '/' || *c == ':')
			*c = '_';
	f->size = size, f->seq = 0, f->done = f->fail = f->stop = false, f->refs = 0;
	f->np = size >= (1u << 20) ? NP : 1; // (pequeño: de una vez)
	u64 piece = (size / f->np + CH - 1) / CH * CH;
	for (int j = 0; j < f->np; j++) {
		f->pat[j] = (u64)j * piece < size ? (u64)j * piece : size;
		f->pend[j] = j == f->np - 1 || (u64)(j + 1) * piece >= size ? size : (u64)(j + 1) * piece;
	}
	f->gen = ++GEN, f->used_at = util_ms(), f->used = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	f->thr = threadCreate(seq_thread, f, 32 * 1024, prio + 1, -2, false);
	if (!f->thr) {
		LightLock_Lock(&L);
		slot_free(f);
		LightLock_Unlock(&L);
		return false;
	}
	snprintf(path, pn, "nube:/%d/%s", k, f->name);
	util_log("nube: abriendo %s sin bajarlo a la SD (%llu bytes)", path, (unsigned long long)size);
	return true;
}

static Nf *by_path(const char *p, int *slot)
{
	if (strncmp(p, "nube:/", 6))
		return NULL;
	int k = atoi(p + 6);
	const char *nm = strchr(p + 6, '/');
	if (k < 0 || k >= NF_SLOTS || !S[k].used || !nm || strcmp(nm + 1, S[k].name))
		return NULL;
	if (slot)
		*slot = k;
	return &S[k];
}

bool nf_is(const char *path) { return !strncmp(path, "nube:", 5); }

bool nf_progress(const char *path, u64 *got, u64 *total, bool *done, bool *failed)
{
	Nf *f = by_path(path, NULL);
	if (!f)
		return false;
	*got = f->seq, *total = f->size, *done = f->done, *failed = f->fail;
	return true;
}

/* ---------------------------------------------------------------- el disco «nube:» */

static int nf_open(struct _reent *r, void *fs, const char *path, int flags, int mode)
{
	(void)mode;
	if ((flags & O_ACCMODE) != O_RDONLY) {
		r->_errno = EROFS;
		return -1;
	}
	LightLock_Lock(&L);
	int k;
	Nf *f = by_path(path, &k);
	if (f)
		f->refs++, f->used_at = util_ms();
	LightLock_Unlock(&L);
	if (!f) {
		r->_errno = ENOENT;
		return -1;
	}
	Fd *d = fs;
	d->slot = k, d->gen = f->gen, d->pos = 0;
	return 0;
}

static int nf_close(struct _reent *r, void *fd)
{
	(void)r;
	Fd *d = fd;
	LightLock_Lock(&L);
	if (S[d->slot].used && S[d->slot].gen == d->gen && S[d->slot].refs > 0)
		S[d->slot].refs--;
	LightLock_Unlock(&L);
	return 0;
}

static ssize_t nf_read(struct _reent *r, void *fd, char *ptr, size_t len)
{
	Fd *d = fd;
	Nf *f = &S[d->slot];
	if (!f->used || f->gen != d->gen) {
		r->_errno = EIO;
		return -1;
	}
	if (d->pos >= f->size)
		return 0;
	if (len > f->size - d->pos)
		len = (size_t)(f->size - d->pos);
	size_t out = 0;
	while (out < len) {
		u64 p = d->pos, k = p / CH;
		u64 end = f->ok[k] ? ((k + 1) * CH < f->size ? (k + 1) * CH : f->size) : f->seq;
		if (p < end) { // lo que ya está: seguido o en su trozo
			size_t n = (size_t)(end - p) < len - out ? (size_t)(end - p) : len - out;
			memcpy(ptr + out, f->buf + p, n);
			out += n, d->pos += n;
			continue;
		}
		if (out) // (lo que había; el resto, en la siguiente lectura)
			break;
		if (f->fail || f->done) { // (no llegará seguido: se pide aparte)
			if (!fetch_chunk(f, k)) {
				r->_errno = EIO;
				return -1;
			}
			continue;
		}
		bool soon = false; // ¿lo trae enseguida la parte en la que cae?
		for (int j = 0; j < f->np; j++)
			if (p >= (j ? f->pend[j - 1] : 0) && p < f->pend[j] && p >= f->pat[j] && p - f->pat[j] < AHEAD)
				soon = true;
		if (!soon) { // lejos de lo que se baja: ese trozo, aparte
			if (!fetch_chunk(f, k)) {
				r->_errno = EIO;
				return -1;
			}
			continue;
		}
		svcSleepThread(20 * 1000000ll); // (llegará enseguida)
	}
	return (ssize_t)out;
}

static off_t nf_seek(struct _reent *r, void *fd, off_t pos, int dir)
{
	Fd *d = fd;
	Nf *f = &S[d->slot];
	s64 base = dir == SEEK_SET ? 0 : dir == SEEK_CUR ? (s64)d->pos : (s64)f->size;
	s64 np = base + pos;
	if (np < 0) {
		r->_errno = EINVAL;
		return -1;
	}
	d->pos = (u64)np;
	return (off_t)np;
}

static void fill_stat(const Nf *f, struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFREG | 0444;
	st->st_size = (off_t)f->size;
	st->st_nlink = 1;
}

static int nf_fstat(struct _reent *r, void *fd, struct stat *st)
{
	(void)r;
	fill_stat(&S[((Fd *)fd)->slot], st);
	return 0;
}

static int nf_stat(struct _reent *r, const char *file, struct stat *st)
{
	Nf *f = by_path(file, NULL);
	if (!f) {
		r->_errno = ENOENT;
		return -1;
	}
	fill_stat(f, st);
	return 0;
}

static ssize_t nf_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	(void)fd, (void)ptr, (void)len;
	r->_errno = EROFS;
	return -1;
}

static const devoptab_t DEV = {
	.name = "nube",
	.structSize = sizeof(Fd),
	.open_r = nf_open,
	.close_r = nf_close,
	.write_r = nf_write,
	.read_r = nf_read,
	.seek_r = nf_seek,
	.fstat_r = nf_fstat,
	.stat_r = nf_stat,
};

void nf_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	LightLock_Init(&L);
	AddDevice(&DEV);
}
