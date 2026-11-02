/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Actualizaciones desde las releases de GitHub. Todo va en un hilo propio:
 * buscar consulta releases/latest; actualizar descarga el .cia (o el .3dsx si
 * se abrió desde el Homebrew Launcher), lo comprueba con el Cloud3DS.sha256
 * de la release y lo instala. Solo se ofrece una versión MAYOR que la actual. */
#include "update.h"
#include "cJSON.h"
#include "state.h"
#include "util.h"
#include "dl.h"

#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>

#ifndef UPDATE_URL
#define UPDATE_URL "https://api.github.com/repos/venomshine/Cloud3DS/releases/latest"
#endif
#define CA_BUNDLE "romfs:/certs/cacert.pem"
#define MAX_JSON  (512 * 1024)
#define MAX_FILE  (32 * 1024 * 1024)

enum { CMD_NONE, CMD_CHECK, CMD_INSTALL, CMD_FILE, CMD_QUIT };
#define SELF_TID 0x000400000C13D500ull

static struct {
	LightLock lock;
	Thread thr;
	LightEvent ev;
	volatile int cmd;
	bool quiet;
	UpdInfo i;
	char url_file[512], url_sha[512], asset[32];
	char self[256]; /* ruta del .3dsx abierto, si lo es */
	char file[300]; /* actualizar desde este archivo de la SD (CMD_FILE) */
} U;

static void set_state(int st, const char *err)
{
	LightLock_Lock(&U.lock);
	U.i.st = st;
	util_strlcpy(U.i.err, err ? err : "", sizeof U.i.err);
	U.i.rev++;
	LightLock_Unlock(&U.lock);
}

/* ---------- versiones ---------- */

static void ver_parse(const char *s, int v[3])
{
	v[0] = v[1] = v[2] = 0;
	if (*s == 'v' || *s == 'V')
		s++;
	sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

bool upd_newer(const char *a, const char *b)
{
	int x[3], y[3];
	ver_parse(a, x);
	ver_parse(b, y);
	for (int i = 0; i < 3; i++)
		if (x[i] != y[i])
			return x[i] > y[i];
	return false;
}

/* ---------- descargas ---------- */

typedef struct {
	u8 *p;
	size_t n, cap, max;
	bool progress;
} Mem;

static size_t write_cb(char *d, size_t sz, size_t nm, void *u)
{
	Mem *m = u;
	size_t n = sz * nm;
	if (m->n + n + 1 > m->max)
		return 0;
	if (m->n + n + 1 > m->cap) {
		size_t nc = m->cap ? m->cap * 2 : 64 * 1024;
		while (nc < m->n + n + 1)
			nc *= 2;
		u8 *np = realloc(m->p, nc);
		if (!np)
			return 0;
		m->p = np;
		m->cap = nc;
	}
	memcpy(m->p + m->n, d, n);
	m->n += n;
	m->p[m->n] = 0;
	if (m->progress) {
		LightLock_Lock(&U.lock);
		U.i.done = m->n;
		U.i.rev++;
		LightLock_Unlock(&U.lock);
	}
	return n;
}

static int xfer_cb(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)ut, (void)un, (void)dn;
	Mem *m = u;
	if (m->progress && dt > 0) {
		LightLock_Lock(&U.lock);
		U.i.total = (u64)dt;
		LightLock_Unlock(&U.lock);
	}
	return U.cmd == CMD_QUIT; /* salir corta la descarga */
}

static void total_cb(void *u, u64 total)
{
	Mem *m = u;
	if (m->progress) {
		LightLock_Lock(&U.lock);
		U.i.total = total;
		LightLock_Unlock(&U.lock);
	}
}

static const char *http_code_err(long code)
{
	if (code == 404)
		return "no_release";
	if (code == 403 || code == 429)
		return "rate";
	if (code != 200)
		return "http";
	return NULL;
}

/* error de curl o HTTP a un texto corto (la interfaz lo traduce). Primero con httpc (el servicio
 * del sistema, más rápido); si no conecta, con libcurl y las raíces de Mozilla */
static const char *http_get(const char *url, Mem *m, bool json)
{
	long hcode = 0;
	int hr = dl_fetch(url, json ? "application/vnd.github+json" : NULL, write_cb, total_cb, m, &hcode);
	util_log("update: GET %s -> httpc %d, HTTP %ld, %u bytes", url, hr, hcode, (unsigned)m->n);
	if (hr == DLF_OK)
		return http_code_err(hcode);
	if (hr == DLF_FAIL)
		return m->n + 1 >= m->max ? "too_big" : "net";
	m->n = 0;
	CURL *c = curl_easy_init();
	if (!c)
		return "net";
	struct curl_slist *h = NULL;
	if (json)
		h = curl_slist_append(h, "Accept: application/vnd.github+json");
	char eb[CURL_ERROR_SIZE] = "";
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "Cloud3DS/" APP_VERSION " (Nintendo 3DS; libcurl)");
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE); /* raíces de Mozilla, siempre verificado */
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
	curl_easy_setopt(c, CURLOPT_XFERINFODATA, m);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	util_log("update: GET %s -> curl %d, HTTP %ld, %u bytes %s", url, (int)r, code, (unsigned)m->n, eb);
	if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT || r == CURLE_OPERATION_TIMEDOUT)
		return "offline";
	if (r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CONNECT_ERROR || r == CURLE_SSL_CACERT_BADFILE)
		return "tls";
	if (r == CURLE_WRITE_ERROR)
		return "too_big";
	if (r != CURLE_OK)
		return "net";
	return http_code_err(code);
}

/* ---------- buscar ---------- */

static void do_check(void)
{
	set_state(UPD_CHECKING, NULL);
	/* al abrir la app la red puede tardar: se espera hasta 30 s a tener IP */
	for (int i = 0; i < 60 && !util_ip() && U.cmd != CMD_QUIT; i++)
		svcSleepThread(500 * 1000000ll);
	if (!util_ip()) {
		set_state(UPD_ERROR, "offline");
		return;
	}
	Mem m = {.max = MAX_JSON};
	const char *e = http_get(UPDATE_URL, &m, true);
	if (e) {
		free(m.p);
		set_state(UPD_ERROR, e);
		return;
	}
	cJSON *r = cJSON_Parse((const char *)m.p);
	free(m.p);
	const cJSON *tag = cJSON_GetObjectItemCaseSensitive(r, "tag_name");
	if (!cJSON_IsString(tag)) {
		cJSON_Delete(r);
		set_state(UPD_ERROR, "bad_json");
		return;
	}
	const char *want = envIsHomebrew() ? "Cloud3DS.3dsx" : "Cloud3DS.cia";
	char file[512] = "", sha[512] = "";
	const cJSON *a, *assets = cJSON_GetObjectItemCaseSensitive(r, "assets");
	cJSON_ArrayForEach(a, assets)
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name"), *u = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
		if (!cJSON_IsString(n) || !cJSON_IsString(u))
			continue;
		if (!strcmp(n->valuestring, want))
			util_strlcpy(file, u->valuestring, sizeof file);
		if (!strcmp(n->valuestring, "Cloud3DS.sha256"))
			util_strlcpy(sha, u->valuestring, sizeof sha);
	}
	const cJSON *body = cJSON_GetObjectItemCaseSensitive(r, "body");
	LightLock_Lock(&U.lock);
	util_strlcpy(U.i.latest, tag->valuestring[0] == 'v' ? tag->valuestring + 1 : tag->valuestring, sizeof U.i.latest);
	util_strlcpy(U.i.notes, cJSON_IsString(body) ? body->valuestring : "", sizeof U.i.notes);
	U.i.has_file = file[0] && sha[0];
	util_strlcpy(U.url_file, file, sizeof U.url_file);
	util_strlcpy(U.url_sha, sha, sizeof U.url_sha);
	util_strlcpy(U.asset, want, sizeof U.asset);
	LightLock_Unlock(&U.lock);
	cJSON_Delete(r);
	bool newer = upd_newer(U.i.latest, APP_VERSION);
	util_log("update: instalada %s, última %s%s", APP_VERSION, U.i.latest, newer ? " (nueva)" : "");
	set_state(newer ? UPD_AVAILABLE : UPD_LATEST, NULL);
}

/* ---------- instalar ---------- */

static bool sha_matches(const u8 *data, size_t n, const char *list, const char *asset)
{
	u8 h[32];
	mbedtls_sha256(data, n, h, 0);
	char hex[65];
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", h[i]);
	/* formato de sha256sum: «<hash>  <archivo>» por línea */
	for (const char *l = list; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (strncasecmp(l, hex, 64) == 0) {
			const char *nm = l + 64;
			while (*nm == ' ' || *nm == '*')
				nm++;
			if (!strncmp(nm, asset, strlen(asset)))
				return true;
		}
	}
	util_log("update: SHA-256 %s no coincide con la lista", hex);
	return false;
}

static const char *install_cia(const u8 *data, size_t n)
{
	if (R_FAILED(amInit()))
		return "am";
	Handle h;
	Result r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
	if (R_FAILED(r)) {
		util_log("update: AM_StartCiaInstall 0x%08lX", (unsigned long)r);
		amExit();
		return "am";
	}
	for (size_t off = 0; off < n && R_SUCCEEDED(r);) {
		u32 w = 0, chunk = n - off > 0x10000 ? 0x10000 : (u32)(n - off);
		r = FSFILE_Write(h, &w, off, data + off, chunk, 0);
		off += w;
		LightLock_Lock(&U.lock);
		U.i.done = off;
		U.i.rev++;
		LightLock_Unlock(&U.lock);
		if (!w)
			break;
	}
	if (R_FAILED(r)) {
		util_log("update: escribir el CIA 0x%08lX", (unsigned long)r);
		AM_CancelCIAInstall(h);
		amExit();
		return "am";
	}
	r = AM_FinishCiaInstall(h);
	amExit();
	if (R_FAILED(r)) {
		util_log("update: AM_FinishCiaInstall 0x%08lX", (unsigned long)r);
		return "am";
	}
	return NULL;
}

static const char *install_3dsx(const u8 *data, size_t n)
{
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.c3d_upd.part", U.self);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return "io";
	bool ok = fwrite(data, 1, n, f) == n;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		unlink(tmp);
		return "io";
	}
	unlink(U.self);
	if (rename(tmp, U.self) != 0)
		return "io";
	return NULL;
}

static void do_install(void)
{
	if (U.i.st != UPD_AVAILABLE || !U.i.has_file)
		return;
	LightLock_Lock(&U.lock);
	U.i.done = U.i.total = 0;
	LightLock_Unlock(&U.lock);
	set_state(UPD_DOWNLOADING, NULL);
	Mem sha = {.max = 64 * 1024};
	const char *e = http_get(U.url_sha, &sha, false);
	Mem m = {.max = MAX_FILE, .progress = true};
	if (!e)
		e = http_get(U.url_file, &m, false);
	if (!e && !sha_matches(m.p, m.n, (const char *)sha.p, U.asset))
		e = "sha";
	free(sha.p);
	if (!e) {
		set_state(UPD_INSTALLING, NULL);
		LightLock_Lock(&U.lock);
		U.i.total = m.n;
		U.i.done = 0;
		LightLock_Unlock(&U.lock);
		e = envIsHomebrew() ? install_3dsx(m.p, m.n) : install_cia(m.p, m.n);
	}
	free(m.p);
	util_log("update: instalar %s -> %s", U.i.latest, e ? e : "ok");
	if (e)
		set_state(UPD_ERROR, e);
	else
		set_state(UPD_DONE, NULL);
}

/* ---------- desde un archivo de la SD ----------
 * El mismo CIA (o .3dsx) de Cloud3DS que se publica: pasado al PC a la SD, o subido desde la
 * web. Del CIA manda su ID de título, no el nombre del archivo, y se instala a trozos desde la SD. */
static const char *install_cia_file(const char *path)
{
	Handle in;
	char sp[320];
	snprintf(sp, sizeof sp, "%s", path);
	FS_Path fp = fsMakePath(PATH_ASCII, sp);
	FS_Archive sd;
	if (R_FAILED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
		return "io";
	Result r = FSUSER_OpenFile(&in, sd, fp, FS_OPEN_READ, 0);
	FSUSER_CloseArchive(sd);
	if (R_FAILED(r))
		return "not_found";
	u64 size = 0;
	FSFILE_GetSize(in, &size);
	const char *e = NULL;
	if (R_FAILED(amInit())) {
		FSFILE_Close(in);
		return "am";
	}
	AM_TitleEntry te;
	if (R_FAILED(AM_GetCiaFileInfo(MEDIATYPE_SD, &te, in)))
		e = "bad_cia";
	else if (te.titleID != SELF_TID)
		e = "not_self";
	Handle h = 0;
	if (!e && R_FAILED(r = AM_StartCiaInstall(MEDIATYPE_SD, &h))) {
		util_log("update: AM_StartCiaInstall 0x%08lX", (unsigned long)r);
		e = "am";
		h = 0;
	}
	u8 *buf = e ? NULL : malloc(256 * 1024);
	if (!e && !buf)
		e = "memory";
	LightLock_Lock(&U.lock);
	U.i.total = size, U.i.done = 0;
	LightLock_Unlock(&U.lock);
	for (u64 off = 0; !e && off < size;) {
		u32 got = 0, w = 0, want = size - off > 256 * 1024 ? 256 * 1024 : (u32)(size - off);
		if (R_FAILED(FSFILE_Read(in, &got, off, buf, want)) || !got) {
			e = "io";
			break;
		}
		if (R_FAILED(r = FSFILE_Write(h, &w, off, buf, got, 0)) || w != got) {
			util_log("update: escribir 0x%08lX", (unsigned long)r);
			e = "am";
			break;
		}
		off += got;
		LightLock_Lock(&U.lock);
		U.i.done = off;
		U.i.rev++;
		LightLock_Unlock(&U.lock);
	}
	free(buf);
	FSFILE_Close(in);
	if (e && h)
		AM_CancelCIAInstall(h);
	else if (!e && R_FAILED(r = AM_FinishCiaInstall(h))) {
		util_log("update: AM_FinishCiaInstall 0x%08lX", (unsigned long)r);
		e = R_DESCRIPTION(r) == 39 ? "older" : "am";
	}
	amExit();
	return e;
}

static void do_install_file(void)
{
	char path[300];
	LightLock_Lock(&U.lock);
	util_strlcpy(path, U.file, sizeof path);
	LightLock_Unlock(&U.lock);
	set_state(UPD_INSTALLING, NULL);
	const char *e;
	if (envIsHomebrew()) { // abierto como .3dsx: se cambia el .3dsx por el elegido
		char sp[320];
		snprintf(sp, sizeof sp, "sdmc:%s", path);
		FILE *f = fopen(sp, "rb");
		u8 *d = NULL;
		long n = 0;
		if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && n < MAX_FILE && !fseek(f, 0, SEEK_SET) && (d = malloc(n)) &&
		    fread(d, 1, n, f) == (size_t)n && n > 4 && !memcmp(d, "3DSX", 4))
			e = install_3dsx(d, n);
		else
			e = "bad_file";
		if (f)
			fclose(f);
		free(d);
	} else
		e = install_cia_file(path);
	util_log("update: desde %s -> %s", path, e ? e : "ok");
	if (e)
		set_state(UPD_ERROR, e);
	else
		set_state(UPD_DONE, NULL);
}

static int scan_dir(const char *dir, UpdLocal *out, int n, int max, bool cia)
{
	char sp[320];
	snprintf(sp, sizeof sp, "sdmc:%s", dir);
	DIR *d = opendir(sp);
	if (!d)
		return n;
	struct dirent *de;
	while (n < max && (de = readdir(d))) {
		size_t l = strlen(de->d_name);
		const char *ext = cia ? ".cia" : ".3dsx";
		size_t el = strlen(ext);
		if (l <= el || strcasecmp(de->d_name + l - el, ext))
			continue;
		UpdLocal *u = &out[n];
		memset(u, 0, sizeof *u);
		snprintf(u->path, sizeof u->path, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", de->d_name);
		if (cia) { // del CIA manda su ID de título
			Handle fh;
			FS_Archive sd;
			bool ok = false;
			if (R_SUCCEEDED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
				if (R_SUCCEEDED(FSUSER_OpenFile(&fh, sd, fsMakePath(PATH_ASCII, u->path), FS_OPEN_READ, 0))) {
					AM_TitleEntry te;
					if (R_SUCCEEDED(AM_GetCiaFileInfo(MEDIATYPE_SD, &te, fh)) && te.titleID == SELF_TID) {
						snprintf(u->ver, sizeof u->ver, "%u.%u.%u", te.version >> 10, (te.version >> 4) & 0x3F, te.version & 0xF);
						ok = true;
					}
					FSFILE_Close(fh);
				}
				FSUSER_CloseArchive(sd);
			}
			if (!ok)
				continue;
		} else if (strncasecmp(de->d_name, "Cloud3DS", 8)) // un .3dsx: por el nombre
			continue;
		n++;
	}
	closedir(d);
	return n;
}

int upd_find_local(UpdLocal *out, int max)
{
	static const char *const DIRS[] = {"/", "/cias", "/CIAs", "/Descargas", "/Downloads", "/3ds", "/3ds/Cloud3DS"};
	bool cia = !envIsHomebrew();
	int n = 0;
	if (cia && R_FAILED(amInit()))
		return 0;
	for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0] && n < max; i++) {
		bool dup = false; // /cias y /CIAs pueden ser la misma
		for (size_t k = 0; k < i; k++)
			dup |= !strcasecmp(DIRS[k], DIRS[i]);
		if (!dup)
			n = scan_dir(DIRS[i], out, n, max, cia);
	}
	if (cia)
		amExit();
	return n;
}

void upd_install_file(const char *path)
{
	if (upd_busy())
		return;
	LightLock_Lock(&U.lock);
	util_strlcpy(U.file, path, sizeof U.file);
	U.i.done = U.i.total = 0;
	LightLock_Unlock(&U.lock);
	U.cmd = CMD_FILE;
	LightEvent_Signal(&U.ev);
}

/* ---------- hilo ---------- */

static void upd_thread(void *arg)
{
	(void)arg;
	for (;;) {
		LightEvent_Wait(&U.ev);
		int c = U.cmd;
		if (c == CMD_QUIT)
			break;
		U.cmd = CMD_NONE;
		if (c == CMD_CHECK)
			do_check();
		else if (c == CMD_INSTALL)
			do_install();
		else if (c == CMD_FILE)
			do_install_file();
	}
}

void upd_init(const char *self_path)
{
	LightLock_Init(&U.lock);
	LightEvent_Init(&U.ev, RESET_ONESHOT);
	util_strlcpy(U.self, self_path && !strncmp(self_path, "sdmc:/", 6) ? self_path : "sdmc:/3ds/Cloud3DS.3dsx", sizeof U.self);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	U.thr = threadCreate(upd_thread, NULL, 48 * 1024, prio + 2, -2, false);
}

static void send_cmd(int c)
{
	if (U.cmd == CMD_QUIT)
		return;
	U.cmd = c;
	LightEvent_Signal(&U.ev);
}

void upd_check(void)
{
	if (U.i.st != UPD_CHECKING && U.i.st != UPD_DOWNLOADING && U.i.st != UPD_INSTALLING && U.i.st != UPD_DONE)
		send_cmd(CMD_CHECK);
}

void upd_install(void)
{
	if (U.i.st == UPD_AVAILABLE)
		send_cmd(CMD_INSTALL);
}

void upd_get(UpdInfo *out)
{
	LightLock_Lock(&U.lock);
	*out = U.i;
	LightLock_Unlock(&U.lock);
}

bool upd_busy(void)
{
	return U.i.st == UPD_DOWNLOADING || U.i.st == UPD_INSTALLING;
}

void upd_exit(void)
{
	if (!U.thr)
		return;
	U.cmd = CMD_QUIT;
	LightEvent_Signal(&U.ev);
	threadJoin(U.thr, 5000000000ull);
	threadFree(U.thr);
	U.thr = NULL;
	curl_global_cleanup();
}
