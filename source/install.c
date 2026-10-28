/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Instalar CIA, siguiendo a FBI (source/fbi/action/installcias.c) con dos
 * diferencias a propósito, por seguridad:
 *  - Nunca se borra el título instalado antes (FBI hace AM_DeleteTitle y
 *    AM_DeleteTicket): así no se pueden perder partidas. AM acepta la misma
 *    versión o una más nueva encima; si ya hay una más nueva, se explica.
 *  - Solo títulos que van a la SD. Los del sistema, DSiWare y firmware (NAND)
 *    se rechazan siempre. */
#include "install.h"
#include "fsx.h"
#include "state.h"
#include "util.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SELF_TID 0x000400000C13D500ull
#define CHUNK    (256 * 1024)

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	InsReq q[MAX_INS];
	int next_id;
} I;

/* ---------- leer el CIA ---------- */

static u32 rd32le(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64le(const u8 *p) { return rd32le(p) | (u64)rd32le(p + 4) << 32; }
static u64 rd64be(const u8 *p)
{
	u64 v = 0;
	for (int i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}
static u64 al64(u64 x) { return (x + 63) & ~63ull; }

static u32 morton(u32 x, u32 y)
{
	static const u8 tbl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
	return tbl[x & 7] | (tbl[y & 7] << 1);
}

static void utf16_field(char *out, size_t n, const u8 *p, int chars)
{
	u16 tmp[0x81] = {0};
	for (int i = 0; i < chars && i < 0x80; i++)
		tmp[i] = p[i * 2] | p[i * 2 + 1] << 8;
	util_utf16_to_utf8(out, n, tmp, chars);
}

/* idioma del SMDH según la consola: 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es… */
static int smdh_lang(void)
{
	u8 l = 1;
	if (R_SUCCEEDED(cfguInit())) {
		CFGU_GetSystemLanguage(&l);
		cfguExit();
	}
	return l < 16 ? l : 1;
}

/* cabecera del CIA → título, versión y, si hay meta, el SMDH (nombre e icono) */
static bool cia_info(const char *path, InsReq *r)
{
	char full[300];
	snprintf(full, sizeof full, "sdmc:%s", path);
	FILE *f = fopen(full, "rb");
	if (!f)
		return false;
	u8 h[0x20];
	bool ok = fread(h, 1, sizeof h, f) == sizeof h;
	u32 hs = rd32le(h), cs = rd32le(h + 8), tk = rd32le(h + 0xC), tmd = rd32le(h + 0x10), meta = rd32le(h + 0x14);
	u64 content = rd64le(h + 0x18);
	fseek(f, 0, SEEK_END);
	r->size = (u64)ftell(f);
	if (ok && hs == 0x2020 && tmd >= 0x200) {
		u64 to = al64(hs) + al64(cs) + al64(tk);
		u8 t[0x300];
		fseek(f, (long)to, SEEK_SET);
		if (fread(t, 1, sizeof t, f) == sizeof t) {
			u32 st = (u32)t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];
			int sig = st == 0x10003 ? 0x23C : st == 0x10004 ? 0x13C : st == 0x10005 ? 0x7C : -1;
			if (sig > 0 && 4 + sig + 0x9E <= (int)sizeof t) {
				const u8 *hd = t + 4 + sig;
				r->tid = rd64be(hd + 0x4C);
				r->ver = (u16)(hd[0x9C] << 8 | hd[0x9D]);
			} else {
				ok = false;
			}
		}
		if (ok && meta >= 0x400 + 0x36C0) {
			u64 mo = to + al64(tmd) + al64(content);
			u8 *smdh = malloc(0x36C0);
			if (smdh) {
				fseek(f, (long)(mo + 0x400), SEEK_SET);
				if (fread(smdh, 1, 0x36C0, f) == 0x36C0 && !memcmp(smdh, "SMDH", 4)) {
					int l = smdh_lang();
					const u8 *ti = smdh + 8 + l * 0x200;
					if (!ti[0] && !ti[1])
						ti = smdh + 8 + 1 * 0x200; /* sin ese idioma: inglés */
					utf16_field(r->title, sizeof r->title, ti, 0x40);
					utf16_field(r->publisher, sizeof r->publisher, ti + 0x180, 0x40);
					const u16 *ic = (const u16 *)(smdh + 0x24C0);
					for (int y = 0; y < 48; y++)
						for (int x = 0; x < 48; x++)
							r->icon[y * 48 + x] = ic[((y >> 3) * 6 + (x >> 3)) * 64 + morton(x, y)];
					r->has_icon = true;
				}
				free(smdh);
			}
		}
	} else {
		ok = false;
	}
	fclose(f);
	return ok;
}

/* destino según el ID, igual que fs_get_title_destination() de FBI */
static bool goes_to_nand(u64 tid)
{
	u16 platform = (u16)(tid >> 48), category = (u16)(tid >> 32);
	u8 variation = (u8)tid;
	return platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02)));
}

static int installed_version(u64 tid)
{
	int v = -1;
	if (R_SUCCEEDED(amInit())) {
		AM_TitleEntry e;
		if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &e)))
			v = e.version;
		amExit();
	}
	return v;
}

/* ---------- cola ---------- */

static InsReq *find(int id)
{
	for (int i = 0; i < MAX_INS; i++)
		if (I.q[i].st != INS_FREE && I.q[i].id == id)
			return &I.q[i];
	return NULL;
}

int ins_request(const char *path, const char *device, char *err, size_t errn)
{
	const char *dot = strrchr(path, '.');
	if (!dot || strcasecmp(dot, ".cia")) {
		util_strlcpy(err, "not_cia", errn);
		return -1;
	}
	InsReq *r = malloc(sizeof *r);
	if (!r) {
		util_strlcpy(err, "memory", errn);
		return -1;
	}
	memset(r, 0, sizeof *r);
	util_strlcpy(r->path, path, sizeof r->path);
	const char *b = strrchr(path, '/');
	util_strlcpy(r->file, b ? b + 1 : path, sizeof r->file);
	util_strlcpy(r->device, device, sizeof r->device);
	bool parsed = cia_info(path, r);
	/* la consola lee el CIA por su cuenta (como FBI): si AM no lo entiende, no se instala */
	Handle fh;
	bool am_ok = false;
	if (R_SUCCEEDED(fsx_open(path, FS_OPEN_READ, &fh))) {
		if (R_SUCCEEDED(amInit())) {
			AM_TitleEntry e;
			if (R_SUCCEEDED(AM_GetCiaFileInfo(MEDIATYPE_SD, &e, fh))) {
				r->tid = e.titleID;
				r->ver = e.version;
				am_ok = true;
			}
			u64 need = 0;
			if (am_ok && R_SUCCEEDED(AM_GetCiaRequiredSpace(&need, MEDIATYPE_SD, fh)))
				r->need = need;
			amExit();
		}
		FSFILE_Close(fh);
	}
	if (!am_ok) {
		util_log("instalar: AM no reconoce %s (cabecera %s)", path, parsed ? "bien" : "mal");
		free(r);
		util_strlcpy(err, "bad_cia", errn);
		return -1;
	}
	if (goes_to_nand(r->tid)) {
		util_log("instalar: %s es un título del sistema o DSiWare (%016llX): rechazado", path, (unsigned long long)r->tid);
		free(r);
		util_strlcpy(err, "system_title", errn);
		return -1;
	}
	r->n3ds_only = ((r->tid >> 28) & 0xF) == 2;
	if (!r->title[0]) {
		util_strlcpy(r->title, r->file, sizeof r->title);
		char *d = strrchr(r->title, '.');
		if (d)
			*d = 0;
	}
	r->self = r->tid == SELF_TID;
	r->inst_ver = installed_version(r->tid);
	util_log("instalar: %s (%016llX v%u, instalada %d) pedido por %s", path, (unsigned long long)r->tid, r->ver, r->inst_ver,
	         device);
	LightLock_Lock(&I.lock);
	int slot = -1;
	for (int i = 0; i < MAX_INS && slot < 0; i++)
		if (I.q[i].st == INS_FREE)
			slot = i;
	/* lleno: se reutiliza el terminado más antiguo */
	for (int i = 0; i < MAX_INS && slot < 0; i++)
		if (I.q[i].st >= INS_DONE)
			slot = i;
	for (int i = 0; i < MAX_INS; i++) /* el mismo archivo ya está pendiente */
		if (I.q[i].st == INS_PENDING && !strcasecmp(I.q[i].path, path))
			slot = -2;
	int id = -1;
	if (slot >= 0) {
		r->id = ++I.next_id;
		r->st = INS_PENDING;
		I.q[slot] = *r;
		id = r->id;
	}
	LightLock_Unlock(&I.lock);
	free(r);
	if (id < 0)
		util_strlcpy(err, slot == -2 ? "already" : "full", errn);
	return id;
}

void ins_decide(int id, int what)
{
	LightLock_Lock(&I.lock);
	for (int i = 0; i < MAX_INS; i++) {
		InsReq *r = &I.q[i];
		if (r->st != INS_PENDING || (id >= 0 && r->id != id))
			continue;
		/* ni Cloud3DS (se actualiza desde Ajustes) ni bajar de versión: se quedan guardados */
		if (what == INS_ACCEPT && !r->self && !(r->inst_ver >= 0 && r->inst_ver > r->ver))
			r->st = INS_QUEUED;
		else
			r->st = what == INS_REJECT ? INS_REJECTED : INS_KEPT, r->t_end = util_ms();
	}
	LightLock_Unlock(&I.lock);
	LightEvent_Signal(&I.ev);
}

int ins_pending(void)
{
	int n = 0;
	LightLock_Lock(&I.lock);
	for (int i = 0; i < MAX_INS; i++)
		n += I.q[i].st == INS_PENDING;
	LightLock_Unlock(&I.lock);
	return n;
}

bool ins_busy(void)
{
	bool b = false;
	LightLock_Lock(&I.lock);
	for (int i = 0; i < MAX_INS; i++)
		b |= I.q[i].st == INS_QUEUED || I.q[i].st == INS_RUNNING;
	LightLock_Unlock(&I.lock);
	return b;
}

int ins_snapshot(InsReq *out, int max)
{
	int n = 0;
	LightLock_Lock(&I.lock);
	/* por orden de llegada */
	for (int id = 0; id <= I.next_id && n < max; id++)
		for (int i = 0; i < MAX_INS; i++)
			if (I.q[i].st != INS_FREE && I.q[i].id == id && n < max)
				out[n++] = I.q[i];
	LightLock_Unlock(&I.lock);
	return n;
}

const char *ins_ver_text(u16 v, char *buf, size_t n)
{
	snprintf(buf, n, "%u.%u.%u", v >> 10, (v >> 4) & 0x3F, v & 0xF);
	return buf;
}

/* ---------- instalar ---------- */

static void set_progress(int id, u64 done)
{
	LightLock_Lock(&I.lock);
	InsReq *r = find(id);
	if (r)
		r->done = done;
	LightLock_Unlock(&I.lock);
}

/* seed de los juegos que la usan (de 2015 en adelante): sin ella no arrancan.
 * Como FBI: primero un archivo en la SD, si no, el servidor de Nintendo. */
static Result add_seed(u64 tid, const u8 *seed)
{
	u32 *cmd = getThreadCommandBuffer();
	cmd[0] = 0x087A0180;
	cmd[1] = (u32)tid;
	cmd[2] = (u32)(tid >> 32);
	memcpy(&cmd[3], seed, 16);
	Result r = svcSendSyncRequest(*fsGetSessionHandle());
	return R_FAILED(r) ? r : (Result)cmd[1];
}

static size_t seed_cb(char *d, size_t sz, size_t nm, void *u)
{
	u8 *b = u;
	size_t n = sz * nm;
	if (b[16] + n > 16)
		return 0;
	memcpy(b + b[16], d, n);
	b[16] += (u8)n;
	return n;
}

static void install_seed(u64 tid)
{
	u8 seed[17] = {0};
	char p[64];
	bool have = false;
	static const char *const DIRS[] = {"sdmc:/3ds/Cloud3DS/seeds", "sdmc:/fbi/seed"};
	for (int i = 0; i < 2 && !have; i++) {
		snprintf(p, sizeof p, "%s/%016llX.dat", DIRS[i], (unsigned long long)tid);
		FILE *f = fopen(p, "rb");
		if (f) {
			have = fread(seed, 1, 16, f) == 16;
			fclose(f);
		}
	}
	const char *from = have ? "SD" : "Nintendo";
	if (!have) {
		u8 region = CFG_REGION_USA;
		if (R_SUCCEEDED(cfguInit())) {
			CFGU_SecureInfoGetRegion(&region);
			cfguExit();
		}
		static const char *const RG[] = {"JP", "US", "GB", "GB", "HK", "KR", "TW"};
		if (region > CFG_REGION_TWN)
			return;
		char url[128];
		snprintf(url, sizeof url, "https://kagiya-ctr.cdn.nintendo.net/title/0x%016llX/ext_key?country=%s", (unsigned long long)tid,
		         RG[region]);
		CURL *c = curl_easy_init();
		if (!c)
			return;
		long code = 0;
		curl_easy_setopt(c, CURLOPT_URL, url);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "Cloud3DS/" APP_VERSION " (Nintendo 3DS; libcurl)");
		curl_easy_setopt(c, CURLOPT_CAINFO, "romfs:/certs/cacert.pem");
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, seed_cb);
		curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)seed);
		CURLcode cr = curl_easy_perform(c);
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_cleanup(c);
		have = cr == CURLE_OK && code == 200 && seed[16] == 16;
		if (!have) { /* la mayoría de juegos no tienen seed: es lo normal */
			util_log("instalar: sin seed para %016llX (curl %d, HTTP %ld)", (unsigned long long)tid, (int)cr, code);
			return;
		}
	}
	Result r = add_seed(tid, seed);
	util_log("instalar: seed de %016llX desde %s: 0x%08lX", (unsigned long long)tid, from, (unsigned long)r);
}

/* errores de AM en palabras (descripciones como las que documenta 3dsDL) */
static const char *am_error(Result r)
{
	int d = R_DESCRIPTION(r);
	if (d == 39)
		return "newer";
	if (d == 37)
		return "damaged";
	if (d == 4)
		return "am_busy";
	return "am";
}

static const char *install_one(const InsReq *r, int xid)
{
	u64 fr = 0, tot = 0;
	if (r->need && fsx_space(&fr, &tot) && fr < r->need) {
		util_log("instalar: hacen falta %llu bytes y quedan %llu", (unsigned long long)r->need, (unsigned long long)fr);
		return "no_space";
	}
	Handle in;
	if (R_FAILED(fsx_open(r->path, FS_OPEN_READ, &in)))
		return "not_found";
	u8 *buf = malloc(CHUNK);
	if (!buf || R_FAILED(amInit())) {
		free(buf);
		FSFILE_Close(in);
		return "am";
	}
	/* una SD sin ningún título instalado aún no tiene base de datos: se crea, como FBI */
	bool avail = true;
	if (R_SUCCEEDED(AM_QueryAvailableExternalTitleDatabase(&avail)) && !avail)
		AM_InitializeExternalTitleDatabase(false);
	Handle h = 0;
	Result res = AM_StartCiaInstall(MEDIATYPE_SD, &h);
	const char *e = NULL;
	if (R_FAILED(res)) {
		util_log("instalar: AM_StartCiaInstall 0x%08lX", (unsigned long)res);
		e = am_error(res);
		h = 0;
	}
	u64 off = 0;
	while (!e && off < r->size) {
		u32 got = 0, w = 0;
		u32 want = r->size - off > CHUNK ? CHUNK : (u32)(r->size - off);
		res = FSFILE_Read(in, &got, off, buf, want);
		if (R_FAILED(res) || !got) {
			e = "io";
			break;
		}
		res = FSFILE_Write(h, &w, off, buf, got, 0);
		if (R_FAILED(res) || w != got) {
			util_log("instalar: escribir 0x%08lX en %llu", (unsigned long)res, (unsigned long long)off);
			e = am_error(res);
			break;
		}
		off += got;
		set_progress(r->id, off);
		state_lock();
		xfer_progress(xid, off);
		state_unlock();
		if (I.quit)
			e = "canceled";
	}
	FSFILE_Close(in);
	free(buf);
	if (e) {
		if (h)
			AM_CancelCIAInstall(h);
	} else if (R_FAILED(res = AM_FinishCiaInstall(h))) {
		util_log("instalar: AM_FinishCiaInstall 0x%08lX", (unsigned long)res);
		e = am_error(res);
	}
	amExit();
	if (!e)
		install_seed(r->tid);
	return e;
}

static void ins_thread(void *arg)
{
	(void)arg;
	while (!I.quit) {
		LightEvent_Wait(&I.ev);
		for (;;) {
			InsReq r;
			bool got = false;
			LightLock_Lock(&I.lock);
			for (int i = 0; i < MAX_INS && !got; i++)
				if (I.q[i].st == INS_QUEUED) {
					I.q[i].st = INS_RUNNING;
					r = I.q[i];
					got = true;
				}
			LightLock_Unlock(&I.lock);
			if (!got || I.quit)
				break;
			state_lock();
			int xid = xfer_begin(XF_INSTALL, r.title, r.device, r.size);
			state_unlock();
			const char *e = install_one(&r, xid);
			util_log("instalar %s: %s", r.path, e ? e : "ok");
			state_lock();
			xfer_end(xid, e ? XS_FAILED : XS_DONE);
			if (!e)
				event_add(EV_INSTALL, r.title, r.device, r.size);
			state_unlock();
			LightLock_Lock(&I.lock);
			InsReq *q = find(r.id);
			if (q) {
				q->st = e ? INS_FAILED : INS_DONE;
				util_strlcpy(q->err, e ? e : "", sizeof q->err);
				q->t_end = util_ms();
				if (!e)
					q->inst_ver = q->ver;
			}
			LightLock_Unlock(&I.lock);
		}
	}
}

void ins_init(void)
{
	LightLock_Init(&I.lock);
	LightEvent_Init(&I.ev, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	I.thr = threadCreate(ins_thread, NULL, 32 * 1024, prio + 1, -2, false);
}

void ins_exit(void)
{
	I.quit = true;
	if (I.thr) {
		LightEvent_Signal(&I.ev);
		threadJoin(I.thr, 5000000000ull);
		threadFree(I.thr);
		I.thr = NULL;
	}
}
