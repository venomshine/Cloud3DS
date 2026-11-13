/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* OneDrive con Microsoft Graph.
 *  - Conectar: inicio de sesión por código (RFC 8628). La consola pide un código a
 *    login.microsoftonline.com, lo enseña con un QR y espera a que se acepte en el móvil.
 *    No hace falta ningún secreto: la app es un «cliente público».
 *  - El refresh token se guarda en APP_DIR/onedrive.json (solo en la SD) y se renueva solo.
 *  - Listar, descargar (por la cola de descargas), subir (a trozos si es grande) y borrar. */
#include "cloud.h"
#include "cJSON.h"
#include "dl.h"
#include "fsx.h"
#include "state.h"
#include "titles.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* el ID de la app registrada en Microsoft Entra (no es secreto). Vacío: la nube no está configurada */
#ifndef OD_CLIENT_ID
#define OD_CLIENT_ID "5ec2cd40-16af-46b5-a564-8eeedc770d6a"
#endif
#ifndef OD_LOGIN
#define OD_LOGIN "https://login.microsoftonline.com/common/oauth2/v2.0"
#endif
#ifndef OD_GRAPH
#define OD_GRAPH "https://graph.microsoft.com/v1.0"
#endif
#define OD_SCOPE  "Files.ReadWrite User.Read offline_access"
#define OD_FILE   APP_DIR "/onedrive.json"
#define OD_FOLDER "Cloud3DS"
#define MAX_ITEMS 400
#define CHUNK     (640 * 1024) /* múltiplo de 320 KiB, como pide OneDrive */
#define DEPTH     16

enum { J_NONE, J_CONNECT, J_LIST, J_DOWNLOAD, J_UPLOAD, J_DELETE, J_ME, J_CAPS };
#define SHOTS "/luma/screenshots"
typedef struct {
	int type;
	char a[700];   /* ruta de la SD, id… */
	char b[160];   /* id del padre, nombre… */
} Job;

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	CloudInfo info;
	char access[4096], refresh[4096];
	u64 access_until;
	char device_code[4096]; /* el de Microsoft mide ~1050 caracteres */
	int interval;
	u64 next_poll;
	Job q[16];
	int nq;
	/* carpeta que se ve */
	struct {
		char id[64], name[160];
	} path[DEPTH];
	int depth;
	CloudItem *items;
	int nitems;
	bool loading;
	char folder_id[64]; /* la carpeta Cloud3DS, cuando ya se sabe */
	cJSON *shots;       /* capturas ya subidas */
	u64 next_caps;      /* ms: próxima mirada a las capturas */
} C;

static void lock(void) { LightLock_Lock(&C.lock); }
static void unlock(void) { LightLock_Unlock(&C.lock); }

static void bump(void)
{
	C.info.rev++;
}

static void set_err(const char *e)
{
	lock();
	util_strlcpy(C.info.err, e, sizeof C.info.err);
	bump();
	unlock();
}

/* ------------------------------------------------------------ HTTP */
typedef struct {
	char *p;
	size_t n, max;
} Mem;

static size_t mem_cb(char *d, size_t s, size_t m, void *u)
{
	Mem *b = u;
	size_t k = s * m;
	if (b->n + k + 1 > b->max)
		return 0;
	char *np = realloc(b->p, b->n + k + 1);
	if (!np)
		return 0;
	memcpy(np + b->n, d, k);
	b->p = np, b->n += k, b->p[b->n] = 0;
	return k;
}

static void url_enc(const char *in, char *out, size_t n, bool keep_slash)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (; *in && o + 4 < n; in++) {
		unsigned char c = *in;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c) || (keep_slash && c == '/'))
			out[o++] = c;
		else
			out[o++] = '%', out[o++] = hex[c >> 4], out[o++] = hex[c & 15];
	}
	out[o] = 0;
}

/* POST de formulario a login.microsoftonline.com; devuelve el JSON (o NULL) y el código HTTP */
static cJSON *form_post(const char *path, const char *body, long *code)
{
	char url[256];
	snprintf(url, sizeof url, "%s/%s", OD_LOGIN, path);
	static const char *const H[] = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json", NULL};
	Mem m = {NULL, 0, 64 * 1024};
	*code = 0;
	int r = dl_request(HTTPC_METHOD_POST, url, H, body, strlen(body), mem_cb, &m, code);
	cJSON *j = r == DLF_OK && m.p ? cJSON_Parse(m.p) : NULL;
	if (r != DLF_OK)
		util_log("nube: %s -> sin conexión (%d)", path, r);
	free(m.p);
	return j;
}

static void save_tokens(void)
{
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "refresh", C.refresh);
	cJSON_AddStringToObject(j, "name", C.info.name);
	cJSON_AddStringToObject(j, "email", C.info.email);
	cJSON_AddBoolToObject(j, "caps", C.info.caps);
	if (C.shots)
		cJSON_AddItemToObject(j, "shots", cJSON_Duplicate(C.shots, true));
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!s)
		return;
	FILE *f = fopen(OD_FILE ".tmp", "wb");
	if (f) {
		bool ok = fwrite(s, 1, strlen(s), f) == strlen(s);
		ok = fclose(f) == 0 && ok;
		if (ok) {
			remove(OD_FILE);
			rename(OD_FILE ".tmp", OD_FILE);
		}
	}
	free(s);
}

static void forget(void)
{
	remove(OD_FILE);
	lock();
	C.refresh[0] = C.access[0] = 0;
	C.access_until = 0;
	C.info.name[0] = C.info.email[0] = 0;
	C.info.used = C.info.total = 0;
	C.info.st = CLS_OFF;
	C.nitems = 0;
	C.depth = 0;
	C.folder_id[0] = 0;
	bump();
	unlock();
}

static bool take_tokens(cJSON *j)
{
	const cJSON *at = cJSON_GetObjectItemCaseSensitive(j, "access_token"), *rt = cJSON_GetObjectItemCaseSensitive(j, "refresh_token"),
	            *ex = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
	if (!cJSON_IsString(at))
		return false;
	lock();
	util_strlcpy(C.access, at->valuestring, sizeof C.access);
	if (cJSON_IsString(rt))
		util_strlcpy(C.refresh, rt->valuestring, sizeof C.refresh);
	C.access_until = util_ms() + (u64)(cJSON_IsNumber(ex) ? ex->valueint : 3600) * 1000;
	unlock();
	return true;
}

/* un access token válido; lo renueva con el refresh token si hace falta */
static bool token(bool force)
{
	if (!force && C.access[0] && util_ms() + 60000 < C.access_until)
		return true;
	if (!C.refresh[0])
		return false;
	char rt[4200], body[4600];
	url_enc(C.refresh, rt, sizeof rt, false);
	snprintf(body, sizeof body, "client_id=%s&grant_type=refresh_token&refresh_token=%s&scope=Files.ReadWrite%%20User.Read%%20offline_access", OD_CLIENT_ID, rt);
	long code;
	cJSON *j = form_post("token", body, &code);
	bool ok = j && code == 200 && take_tokens(j);
	if (!ok && j) {
		const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
		if (cJSON_IsString(e) && !strcmp(e->valuestring, "invalid_grant")) { // retirado desde la cuenta: hay que volver a conectar
			util_log("nube: el permiso ya no vale (invalid_grant)");
			cJSON_Delete(j);
			forget();
			set_err("revoked");
			return false;
		}
	}
	cJSON_Delete(j);
	if (ok)
		save_tokens();
	return ok;
}

/* petición a Graph con el token; si responde 401, se renueva y se repite una vez */
static int graph(int method, const char *path_or_url, const char *ctype, const void *body, size_t blen, Mem *out, long *code)
{
	char url[2048];
	if (!strncmp(path_or_url, "http", 4))
		util_strlcpy(url, path_or_url, sizeof url);
	else
		snprintf(url, sizeof url, "%s%s", OD_GRAPH, path_or_url);
	for (int t = 0; t < 2; t++) {
		if (!token(t > 0))
			return DLF_FAIL;
		char auth[4200], ct[96];
		snprintf(auth, sizeof auth, "Authorization: Bearer %s", C.access);
		snprintf(ct, sizeof ct, "Content-Type: %s", ctype ? ctype : "application/json");
		const char *H[] = {auth, "Accept: application/json", ctype || body ? ct : NULL, NULL};
		if (out)
			free(out->p), out->p = NULL, out->n = 0;
		int r = dl_request(method, url, H, body, blen, out ? mem_cb : NULL, out, code);
		if (r != DLF_OK || *code != 401)
			return r;
	}
	return DLF_OK;
}

static cJSON *graph_json(const char *path, long *code)
{
	Mem m = {NULL, 0, 2 * 1024 * 1024};
	int r = graph(HTTPC_METHOD_GET, path, NULL, NULL, 0, &m, code);
	cJSON *j = r == DLF_OK && m.p ? cJSON_Parse(m.p) : NULL;
	free(m.p);
	return j;
}

/* ------------------------------------------------------------ trabajos */
static void do_me(void)
{
	long code;
	cJSON *me = graph_json("/me?$select=displayName,userPrincipalName,mail", &code);
	cJSON *dr = graph_json("/me/drive?$select=quota", &code);
	lock();
	if (me) {
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(me, "displayName"), *m = cJSON_GetObjectItemCaseSensitive(me, "mail"),
		            *u = cJSON_GetObjectItemCaseSensitive(me, "userPrincipalName");
		if (cJSON_IsString(n))
			util_strlcpy(C.info.name, n->valuestring, sizeof C.info.name);
		util_strlcpy(C.info.email, cJSON_IsString(m) ? m->valuestring : cJSON_IsString(u) ? u->valuestring : "", sizeof C.info.email);
	}
	const cJSON *q = cJSON_GetObjectItemCaseSensitive(dr, "quota");
	if (q) {
		const cJSON *t = cJSON_GetObjectItemCaseSensitive(q, "total"), *us = cJSON_GetObjectItemCaseSensitive(q, "used");
		if (cJSON_IsNumber(t))
			C.info.total = (u64)t->valuedouble;
		if (cJSON_IsNumber(us))
			C.info.used = (u64)us->valuedouble;
	}
	bump();
	unlock();
	cJSON_Delete(me);
	cJSON_Delete(dr);
	if (me)
		save_tokens();
}

static void do_connect(void)
{
	char body[256];
	snprintf(body, sizeof body, "client_id=%s&scope=Files.ReadWrite%%20User.Read%%20offline_access", OD_CLIENT_ID);
	long code;
	cJSON *j = form_post("devicecode", body, &code);
	const cJSON *dc = cJSON_GetObjectItemCaseSensitive(j, "device_code"), *uc = cJSON_GetObjectItemCaseSensitive(j, "user_code"),
	            *vu = cJSON_GetObjectItemCaseSensitive(j, "verification_uri"), *ex = cJSON_GetObjectItemCaseSensitive(j, "expires_in"),
	            *iv = cJSON_GetObjectItemCaseSensitive(j, "interval");
	if (!cJSON_IsString(dc) || !cJSON_IsString(uc) || !cJSON_IsString(vu)) {
		util_log("nube: devicecode -> HTTP %ld", code);
		cJSON_Delete(j);
		lock();
		C.info.st = CLS_OFF;
		C.info.busy = false;
		util_strlcpy(C.info.err, code ? "server" : "offline", sizeof C.info.err);
		bump();
		unlock();
		return;
	}
	lock();
	util_strlcpy(C.device_code, dc->valuestring, sizeof C.device_code);
	util_strlcpy(C.info.user_code, uc->valuestring, sizeof C.info.user_code);
	util_strlcpy(C.info.verify, vu->valuestring, sizeof C.info.verify);
	C.info.code_until = util_ms() + (u64)(cJSON_IsNumber(ex) ? ex->valueint : 900) * 1000;
	C.interval = cJSON_IsNumber(iv) && iv->valueint > 0 ? iv->valueint : 5;
	C.next_poll = util_ms() + C.interval * 1000;
	C.info.st = CLS_CODE;
	C.info.busy = false;
	C.info.err[0] = 0;
	bump();
	unlock();
	cJSON_Delete(j);
}

/* ¿ya aceptó en el móvil? */
static void poll_code(void)
{
	if (util_ms() < C.next_poll)
		return;
	if (util_ms() > C.info.code_until) {
		lock();
		C.info.st = CLS_OFF;
		util_strlcpy(C.info.err, "expired", sizeof C.info.err);
		bump();
		unlock();
		return;
	}
	static char dc[4200], body[4400]; // solo el hilo de la nube los usa
	url_enc(C.device_code, dc, sizeof dc, false);
	snprintf(body, sizeof body, "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code&client_id=%s&device_code=%s", OD_CLIENT_ID, dc);
	long code;
	cJSON *j = form_post("token", body, &code);
	C.next_poll = util_ms() + C.interval * 1000;
	if (j && code == 200 && take_tokens(j)) {
		cJSON_Delete(j);
		lock();
		C.info.st = CLS_ON;
		C.info.err[0] = 0;
		C.info.caps = true;
		bump();
		unlock();
		util_log("nube: conectada");
		do_me();
		save_tokens();
		cloud_list("", "OneDrive");
		return;
	}
	const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
	const char *es = cJSON_IsString(e) ? e->valuestring : "";
	if (!strcmp(es, "slow_down"))
		C.interval += 5;
	else if (strcmp(es, "authorization_pending") && j) { // rechazado, caducado o mal
		util_log("nube: token -> %s", es);
		lock();
		C.info.st = CLS_OFF;
		util_strlcpy(C.info.err, !strcmp(es, "authorization_declined") ? "declined" : !strcmp(es, "expired_token") ? "expired" : "server", sizeof C.info.err);
		bump();
		unlock();
	}
	cJSON_Delete(j);
}

static void parse_items(cJSON *arr, CloudItem *it, int *n)
{
	const cJSON *e;
	cJSON_ArrayForEach(e, arr)
	{
		if (*n >= MAX_ITEMS)
			return;
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id"), *nm = cJSON_GetObjectItemCaseSensitive(e, "name"),
		            *sz = cJSON_GetObjectItemCaseSensitive(e, "size"), *fo = cJSON_GetObjectItemCaseSensitive(e, "folder"),
		            *mt = cJSON_GetObjectItemCaseSensitive(e, "lastModifiedDateTime");
		if (!cJSON_IsString(id) || !cJSON_IsString(nm))
			continue;
		CloudItem *c = &it[(*n)++];
		memset(c, 0, sizeof *c);
		util_strlcpy(c->id, id->valuestring, sizeof c->id);
		util_strlcpy(c->name, nm->valuestring, sizeof c->name);
		c->size = cJSON_IsNumber(sz) ? (u64)sz->valuedouble : 0;
		c->dir = fo != NULL;
		const cJSON *cc = fo ? cJSON_GetObjectItemCaseSensitive(fo, "childCount") : NULL;
		c->children = cJSON_IsNumber(cc) ? cc->valueint : 0;
		if (cJSON_IsString(mt))
			util_strlcpy(c->mtime, mt->valuestring, sizeof c->mtime);
	}
}

static int item_cmp(const void *a, const void *b)
{
	const CloudItem *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	if (x->dir && !strcmp(x->name, OD_FOLDER)) // la de la app, la primera
		return -1;
	if (y->dir && !strcmp(y->name, OD_FOLDER))
		return 1;
	return strcasecmp(x->name, y->name);
}

static void do_list(const char *id)
{
	CloudItem *it = malloc(sizeof(CloudItem) * MAX_ITEMS);
	if (!it)
		return;
	int n = 0;
	char path[300];
	if (id[0])
		snprintf(path, sizeof path, "/me/drive/items/%s/children?$top=200&$select=id,name,size,folder,file,lastModifiedDateTime", id);
	else
		snprintf(path, sizeof path, "/me/drive/root/children?$top=200&$select=id,name,size,folder,file,lastModifiedDateTime");
	char next[2048];
	util_strlcpy(next, path, sizeof next);
	long code = 0;
	for (int page = 0; page < 5 && next[0]; page++) {
		cJSON *j = graph_json(next, &code);
		next[0] = 0;
		if (!j)
			break;
		parse_items(cJSON_GetObjectItemCaseSensitive(j, "value"), it, &n);
		const cJSON *nl = cJSON_GetObjectItemCaseSensitive(j, "@odata.nextLink");
		if (cJSON_IsString(nl))
			util_strlcpy(next, nl->valuestring, sizeof next);
		cJSON_Delete(j);
	}
	qsort(it, n, sizeof *it, item_cmp);
	lock();
	// solo si sigue siendo la carpeta que se ve
	if (!strcmp(C.depth ? C.path[C.depth - 1].id : "", id)) {
		free(C.items);
		C.items = it, C.nitems = n, it = NULL;
		C.loading = false;
		if (!code || code >= 400)
			util_strlcpy(C.info.err, code ? "server" : "offline", sizeof C.info.err);
		else
			C.info.err[0] = 0;
		bump();
	}
	unlock();
	free(it);
}

/* la carpeta Cloud3DS de OneDrive (la crea si no está) */
static bool app_folder(char *id, size_t n)
{
	if (C.folder_id[0]) {
		util_strlcpy(id, C.folder_id, n);
		return true;
	}
	long code;
	for (int t = 0; t < 2; t++) {
		cJSON *j = graph_json("/me/drive/root:/" OD_FOLDER "?$select=id", &code);
		const cJSON *i = cJSON_GetObjectItemCaseSensitive(j, "id");
		if (cJSON_IsString(i)) {
			util_strlcpy(C.folder_id, i->valuestring, sizeof C.folder_id);
			util_strlcpy(id, i->valuestring, n);
			cJSON_Delete(j);
			return true;
		}
		cJSON_Delete(j);
		if (code != 404 || t)
			return false;
		static const char body[] = "{\"name\":\"" OD_FOLDER "\",\"folder\":{},\"@microsoft.graph.conflictBehavior\":\"fail\"}";
		Mem m = {NULL, 0, 64 * 1024};
		graph(HTTPC_METHOD_POST, "/me/drive/root/children", "application/json", body, sizeof body - 1, &m, &code);
		free(m.p);
	}
	return false;
}

/* sube un archivo de la SD a la carpeta parent de OneDrive; xid >= 0: suma su progreso a esa
 * transferencia a partir de base. Devuelve si salió bien */
static bool upload_file(const char *sd, const char *parent, int xid, u64 base)
{
	const char *nm = strrchr(sd, '/');
	nm = nm ? nm + 1 : sd;
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", sd);
	FILE *f = fopen(sp, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	u64 size = (u64)ftell(f);
	fseek(f, 0, SEEK_SET);
	char enc[500], path[700];
	url_enc(nm, enc, sizeof enc, false);
	u8 *buf = malloc(size < CHUNK ? (size ? size : 1) : CHUNK);
	bool ok = buf != NULL;
	long code = 0;
	Mem m = {NULL, 0, 256 * 1024};
	if (ok && size <= CHUNK) { // pequeño: de una vez
		size_t got = size ? fread(buf, 1, size, f) : 0;
		snprintf(path, sizeof path, "/me/drive/items/%s:/%s:/content?@microsoft.graph.conflictBehavior=rename", parent, enc);
		ok = got == size && graph(HTTPC_METHOD_PUT, path, "application/octet-stream", buf, size, &m, &code) == DLF_OK && code < 300;
	} else if (ok) { // grande: una sesión y trozos
		snprintf(path, sizeof path, "/me/drive/items/%s:/%s:/createUploadSession", parent, enc);
		static const char body[] = "{\"item\":{\"@microsoft.graph.conflictBehavior\":\"rename\"}}";
		ok = graph(HTTPC_METHOD_POST, path, "application/json", body, sizeof body - 1, &m, &code) == DLF_OK && code < 300;
		cJSON *j = ok && m.p ? cJSON_Parse(m.p) : NULL;
		const cJSON *uu = cJSON_GetObjectItemCaseSensitive(j, "uploadUrl");
		char upurl[2048] = "";
		if (cJSON_IsString(uu))
			util_strlcpy(upurl, uu->valuestring, sizeof upurl);
		cJSON_Delete(j);
		ok = ok && upurl[0];
		for (u64 off = 0; ok && off < size && !C.quit;) {
			size_t want = size - off > CHUNK ? CHUNK : (size_t)(size - off), got = fread(buf, 1, want, f);
			if (got != want) {
				ok = false;
				break;
			}
			char cr[96];
			snprintf(cr, sizeof cr, "Content-Range: bytes %llu-%llu/%llu", (unsigned long long)off, (unsigned long long)(off + got - 1),
			         (unsigned long long)size);
			const char *H[] = {cr, "Content-Type: application/octet-stream", NULL}; // la sesión ya autoriza: sin token
			free(m.p), m.p = NULL, m.n = 0;
			int r = dl_request(HTTPC_METHOD_PUT, upurl, H, buf, got, mem_cb, &m, &code);
			ok = r == DLF_OK && (code == 200 || code == 201 || code == 202);
			off += got;
			if (xid >= 0) {
				state_lock();
				xfer_progress(xid, base + off);
				state_unlock();
			}
		}
	}
	free(m.p);
	free(buf);
	fclose(f);
	if (xid >= 0) {
		state_lock();
		xfer_progress(xid, base + size);
		state_unlock();
	}
	util_log("nube: subir %s -> %s (HTTP %ld)", sd, ok ? "bien" : "mal", code);
	return ok;
}

static void do_upload(const char *sd, const char *parent0)
{
	char parent[64];
	if (parent0[0])
		util_strlcpy(parent, parent0, sizeof parent);
	else if (!app_folder(parent, sizeof parent)) {
		set_err("upload");
		return;
	}
	const char *nm = strrchr(sd, '/');
	bool d;
	u64 size = 0;
	fsx_stat(sd, &d, &size);
	state_lock();
	int xid = xfer_begin(XF_DOWNLOAD, nm ? nm + 1 : sd, "OneDrive", size); // la consola envía
	state_unlock();
	bool ok = upload_file(sd, parent, xid, 0);
	state_lock();
	xfer_end(xid, ok ? XS_DONE : XS_FAILED);
	state_unlock();
	if (!ok)
		set_err("upload");
	// si se está viendo esa carpeta, se vuelve a leer
	lock();
	bool here = !strcmp(C.depth ? C.path[C.depth - 1].id : "", parent) || (!parent0[0] && C.depth && !strcmp(C.path[C.depth - 1].id, C.folder_id));
	unlock();
	if (here)
		do_list(parent);
	do_me();
}

/* la carpeta de OneDrive con esa ruta (desde la raíz, con «/»), creándola si falta; su id en out */
static bool ensure_path(const char *rel, char *out, size_t n)
{
	char parent[64] = "", sofar[600] = "";
	const char *p = rel;
	while (*p) {
		const char *e = strchr(p, '/');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		char seg[200];
		snprintf(seg, sizeof seg, "%.*s", (int)(l < sizeof seg - 1 ? l : sizeof seg - 1), p);
		p += l + (e ? 1 : 0);
		if (!seg[0])
			continue;
		size_t sl = strlen(sofar);
		snprintf(sofar + sl, sizeof sofar - sl, "%s%s", sl ? "/" : "", seg);
		char enc[900], path[1000];
		url_enc(sofar, enc, sizeof enc, true);
		snprintf(path, sizeof path, "/me/drive/root:/%s?$select=id", enc);
		long code;
		bool got = false;
		for (int t = 0; t < 2 && !got; t++) {
			cJSON *j = graph_json(path, &code);
			const cJSON *i = cJSON_GetObjectItemCaseSensitive(j, "id");
			if (cJSON_IsString(i)) {
				util_strlcpy(parent, i->valuestring, sizeof parent);
				got = true;
			}
			cJSON_Delete(j);
			if (got || code != 404 || t)
				break;
			// no está: se crea dentro de la anterior
			cJSON *b = cJSON_CreateObject();
			cJSON_AddStringToObject(b, "name", seg);
			cJSON_AddItemToObject(b, "folder", cJSON_CreateObject());
			cJSON_AddStringToObject(b, "@microsoft.graph.conflictBehavior", "fail");
			char *body = cJSON_PrintUnformatted(b);
			cJSON_Delete(b);
			char cpath[200];
			if (parent[0])
				snprintf(cpath, sizeof cpath, "/me/drive/items/%s/children", parent);
			else
				util_strlcpy(cpath, "/me/drive/root/children", sizeof cpath);
			Mem m = {NULL, 0, 64 * 1024};
			graph(HTTPC_METHOD_POST, cpath, "application/json", body, body ? strlen(body) : 0, &m, &code);
			free(body);
			// la respuesta trae su id; si otro la creó a la vez (409), se vuelve a buscar
			cJSON *r = code < 300 && m.p ? cJSON_ParseWithLength(m.p, m.n) : NULL;
			const cJSON *ri = cJSON_GetObjectItemCaseSensitive(r, "id");
			if (cJSON_IsString(ri)) {
				util_strlcpy(parent, ri->valuestring, sizeof parent);
				got = true;
			}
			cJSON_Delete(r);
			free(m.p);
		}
		if (!got)
			return false;
	}
	util_strlcpy(out, parent, n);
	return parent[0] != 0;
}

typedef struct {
	char files[64][300];
	int nf;
	char dirs[16][160];
	int nd;
	u64 bytes;
} TreeList;

static bool shot_cb(const FsxEntry *e, void *u)
{
	TreeList *t = u;
	size_t l = strlen(e->name);
	if (!e->dir && l > 4 && !strcasecmp(e->name + l - 4, ".bmp") && t->nf < 64)
		util_strlcpy(t->files[t->nf++], e->name, sizeof t->files[0]);
	return true;
}

static bool shot_done(const char *name)
{
	const cJSON *e;
	cJSON_ArrayForEach(e, C.shots)
	if (cJSON_IsString(e) && !strcmp(e->valuestring, name))
		return true;
	return false;
}

/* las capturas nuevas de Rosalina, a /Cloud3DS/Capturas */
static void do_caps(void)
{
	TreeList *t = calloc(1, sizeof *t);
	if (!t)
		return;
	fsx_list(SHOTS, false, shot_cb, t);
	if (!C.shots)
		C.shots = cJSON_CreateArray();
	char id[64] = "";
	int up = 0, todo = 0;
	for (int i = 0; i < t->nf; i++)
		todo += !shot_done(t->files[i]);
	lock();
	C.info.caps_queue = todo;
	bump();
	unlock();
	for (int i = 0; i < t->nf && !C.quit; i++) {
		if (shot_done(t->files[i]))
			continue;
		if (!id[0] && !ensure_path(OD_FOLDER "/Capturas", id, sizeof id))
			break;
		char p[400];
		snprintf(p, sizeof p, SHOTS "/%s", t->files[i]);
		if (!upload_file(p, id, -1, 0))
			break;
		cJSON_AddItemToArray(C.shots, cJSON_CreateString(t->files[i]));
		while (cJSON_GetArraySize(C.shots) > 1000) // las más antiguas se olvidan
			cJSON_DeleteItemFromArray(C.shots, 0);
		up++;
		lock();
		C.info.caps_queue = todo - up;
		bump();
		unlock();
	}
	free(t);
	lock();
	C.info.caps_queue = 0;
	bump();
	unlock();
	if (up) {
		util_log("nube: %d capturas subidas", up);
		save_tokens();
	}
}

static void do_download(const char *id, const char *name)
{
	char path[300];
	snprintf(path, sizeof path, "/me/drive/items/%s?$select=id,name,@microsoft.graph.downloadUrl", id);
	long code;
	cJSON *j = graph_json(path, &code);
	const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "@microsoft.graph.downloadUrl");
	if (!cJSON_IsString(u)) {
		cJSON_Delete(j);
		set_err("download");
		return;
	}
	size_t l = strlen(name);
	bool cia = l > 4 && !strcasecmp(name + l - 4, ".cia");
	u32 did = 0;
	// pedido desde la propia consola: un .cia se instala mientras baja
	if (dl_add(u->valuestring, cia ? "/cias" : "/Descargas", "", cia ? DLA_INSTALL | DLA_STREAM : DLA_NONE, "OneDrive", &did) == 0)
		dl_set_name(did, name);
	else
		set_err("download");
	cJSON_Delete(j);
}

static void do_delete(const char *id)
{
	char path[200];
	snprintf(path, sizeof path, "/me/drive/items/%s", id);
	long code;
	int r = graph(HTTPC_METHOD_DELETE, path, NULL, NULL, 0, NULL, &code);
	if (r != DLF_OK || code >= 300)
		set_err("delete");
	lock();
	char cur[64];
	util_strlcpy(cur, C.depth ? C.path[C.depth - 1].id : "", sizeof cur);
	unlock();
	do_list(cur);
	do_me();
}

static void push(int type, const char *a, const char *b)
{
	lock();
	if (C.nq < 16) {
		Job *j = &C.q[C.nq++];
		j->type = type;
		util_strlcpy(j->a, a ? a : "", sizeof j->a);
		util_strlcpy(j->b, b ? b : "", sizeof j->b);
	}
	unlock();
	LightEvent_Signal(&C.ev);
}

static void thread_main(void *arg)
{
	(void)arg;
	while (!C.quit) {
		LightEvent_WaitTimeout(&C.ev, 500 * 1000000ll);
		if (C.quit)
			break;
		if (!util_ip())
			continue;
		if (C.info.st == CLS_CODE)
			poll_code();
		if (C.info.st == CLS_ON && util_ms() > C.next_caps) {
			C.next_caps = util_ms() + 60000;
			bool queued = false;
			lock();
			for (int k = 0; k < C.nq; k++)
				queued |= C.q[k].type == J_CAPS;
			unlock();
			if (!queued && C.info.caps)
				push(J_CAPS, NULL, NULL);
		}
		Job j;
		lock();
		bool have = C.nq > 0;
		if (have) {
			j = C.q[0];
			memmove(C.q, C.q + 1, sizeof C.q[0] * --C.nq);
			C.info.busy = true;
		}
		unlock();
		if (!have)
			continue;
		switch (j.type) {
		case J_CONNECT: do_connect(); break;
		case J_ME: do_me(); break;
		case J_LIST: do_list(j.a); break;
		case J_DOWNLOAD: do_download(j.a, j.b); break;
		case J_UPLOAD: do_upload(j.a, j.b); break;
		case J_DELETE: do_delete(j.a); break;
		case J_CAPS: do_caps(); break;
		}
		lock();
		C.info.busy = C.nq > 0;
		bump();
		unlock();
	}
}

/* ------------------------------------------------------------ uso */
void cloud_init(void)
{
	LightLock_Init(&C.lock);
	LightEvent_Init(&C.ev, RESET_ONESHOT);
	C.info.st = OD_CLIENT_ID[0] ? CLS_OFF : CLS_NOCFG;
	FILE *f = fopen(OD_FILE, "rb");
	if (f && C.info.st == CLS_OFF) {
		// entero: con la lista de capturas ya subidas puede pasar de decenas de KB
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buf = sz > 0 && sz < 4 * 1024 * 1024 ? malloc(sz + 1) : NULL;
		size_t n = buf ? fread(buf, 1, sz, f) : 0;
		if (buf)
			buf[n] = 0;
		cJSON *j = buf ? cJSON_Parse(buf) : NULL;
		free(buf);
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "refresh"), *nm = cJSON_GetObjectItemCaseSensitive(j, "name"),
		            *em = cJSON_GetObjectItemCaseSensitive(j, "email");
		const cJSON *cp = cJSON_GetObjectItemCaseSensitive(j, "caps");
		C.info.caps = cJSON_IsBool(cp) ? cJSON_IsTrue(cp) : true;
		cJSON *sh = cJSON_DetachItemFromObjectCaseSensitive(j, "shots");
		if (sh)
			C.shots = sh;
		if (cJSON_IsString(r) && r->valuestring[0]) {
			util_strlcpy(C.refresh, r->valuestring, sizeof C.refresh);
			if (cJSON_IsString(nm))
				util_strlcpy(C.info.name, nm->valuestring, sizeof C.info.name);
			if (cJSON_IsString(em))
				util_strlcpy(C.info.email, em->valuestring, sizeof C.info.email);
			C.info.st = CLS_ON;
		}
		cJSON_Delete(j);
	}
	if (f)
		fclose(f);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	C.thr = threadCreate(thread_main, NULL, 64 * 1024, prio + 1, -2, false);
	if (C.info.st == CLS_ON) {
		push(J_ME, NULL, NULL);
	}
	C.next_caps = util_ms() + 20000;
}

void cloud_exit(void)
{
	C.quit = true;
	LightEvent_Signal(&C.ev);
	if (C.thr) {
		threadJoin(C.thr, 5000000000ull);
		threadFree(C.thr);
		C.thr = NULL;
	}
	free(C.items);
	C.items = NULL;
}

void cloud_get(CloudInfo *out)
{
	lock();
	*out = C.info;
	unlock();
}

bool cl_connected(void)
{
	return C.info.st == CLS_ON;
}

void cloud_connect(void)
{
	if (C.info.st != CLS_OFF)
		return;
	lock();
	C.info.busy = true;
	C.info.err[0] = 0;
	bump();
	unlock();
	push(J_CONNECT, NULL, NULL);
}

void cloud_cancel(void)
{
	lock();
	if (C.info.st == CLS_CODE)
		C.info.st = CLS_OFF;
	bump();
	unlock();
}

void cloud_disconnect(void)
{
	forget();
}

void cloud_list(const char *id, const char *name)
{
	lock();
	if (!id[0])
		C.depth = 0;
	if (C.depth < DEPTH) {
		util_strlcpy(C.path[C.depth].id, id, sizeof C.path[0].id);
		util_strlcpy(C.path[C.depth].name, name, sizeof C.path[0].name);
		C.depth++;
	}
	C.loading = true;
	C.nitems = 0;
	bump();
	unlock();
	push(J_LIST, id, NULL);
}

void cloud_up(void)
{
	lock();
	if (C.depth > 1) {
		C.depth--;
		C.loading = true;
		C.nitems = 0;
		char id[64];
		util_strlcpy(id, C.path[C.depth - 1].id, sizeof id);
		bump();
		unlock();
		push(J_LIST, id, NULL);
		return;
	}
	unlock();
}

int cloud_depth(void)
{
	return C.depth;
}

const char *cloud_folder_name(void)
{
	return C.depth ? C.path[C.depth - 1].name : "OneDrive";
}

const char *cloud_folder_id(void)
{
	return C.depth ? C.path[C.depth - 1].id : "";
}

int cloud_items(CloudItem *out, int max, bool *loading)
{
	lock();
	int n = C.nitems < max ? C.nitems : max;
	if (n && C.items)
		memcpy(out, C.items, n * sizeof *out);
	*loading = C.loading;
	unlock();
	return n;
}

void cloud_download(const CloudItem *it)
{
	if (!it->dir)
		push(J_DOWNLOAD, it->id, it->name);
}

void cloud_upload(const char *sd_path, const char *parent_id)
{
	push(J_UPLOAD, sd_path, parent_id);
}

void cloud_delete(const CloudItem *it)
{
	push(J_DELETE, it->id, NULL);
}

void cloud_set_caps(bool on)
{
	lock();
	C.info.caps = on;
	bump();
	unlock();
	save_tokens();
}

void cloud_backup_now(void)
{
	push(J_CAPS, NULL, NULL);
}
