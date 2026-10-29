/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "hash.h"
#include "digest.h"
#include "fsx.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct {
	LightLock lock;
	bool init;
	HashJob j;
	Thread thr;
	volatile bool cancel;
} H;

#define CHUNK (256 * 1024)

static void lock(void)
{
	if (!H.init) {
		LightLock_Init(&H.lock);
		H.init = true;
	}
	LightLock_Lock(&H.lock);
}

/* busca hex (minúsculas) en un texto, sin distinguir mayúsculas y como palabra entera */
static bool has_word(const char *txt, const char *hex)
{
	size_t n = strlen(hex);
	for (const char *p = txt; *p; p++)
		if (!strncasecmp(p, hex, n) && (p == txt || !isxdigit((unsigned char)p[-1])) && !isxdigit((unsigned char)p[n]))
			return true;
	return false;
}

typedef struct {
	const char *dir, *name;
	HashJob *j;
} SideCtx;

/* .md5 / .sha256 (y .md5sum, .sha256sum) de la misma carpeta: si contienen el
 * valor, coincide; si nombran este archivo con otro valor, no coincide */
static bool side_cb(const FsxEntry *e, void *u)
{
	SideCtx *c = u;
	const char *x = strrchr(e->name, '.');
	if (e->dir || !x || e->size > 64 * 1024 || !strcmp(e->name, c->name))
		return true;
	bool md5 = !strcasecmp(x, ".md5") || !strcasecmp(x, ".md5sum");
	bool sha = !strcasecmp(x, ".sha256") || !strcasecmp(x, ".sha256sum");
	if (!md5 && !sha)
		return true;
	char p[800];
	snprintf(p, sizeof p, "sdmc:%s%s%s", c->dir, strcmp(c->dir, "/") ? "/" : "", e->name);
	FILE *f = fopen(p, "rb");
	if (!f)
		return true;
	char *t = malloc(e->size + 1);
	size_t n = t ? fread(t, 1, e->size, f) : 0;
	fclose(f);
	if (!t)
		return true;
	t[n] = 0;
	const char *want = md5 ? c->j->md5 : c->j->sha;
	if (has_word(t, want)) {
		c->j->check = HCHK_OK;
		util_strlcpy(c->j->check_file, e->name, sizeof c->j->check_file);
		free(t);
		return false;
	}
	// ¿habla de este archivo? («valor  nombre» o el .sha256 se llama como él)
	size_t bl = strlen(c->name);
	bool named = strstr(t, c->name) || (strlen(e->name) > bl && !strncasecmp(e->name, c->name, bl) && e->name[bl] == '.');
	if (named && c->j->check == HCHK_NONE) {
		c->j->check = HCHK_BAD;
		util_strlcpy(c->j->check_file, e->name, sizeof c->j->check_file);
	}
	free(t);
	return true;
}

static void hash_thread(void *arg)
{
	(void)arg;
	char path[512], sd[520];
	lock();
	util_strlcpy(path, H.j.path, sizeof path);
	u32 id = H.j.id;
	LightLock_Unlock(&H.lock);
	snprintf(sd, sizeof sd, "sdmc:%s", path);
	FILE *f = fopen(sd, "rb");
	u8 *buf = malloc(CHUNK);
	Md5 m;
	Sha256 s;
	md5_init(&m);
	sha256_init(&s);
	bool ok = f && buf;
	if (f)
		setvbuf(f, NULL, _IONBF, 0);
	while (ok && !H.cancel) {
		size_t n = fread(buf, 1, CHUNK, f);
		if (n == 0) {
			ok = !ferror(f);
			break;
		}
		md5_update(&m, buf, n);
		sha256_update(&s, buf, n);
		lock();
		H.j.done += n;
		LightLock_Unlock(&H.lock);
	}
	if (f)
		fclose(f);
	free(buf);
	if (H.cancel)
		return;
	HashJob r;
	lock();
	r = H.j;
	LightLock_Unlock(&H.lock);
	if (ok) {
		u8 d1[16], d2[32];
		md5_final(&m, d1);
		sha256_final(&s, d2);
		digest_hex(d1, 16, r.md5);
		digest_hex(d2, 32, r.sha);
		char dir[512];
		util_strlcpy(dir, path, sizeof dir);
		char *sl = strrchr(dir, '/');
		const char *name = sl ? sl + 1 : path;
		if (sl == dir)
			dir[1] = 0;
		else if (sl)
			*sl = 0;
		char nm[256];
		util_strlcpy(nm, name, sizeof nm);
		SideCtx c = {dir, nm, &r};
		fsx_list(dir, false, side_cb, &c);
	}
	lock();
	if (H.j.id == id) {
		r.st = ok ? HASH_DONE : HASH_ERR;
		r.done = H.j.done;
		H.j = r;
	}
	LightLock_Unlock(&H.lock);
}

void hash_cancel(void)
{
	if (!H.thr)
		return;
	H.cancel = true;
	threadJoin(H.thr, U64_MAX);
	threadFree(H.thr);
	H.thr = NULL;
	H.cancel = false;
	lock();
	if (H.j.st == HASH_RUN)
		H.j.st = HASH_IDLE;
	LightLock_Unlock(&H.lock);
}

void hash_start(const char *path)
{
	bool d;
	u64 sz;
	if (fsx_stat(path, &d, &sz) < 0 || d)
		return;
	u64 mt = fsx_mtime(path);
	lock();
	bool same = !strcmp(H.j.path, path) && H.j.size == sz && H.j.mtime == mt && (H.j.st == HASH_RUN || H.j.st == HASH_DONE);
	LightLock_Unlock(&H.lock);
	if (same)
		return;
	hash_cancel();
	lock();
	u32 id = H.j.id + 1;
	memset(&H.j, 0, sizeof H.j);
	H.j.id = id;
	H.j.st = HASH_RUN;
	util_strlcpy(H.j.path, path, sizeof H.j.path);
	H.j.size = sz, H.j.mtime = mt;
	LightLock_Unlock(&H.lock);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	H.thr = threadCreate(hash_thread, NULL, 24 * 1024, prio + 2, -2, false);
	if (!H.thr) {
		lock();
		H.j.st = HASH_ERR;
		LightLock_Unlock(&H.lock);
	}
}

void hash_get(HashJob *out)
{
	lock();
	*out = H.j;
	LightLock_Unlock(&H.lock);
}

int hash_compare(const HashJob *j, const char *expected)
{
	char e[80];
	size_t n = 0;
	for (const char *p = expected; *p && n < sizeof e - 1; p++) // se queda solo con las cifras (pegado con espacios, «MD5: …»…)
		if (isxdigit((unsigned char)*p))
			e[n++] = (char)tolower((unsigned char)*p);
		else if (n && n != 32 && n != 64)
			n = 0; // lo de antes no era el valor
		else if (n)
			break;
	e[n] = 0;
	if (n == 32)
		return !strcmp(e, j->md5) ? HCHK_OK : HCHK_BAD;
	if (n == 64)
		return !strcmp(e, j->sha) ? HCHK_OK : HCHK_BAD;
	return HCHK_NONE;
}

int hash_save_sha256(const HashJob *j)
{
	if (j->st != HASH_DONE)
		return -1;
	const char *nm = strrchr(j->path, '/');
	nm = nm ? nm + 1 : j->path;
	char p[600];
	snprintf(p, sizeof p, "sdmc:%s.sha256", j->path);
	FILE *f = fopen(p, "wb");
	if (!f)
		return -1;
	int r = fprintf(f, "%s  %s\n", j->sha, nm) > 0 ? 0 : -1;
	return fclose(f) == 0 ? r : -1;
}
