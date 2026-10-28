/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "ops.h"
#include "fsx.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COPY_BUF (256 * 1024)

/* ---------- utilidades ---------- */

static void join(char *out, size_t n, const char *dir, const char *name)
{
	snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

typedef struct {
	char (*names)[256];
	bool *dirs;
	int n, cap;
} Names;

static bool collect(const FsxEntry *e, void *user)
{
	Names *l = user;
	if (l->n == l->cap) {
		int nc = l->cap ? l->cap * 2 : 32;
		void *a = realloc(l->names, nc * sizeof *l->names), *b = realloc(l->dirs, nc * sizeof *l->dirs);
		if (a)
			l->names = a;
		if (b)
			l->dirs = b;
		if (!a || !b)
			return false;
		l->cap = nc;
	}
	util_strlcpy(l->names[l->n], e->name, sizeof l->names[0]);
	l->dirs[l->n++] = e->dir;
	return true;
}

/* nombres de una carpeta copiados antes de tocarla (moverlos cambia la lista) */
static int list_names(const char *dir, Names *l)
{
	memset(l, 0, sizeof *l);
	return fsx_list(dir, false, collect, l);
}

static void names_free(Names *l)
{
	free(l->names);
	free(l->dirs);
}

bool ops_inside(const char *src, const char *dst)
{
	size_t n = strlen(src);
	return strncasecmp(src, dst, n) == 0 && (dst[n] == '/' || dst[n] == 0);
}

void ops_free_name(char *out, size_t n, const char *dir, const char *name)
{
	const char *dot = strrchr(name, '.');
	int bl = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
	const char *ext = dot && dot != name ? dot : "";
	bool d;
	u64 s;
	for (int k = 2; k < 1000; k++) {
		char nm[256];
		snprintf(nm, sizeof nm, "%.*s (%d)%s", bl, name, k, ext);
		join(out, n, dir, nm);
		if (fsx_stat(out, &d, &s) < 0)
			return;
	}
}

/* ---------- mover ---------- */

int ops_move(const char *src, const char *dst)
{
	bool sd, dd;
	u64 sz;
	if (fsx_stat(src, &sd, &sz) < 0)
		return OPS_NOT_FOUND;
	char a[1100], b[1100];
	if (fsx_stat(dst, &dd, &sz) < 0 || strcasecmp(src, dst) == 0) {
		snprintf(a, sizeof a, "sdmc:%s", src);
		snprintf(b, sizeof b, "sdmc:%s", dst);
		return rename(a, b) == 0 ? OPS_OK : OPS_IO;
	}
	if (sd != dd)
		return OPS_TYPE;
	if (!sd) { /* archivo que sustituye a otro */
		if (fsx_rmrf(dst) < 0)
			return OPS_IO;
		return ops_move(src, dst);
	}
	/* carpeta sobre carpeta: se combinan, y lo de dentro sustituye a lo que se llame igual */
	Names l;
	if (list_names(src, &l) < 0)
		return OPS_IO;
	int r = OPS_OK;
	for (int i = 0; i < l.n && r == OPS_OK; i++) {
		char s2[1024], d2[1024];
		join(s2, sizeof s2, src, l.names[i]);
		join(d2, sizeof d2, dst, l.names[i]);
		r = ops_move(s2, d2);
	}
	names_free(&l);
	if (r == OPS_OK)
		fsx_rmrf(src);
	return r;
}

/* ---------- copiar (en su propio hilo) ---------- */

static struct {
	LightLock lock;
	bool init;
	Thread thr;
	volatile int st; /* OpsState */
	int err;
	u64 total, done;
	char src[1024], dst[1024], device[48];
	int xid;
	volatile bool cancel;
} J;

static u64 tree_size(const char *p)
{
	bool d;
	u64 sz;
	if (fsx_stat(p, &d, &sz) < 0)
		return 0;
	if (!d)
		return sz;
	Names l;
	u64 t = 0;
	if (list_names(p, &l) >= 0)
		for (int i = 0; i < l.n; i++) {
			char c[1024];
			join(c, sizeof c, p, l.names[i]);
			t += tree_size(c);
		}
	names_free(&l);
	return t;
}

static void progress(u64 add)
{
	J.done += add;
	state_lock();
	xfer_progress(J.xid, J.done);
	state_unlock();
}

/* un archivo: a un temporal .part y luego rename, para no dejar copias a medias */
static int copy_file(const char *src, const char *dst, u8 *buf)
{
	char a[1100], b[1100], tmp[1100], dir[1024];
	snprintf(a, sizeof a, "sdmc:%s", src);
	snprintf(b, sizeof b, "sdmc:%s", dst);
	util_strlcpy(dir, dst, sizeof dir);
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	snprintf(tmp, sizeof tmp, "sdmc:%s/.c3d_%08lx.part", dir, (unsigned long)(svcGetSystemTick() & 0xFFFFFFFF));
	FILE *in = fopen(a, "rb");
	if (!in)
		return OPS_NOT_FOUND;
	FILE *out = fopen(tmp, "wb");
	if (!out) {
		fclose(in);
		return OPS_IO;
	}
	int r = OPS_OK;
	size_t n;
	while ((n = fread(buf, 1, COPY_BUF, in)) > 0) {
		if (J.cancel) {
			r = OPS_CANCELED;
			break;
		}
		if (fwrite(buf, 1, n, out) != n) {
			r = OPS_NO_SPACE;
			break;
		}
		progress(n);
	}
	if (r == OPS_OK && ferror(in))
		r = OPS_IO;
	fclose(in);
	if (fclose(out) != 0 && r == OPS_OK)
		r = OPS_IO;
	if (r == OPS_OK) {
		bool d;
		u64 s;
		if (fsx_stat(dst, &d, &s) == 0 && fsx_rmrf(dst) < 0)
			r = OPS_IO;
		else if (rename(tmp, b) != 0)
			r = OPS_IO;
	}
	if (r != OPS_OK)
		unlink(tmp);
	return r;
}

static int copy_tree(const char *src, const char *dst, u8 *buf)
{
	bool d;
	u64 sz;
	if (fsx_stat(src, &d, &sz) < 0)
		return OPS_NOT_FOUND;
	if (!d)
		return copy_file(src, dst, buf);
	if (fsx_mkdirs(dst) < 0)
		return OPS_IO;
	Names l;
	if (list_names(src, &l) < 0)
		return OPS_IO;
	int r = OPS_OK;
	for (int i = 0; i < l.n && r == OPS_OK; i++) {
		char s2[1024], d2[1024];
		join(s2, sizeof s2, src, l.names[i]);
		join(d2, sizeof d2, dst, l.names[i]);
		r = copy_tree(s2, d2, buf);
	}
	names_free(&l);
	return r;
}

static void copy_thread(void *arg)
{
	(void)arg;
	u8 *buf = malloc(COPY_BUF);
	int r = buf ? copy_tree(J.src, J.dst, buf) : OPS_IO;
	free(buf);
	const char *name = strrchr(J.dst, '/');
	name = name ? name + 1 : J.dst;
	state_lock();
	xfer_end(J.xid, r == OPS_OK ? XS_DONE : r == OPS_CANCELED ? XS_CANCELED : XS_FAILED);
	if (r == OPS_OK)
		event_add(EV_COPY, name, J.device, J.total);
	state_unlock();
	util_log("copiar %s -> %s: %d", J.src, J.dst, r);
	J.err = r;
	J.st = r == OPS_OK ? OPS_DONE : OPS_FAILED;
}

int ops_copy_start(const char *src, const char *dst, const char *device)
{
	if (!J.init) {
		LightLock_Init(&J.lock);
		J.init = true;
	}
	if (J.st == OPS_RUNNING)
		return OPS_BUSY;
	if (J.thr) {
		threadJoin(J.thr, U64_MAX);
		threadFree(J.thr);
		J.thr = NULL;
	}
	u64 total = tree_size(src), fr = 0, tot = 0;
	if (fsx_space(&fr, &tot) && fr < total + 512 * 1024)
		return OPS_NO_SPACE;
	util_strlcpy(J.src, src, sizeof J.src);
	util_strlcpy(J.dst, dst, sizeof J.dst);
	util_strlcpy(J.device, device, sizeof J.device);
	J.total = total;
	J.done = 0;
	J.err = 0;
	J.cancel = false;
	const char *name = strrchr(dst, '/');
	state_lock();
	J.xid = xfer_begin(XF_COPY, name ? name + 1 : dst, device, total);
	state_unlock();
	J.st = OPS_RUNNING;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	J.thr = threadCreate(copy_thread, NULL, 32 * 1024, prio + 1, -2, false);
	if (!J.thr) {
		J.st = OPS_FAILED;
		state_lock();
		xfer_end(J.xid, XS_FAILED);
		state_unlock();
		return OPS_IO;
	}
	return OPS_OK;
}

void ops_job(int *st, int *err, u64 *done, u64 *total)
{
	*st = J.st;
	*err = J.err;
	*done = J.done;
	*total = J.total;
}

void ops_cancel(void)
{
	J.cancel = true;
}

void ops_exit(void)
{
	J.cancel = true;
	if (J.thr) {
		threadJoin(J.thr, 3000000000ull);
		threadFree(J.thr);
		J.thr = NULL;
	}
}
