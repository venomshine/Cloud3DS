/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "zipjob.h"
#include "fsx.h"
#include "ops.h"
#include "state.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
	ZipJob j;
	Thread thr;
	volatile bool cancel;
	volatile u64 done;
	char zip[700], dest[700], base[700], device[48], tmp[720];
	char (*names)[256];
	int n, xid;
} Z;

static void finish(int r, const char *evname)
{
	state_lock();
	xfer_progress(Z.xid, Z.done);
	xfer_end(Z.xid, r == ZIP_OK ? XS_DONE : r == ZIP_CANCELED ? XS_CANCELED : XS_FAILED);
	if (r == ZIP_OK)
		event_add(Z.j.create ? EV_ZIP : EV_UNZIP, evname, Z.device, Z.j.total);
	state_unlock();
	Z.j.done = Z.done;
	Z.j.err = r;
	Z.j.st = r == ZIP_OK ? ZJ_DONE : ZJ_FAIL;
}

/* el progreso pasa a la transferencia cada poco, desde el mismo hilo */
static void tick(void)
{
	static u64 last;
	if (util_ms() - last < 200)
		return;
	last = util_ms();
	Z.j.done = Z.done;
	state_lock();
	xfer_progress(Z.xid, Z.done);
	state_unlock();
}

static void extract_thread(void *arg)
{
	(void)arg;
	ZipDir z;
	int r = zip_read_dir(Z.zip, &z);
	if (r == ZIP_OK) {
		ZipProgress pr = {&Z.cancel, &Z.done};
		r = zip_extract(Z.zip, &z, Z.dest, &pr);
		zip_dir_free(&z);
	}
	const char *nm = strrchr(Z.zip, '/');
	finish(r, nm ? nm + 1 : Z.zip);
}

static void create_thread(void *arg)
{
	(void)arg;
	ZipProgress pr = {&Z.cancel, &Z.done};
	int r = zip_create(Z.tmp, Z.base, Z.names, Z.n, &pr);
	if (r == ZIP_OK) { // listo: se le da su nombre (se escribió aparte, así nunca queda uno a medias)
		char a[740], b[720];
		snprintf(a, sizeof a, "sdmc:%s", Z.tmp);
		snprintf(b, sizeof b, "sdmc:%s", Z.j.result);
		if (rename(a, b) != 0) {
			remove(a);
			r = ZIP_IO;
		}
	}
	free(Z.names);
	Z.names = NULL;
	const char *nm = strrchr(Z.j.result, '/');
	finish(r, nm ? nm + 1 : Z.j.result);
}

static void join_prev(void)
{
	if (Z.thr) {
		threadJoin(Z.thr, U64_MAX);
		threadFree(Z.thr);
		Z.thr = NULL;
	}
}

static int launch(ThreadFunc fn, const char *xname, u64 total, XferKind kind)
{
	Z.cancel = false;
	Z.done = 0;
	Z.j.done = 0;
	Z.j.total = total;
	Z.j.err = 0;
	Z.j.id++;
	state_lock();
	Z.xid = xfer_begin(kind, xname, Z.device, total);
	state_unlock();
	Z.j.st = ZJ_RUN;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Z.thr = threadCreate(fn, NULL, 32 * 1024, prio + 1, -2, false);
	if (!Z.thr) {
		finish(ZIP_IO, xname);
		return ZIP_IO;
	}
	return ZIP_OK;
}

int zipjob_extract(const char *zip, bool new_dir, const char *device)
{
	if (Z.j.st == ZJ_RUN)
		return ZIP_BUSY;
	join_prev();
	ZipDir z;
	int r = zip_read_dir(zip, &z);
	if (r != ZIP_OK)
		return r;
	u64 total = z.usize;
	bool bad = z.encrypted || z.unsupported;
	zip_dir_free(&z);
	if (bad)
		return ZIP_UNSUP;
	u64 fr = 0, tot = 0;
	if (fsx_space(&fr, &tot) && fr < total + 1024 * 1024)
		return ZIP_NO_SPACE;
	util_strlcpy(Z.zip, zip, sizeof Z.zip);
	util_strlcpy(Z.device, device, sizeof Z.device);
	char dir[700];
	util_strlcpy(dir, zip, sizeof dir);
	char *sl = strrchr(dir, '/');
	if (sl == dir)
		dir[1] = 0;
	else if (sl)
		*sl = 0;
	if (new_dir) { // carpeta nueva con el nombre del ZIP (sin .zip); si ya existe, «x (2)»
		char nm[256];
		util_strlcpy(nm, sl ? sl + 1 : zip, sizeof nm);
		char *dot = strrchr(nm, '.');
		if (dot && dot != nm)
			*dot = 0;
		ops_free_name(Z.dest, sizeof Z.dest, dir, nm);
	} else
		util_strlcpy(Z.dest, dir, sizeof Z.dest);
	Z.j.create = false;
	util_strlcpy(Z.j.result, Z.dest, sizeof Z.j.result);
	return launch(extract_thread, sl ? sl + 1 : zip, total, XF_UNZIP);
}

int zipjob_create(const char *base, char (*names)[256], int n, const char *zipname, const char *device)
{
	if (Z.j.st == ZJ_RUN)
		return ZIP_BUSY;
	join_prev();
	u64 total;
	int cnt;
	int r = zip_plan_size(base, names, n, &total, &cnt);
	if (r != ZIP_OK)
		return r;
	if (total >= 0xF0000000ull)
		return ZIP_TOO_BIG;
	Z.names = malloc(n * sizeof *Z.names);
	if (!Z.names)
		return ZIP_IO;
	memcpy(Z.names, names, n * sizeof *Z.names);
	Z.n = n;
	util_strlcpy(Z.base, base, sizeof Z.base);
	util_strlcpy(Z.device, device, sizeof Z.device);
	ops_free_name(Z.j.result, sizeof Z.j.result, base, zipname);
	snprintf(Z.tmp, sizeof Z.tmp, "%s%s.c3d_zip%08lx.part", base, strcmp(base, "/") ? "/" : "", (unsigned long)util_ms());
	Z.j.create = true;
	const char *nm = strrchr(Z.j.result, '/');
	return launch(create_thread, nm ? nm + 1 : Z.j.result, total, XF_ZIP);
}

void zipjob_get(ZipJob *out)
{
	if (Z.j.st == ZJ_RUN)
		tick();
	*out = Z.j;
	out->done = Z.done;
}

void zipjob_cancel(void)
{
	Z.cancel = true;
}
