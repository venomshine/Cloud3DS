/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "share.h"
#include "cJSON.h"
#include "dl.h"
#include "fsx.h"
#include "state.h"
#include "titles.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define SH_FILE APP_DIR "/compartir.json"
#define SH_REQS 16
#define SH_HIDE 64

enum { J_NONE, J_DISCOVER, J_PAIR, J_CATALOG, J_REQ };

typedef struct {
	int rid, kind; // SP_PAIR o SP_SEND
	int st;        // 0 esperando, 1 aceptado, 2 rechazado
	u64 t;
	// vincular: quién y lo que trae
	char pid[17], pname[48], code[8], tok_theirs[33], tok_mine[33];
	u32 ip;
	int port;
	// enviar: a quién y qué
	int peer;
	char path[256], name[64];
	u64 size;
	bool asked;    // (ya se le preguntó a la consola)
} Req;

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	int sock;
	char id[17];
	SharePeer p[SHARE_PEERS];
	u64 seen[SHARE_PEERS];
	int np;
	u32 rev;
	// lo que piden otras (servidor)
	Req rq[SH_REQS];
	int nrq, next_rid;
	// lo que ofrece esta consola
	ShareItem *mine;
	int nmine;
	u64 mine_at;
	char hide[SH_HIDE][256];
	int nhide;
	// trabajos de la pantalla (en el hilo)
	struct { int type, peer, item; } job[8];
	int nj;
	// vincular
	int pair_peer, pair_st;
	char pair_code[8];
	volatile bool pair_cancel;
	// catálogo de otra consola
	ShareItem *cat;
	int ncat, cat_peer;
	bool cat_loading, cat_failed;
} S = {.sock = -1, .pair_peer = -1, .cat_peer = -1};

static void lock(void) { LightLock_Lock(&S.lock); }
static void unlock(void) { LightLock_Unlock(&S.lock); }

static void rand_hex(char *out, int n) // n caracteres hexadecimales
{
	u8 b[32];
	int k = n / 2;
	if (R_SUCCEEDED(psInit())) {
		PS_GenerateRandomBytes(b, k);
		psExit();
	} else
		for (int i = 0; i < k; i++)
			b[i] = (u8)(svcGetSystemTick() >> (i % 8) ^ rand());
	for (int i = 0; i < k; i++)
		sprintf(out + i * 2, "%02x", b[i]);
	out[n] = 0;
}

/* ---------------------------------------------------------------- guardar */

static void save(void)
{
	cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray(), *h = cJSON_CreateArray();
	lock();
	cJSON_AddStringToObject(j, "id", S.id);
	for (int i = 0; i < S.np; i++) {
		const SharePeer *p = &S.p[i];
		if (!p->paired)
			continue;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "id", p->id);
		cJSON_AddStringToObject(e, "name", p->name);
		cJSON_AddNumberToObject(e, "ip", (double)p->ip);
		cJSON_AddNumberToObject(e, "port", p->port);
		cJSON_AddStringToObject(e, "out", p->tok_out);
		cJSON_AddStringToObject(e, "in", p->tok_in);
		cJSON_AddBoolToObject(e, "trusted", p->trusted);
		cJSON_AddItemToArray(a, e);
	}
	for (int i = 0; i < S.nhide; i++)
		cJSON_AddItemToArray(h, cJSON_CreateString(S.hide[i]));
	unlock();
	cJSON_AddItemToObject(j, "peers", a);
	cJSON_AddItemToObject(j, "hide", h);
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!s)
		return;
	FILE *f = fopen(SH_FILE ".tmp", "wb");
	if (f) {
		bool ok = fwrite(s, 1, strlen(s), f) == strlen(s);
		ok = fclose(f) == 0 && ok;
		if (ok) {
			remove(SH_FILE);
			rename(SH_FILE ".tmp", SH_FILE);
		}
	}
	free(s);
}

static void load(void)
{
	FILE *f = fopen(SH_FILE, "rb");
	char *b = NULL;
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		b = sz > 0 && sz < 256 * 1024 ? malloc(sz + 1) : NULL;
		size_t n = b ? fread(b, 1, sz, f) : 0;
		if (b)
			b[n] = 0;
		fclose(f);
	}
	cJSON *j = b ? cJSON_Parse(b) : NULL;
	free(b);
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id"), *a = cJSON_GetObjectItemCaseSensitive(j, "peers"),
	            *h = cJSON_GetObjectItemCaseSensitive(j, "hide"), *e;
	if (cJSON_IsString(id) && strlen(id->valuestring) == 16)
		util_strlcpy(S.id, id->valuestring, sizeof S.id);
	cJSON_ArrayForEach(e, a)
	{
		if (S.np >= SHARE_PEERS)
			break;
		SharePeer *p = &S.p[S.np];
		memset(p, 0, sizeof *p);
		const cJSON *pi = cJSON_GetObjectItemCaseSensitive(e, "id"), *pn = cJSON_GetObjectItemCaseSensitive(e, "name"),
		            *ip = cJSON_GetObjectItemCaseSensitive(e, "ip"), *po = cJSON_GetObjectItemCaseSensitive(e, "port"),
		            *to = cJSON_GetObjectItemCaseSensitive(e, "out"), *ti = cJSON_GetObjectItemCaseSensitive(e, "in"),
		            *tr = cJSON_GetObjectItemCaseSensitive(e, "trusted");
		if (!cJSON_IsString(pi) || !cJSON_IsString(to) || !cJSON_IsString(ti))
			continue;
		util_strlcpy(p->id, pi->valuestring, sizeof p->id);
		util_strlcpy(p->name, cJSON_IsString(pn) ? pn->valuestring : "3DS", sizeof p->name);
		p->ip = cJSON_IsNumber(ip) ? (u32)ip->valuedouble : 0;
		p->port = cJSON_IsNumber(po) ? po->valueint : 80;
		util_strlcpy(p->tok_out, to->valuestring, sizeof p->tok_out);
		util_strlcpy(p->tok_in, ti->valuestring, sizeof p->tok_in);
		p->trusted = cJSON_IsTrue(tr);
		p->paired = true;
		S.np++;
	}
	cJSON_ArrayForEach(e, h)
	{
		if (S.nhide < SH_HIDE && cJSON_IsString(e))
			util_strlcpy(S.hide[S.nhide++], e->valuestring, sizeof S.hide[0]);
	}
	cJSON_Delete(j);
	if (!S.id[0])
		rand_hex(S.id, 16);
}

/* ---------------------------------------------------------------- lo que se ofrece */

static bool rd(FILE *f, u64 off, void *b, size_t n)
{
	return fseek(f, (long)off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
}
static u32 le32(const u8 *b) { return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }
static u32 be32(const u8 *b) { return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static u64 align64(u64 x) { return (x + 63) & ~63ull; }

bool share_cia_homebrew(const char *path)
{
	char sp[300];
	snprintf(sp, sizeof sp, "sdmc:%s", path);
	FILE *f = fopen(sp, "rb");
	if (!f)
		return false;
	bool ok = false;
	u8 h[0x20];
	if (rd(f, 0, h, sizeof h)) {
		u32 hs = le32(h), cs = le32(h + 8), ts = le32(h + 12), ms = le32(h + 16);
		u64 tmd = align64(align64(align64(hs) + cs) + ts), con = align64(tmd + ms);
		u8 sig[4];
		if (hs == 0x2020 && rd(f, tmd, sig, 4)) {
			u32 st = be32(sig);
			u64 hdr = tmd + (st == 0x10004 || st == 0x10001 ? 0x140 : st == 0x10003 || st == 0x10000 ? 0x240 : 0x80);
			u8 th[0xC4], ch[0x30], nc[0x200];
			if (rd(f, hdr, th, sizeof th) && rd(f, hdr + 0xC4 + 0x900, ch, sizeof ch) && rd(f, con, nc, sizeof nc)) {
				u64 tid = (u64)be32(th + 0x4C) << 32 | be32(th + 0x50);
				u32 hi = (u32)(tid >> 32), uniq = ((u32)tid) >> 8;
				bool enc = (ch[7] & 1) != 0;                       // el contenido 0 (su tipo: 1 = cifrado con la clave del título)
				bool ncch = !memcmp(nc + 0x100, "NCCH", 4);
				bool nocrypto = ncch && (nc[0x18F] & 0x04);       // NCCH sin cifrar
				bool maker00 = ncch && nc[0x110] == '0' && nc[0x111] == '0'; // (el de makerom: el homebrew)
				ok = hi == 0x00040000 && !enc && nocrypto && (maker00 || (uniq >= 0xF0000 && uniq <= 0xFFFFF));
			}
		}
	}
	fclose(f);
	return ok;
}

typedef struct {
	char (*names)[256];
	bool *dirs;
	u64 *sizes;
	int n, cap;
} Ents;
static bool ents_cb(const FsxEntry *e, void *u)
{
	Ents *l = u;
	if (l->n == l->cap) {
		int nc = l->cap ? l->cap * 2 : 32;
		void *a = realloc(l->names, nc * sizeof *l->names), *b = realloc(l->dirs, nc * sizeof *l->dirs), *c = realloc(l->sizes, nc * sizeof *l->sizes);
		if (a)
			l->names = a;
		if (b)
			l->dirs = b;
		if (c)
			l->sizes = c;
		if (!a || !b || !c)
			return false;
		l->cap = nc;
	}
	util_strlcpy(l->names[l->n], e->name, sizeof l->names[0]);
	l->dirs[l->n] = e->dir, l->sizes[l->n] = e->size;
	l->n++;
	return true;
}

static bool hidden_path(const char *p)
{
	for (int i = 0; i < S.nhide; i++)
		if (!strcmp(S.hide[i], p))
			return true;
	return false;
}

// añade a out lo compartible de dir: .3dsx (también un nivel más abajo, como /3ds/App/App.3dsx) y .cia homebrew
static void scan_dir(const char *dir, ShareItem *out, int *n, int depth)
{
	Ents l = {0};
	fsx_list(dir, false, ents_cb, &l);
	for (int i = 0; i < l.n && *n < SHARE_ITEMS; i++) {
		char p[256];
		snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", l.names[i]);
		if (l.dirs[i]) {
			if (depth < 1 && strcasecmp(p, "/3ds/Cloud3DS") && strcasecmp(p, "/3ds/Checkpoint"))
				scan_dir(p, out, n, depth + 1);
			continue;
		}
		const char *x = strrchr(l.names[i], '.');
		int kind = x && !strcasecmp(x, ".3dsx") ? SK_3DSX : x && !strcasecmp(x, ".cia") ? SK_CIA : -1;
		if (kind < 0)
			continue;
		ShareItem *it = &out[*n];
		memset(it, 0, sizeof *it);
		util_strlcpy(it->path, p, sizeof it->path);
		it->kind = kind, it->size = l.sizes[i];
		if (kind == SK_3DSX)
			it->has_icon = smdh_3dsx(p, it->name, sizeof it->name, it->author, sizeof it->author, it->icon);
		else {
			if (!share_cia_homebrew(p))
				continue;
			it->has_icon = false;
		}
		if (!it->name[0]) {
			util_strlcpy(it->name, l.names[i], sizeof it->name);
			char *dot = strrchr(it->name, '.');
			if (dot)
				*dot = 0;
		}
		it->hidden = hidden_path(p);
		it->id = *n;
		(*n)++;
	}
	free(l.names), free(l.dirs), free(l.sizes);
}

static void refresh_mine(bool force)
{
	u64 now = util_ms();
	if (!force && S.mine && now - S.mine_at < 30000)
		return;
	ShareItem *out = malloc(sizeof(ShareItem) * SHARE_ITEMS);
	if (!out)
		return;
	int n = 0;
	static const char *const DIRS[] = {"/3ds", "/cias", "/CIAs", DL_DIR, "/"};
	for (size_t d = 0; d < sizeof DIRS / sizeof DIRS[0]; d++) {
		Ents l = {0}; // (la raíz: solo sus archivos, no se baja por todas sus carpetas)
		if (!strcmp(DIRS[d], "/")) {
			fsx_list("/", false, ents_cb, &l);
			for (int i = 0; i < l.n && n < SHARE_ITEMS; i++) {
				const char *x = strrchr(l.names[i], '.');
				if (l.dirs[i] || !x || (strcasecmp(x, ".cia") && strcasecmp(x, ".3dsx")))
					continue;
				char p[256];
				snprintf(p, sizeof p, "/%s", l.names[i]);
				ShareItem *it = &out[n];
				memset(it, 0, sizeof *it);
				util_strlcpy(it->path, p, sizeof it->path);
				it->kind = !strcasecmp(x, ".3dsx") ? SK_3DSX : SK_CIA, it->size = l.sizes[i];
				if (it->kind == SK_CIA && !share_cia_homebrew(p))
					continue;
				if (it->kind == SK_3DSX)
					it->has_icon = smdh_3dsx(p, it->name, sizeof it->name, it->author, sizeof it->author, it->icon);
				if (!it->name[0]) {
					util_strlcpy(it->name, l.names[i], sizeof it->name);
					char *dot = strrchr(it->name, '.');
					if (dot)
						*dot = 0;
				}
				it->hidden = hidden_path(p);
				it->id = n++;
			}
			free(l.names), free(l.dirs), free(l.sizes);
		} else
			scan_dir(DIRS[d], out, &n, DIRS[d][1] == '3' ? 0 : 1);
	}
	lock();
	free(S.mine);
	S.mine = out, S.nmine = n, S.mine_at = now;
	S.rev++;
	unlock();
}

/* ---------------------------------------------------------------- red */

static void peer_seen(const char *id, const char *name, u32 ip, int port)
{
	if (!id[0] || !strcmp(id, S.id))
		return;
	lock();
	int i = 0;
	while (i < S.np && strcmp(S.p[i].id, id))
		i++;
	if (i == S.np && S.np < SHARE_PEERS) {
		memset(&S.p[i], 0, sizeof S.p[i]);
		util_strlcpy(S.p[i].id, id, sizeof S.p[i].id);
		S.np++;
	}
	if (i < S.np) {
		SharePeer *p = &S.p[i];
		if (name && name[0])
			util_strlcpy(p->name, name, sizeof p->name);
		p->ip = ip, p->port = port ? port : 80, p->online = true;
		S.seen[i] = util_ms();
		S.rev++;
	}
	unlock();
}

static void udp_send(u32 ip, const char *msg)
{
	if (S.sock < 0)
		return;
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons(SHARE_PORT);
	a.sin_addr.s_addr = ip;
	sendto(S.sock, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
}

static void hello_msg(char *out, size_t n, char kind)
{
	state_lock();
	char nm[48];
	util_strlcpy(nm, G.console, sizeof nm);
	int port = G.port;
	state_unlock();
	cJSON *j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "id", S.id);
	cJSON_AddStringToObject(j, "name", nm);
	cJSON_AddNumberToObject(j, "port", port);
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	snprintf(out, n, "C3DS%c%s", kind, s ? s : "{}");
	free(s);
}

static void udp_read(void)
{
	char b[512];
	struct sockaddr_in from;
	socklen_t fl = sizeof from;
	int len = recvfrom(S.sock, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
	if (len < 5 || memcmp(b, "C3DS", 4))
		return;
	b[len] = 0;
	cJSON *j = cJSON_Parse(b + 5);
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id"), *nm = cJSON_GetObjectItemCaseSensitive(j, "name"),
	            *po = cJSON_GetObjectItemCaseSensitive(j, "port");
	if (cJSON_IsString(id) && strcmp(id->valuestring, S.id)) {
		peer_seen(id->valuestring, cJSON_IsString(nm) ? nm->valuestring : "", from.sin_addr.s_addr, cJSON_IsNumber(po) ? po->valueint : 80);
		if (b[4] == '?') { // nos buscan: contestamos a quien preguntó
			char m[256];
			hello_msg(m, sizeof m, '!');
			udp_send(from.sin_addr.s_addr, m);
		}
	}
	cJSON_Delete(j);
}

static void open_sock(void)
{
	if (S.sock >= 0 || !util_ip())
		return;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		return;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons(SHARE_PORT);
	a.sin_addr.s_addr = INADDR_ANY;
	if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
		closesocket(s);
		util_log("compartir: no se pudo abrir el puerto %d (%d)", SHARE_PORT, errno);
		return;
	}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	S.sock = s;
}

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
	b->p = np;
	memcpy(b->p + b->n, d, k);
	b->n += k;
	b->p[b->n] = 0;
	return k;
}

// petición a otra consola: /api/share/<what>?<query>; la respuesta (JSON o lo que sea) en out
static int peer_http(const SharePeer *p, int method, const char *what, const char *query, Mem *out, long *code)
{
	char url[700];
	struct in_addr ia = {p->ip};
	snprintf(url, sizeof url, "http://%s:%d/api/share/%s%s%s", inet_ntoa(ia), p->port ? p->port : 80, what, query && query[0] ? "?" : "", query ? query : "");
	const char *H[] = {"Content-Type: application/json", NULL};
	*code = 0;
	bool post = method == HTTPC_METHOD_POST; // (con un cuerpo mínimo: sin cuerpo, el servicio HTTP del sistema no manda el POST)
	return dl_request(method, url, post ? H : H + 1, post ? "{}" : NULL, post ? 2 : 0, mem_cb, out, code);
}

static void pct(const char *in, char *out, size_t n)
{
	size_t o = 0;
	for (const u8 *s = (const u8 *)in; *s && o + 4 < n; s++) {
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("-_.~", *s))
			out[o++] = (char)*s;
		else
			o += snprintf(out + o, n - o, "%%%02X", *s);
	}
	out[o] = 0;
}

/* ---------------------------------------------------------------- trabajos */

static void do_discover(void)
{
	open_sock();
	if (S.sock < 0)
		return;
	char m[256];
	hello_msg(m, sizeof m, '?');
	udp_send(0xFFFFFFFF, m); // a toda la red
	u32 me = util_ip();
	udp_send((me & 0x00FFFFFF) | 0xFF000000, m); // (y a la de /24, por si la difusión general no sale)
	lock();
	u32 ips[SHARE_PEERS];
	int n = 0;
	for (int i = 0; i < S.np; i++)
		if (S.p[i].ip)
			ips[n++] = S.p[i].ip;
	unlock();
	for (int i = 0; i < n; i++) // las ya conocidas, directamente
		udp_send(ips[i], m);
}

static void do_pair(int peer)
{
	SharePeer p;
	char mine[33];
	rand_hex(mine, 32); // con el que entrará ella aquí
	lock();
	bool ok = peer >= 0 && peer < S.np;
	if (ok)
		p = S.p[peer];
	char code[8];
	util_strlcpy(code, S.pair_code, sizeof code);
	unlock();
	if (!ok)
		return;
	state_lock();
	char nm[48], q[400], enm[150];
	util_strlcpy(nm, G.console, sizeof nm);
	int port = G.port;
	state_unlock();
	pct(nm, enm, sizeof enm);
	snprintf(q, sizeof q, "from=%s&name=%s&code=%s&tok=%s&port=%d", S.id, enm, code, mine, port);
	Mem m = {NULL, 0, 4096};
	long hc;
	int st = 4;
	if (peer_http(&p, HTTPC_METHOD_POST, "pair", q, &m, &hc) == DLF_OK && hc == 200 && m.p) {
		cJSON *j = cJSON_Parse(m.p);
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "rid");
		int rid = cJSON_IsNumber(r) ? r->valueint : -1;
		cJSON_Delete(j);
		for (int k = 0; rid >= 0 && k < 120 && !S.pair_cancel && !S.quit; k++) { // hasta 2 minutos a que la acepten
			svcSleepThread(1000000000ll);
			char q2[64];
			snprintf(q2, sizeof q2, "rid=%d", rid);
			free(m.p), m.p = NULL, m.n = 0;
			if (peer_http(&p, HTTPC_METHOD_GET, "pair_st", q2, &m, &hc) != DLF_OK || hc != 200 || !m.p)
				continue;
			cJSON *s = cJSON_Parse(m.p);
			const cJSON *ss = cJSON_GetObjectItemCaseSensitive(s, "st"), *tk = cJSON_GetObjectItemCaseSensitive(s, "tok");
			int v = cJSON_IsNumber(ss) ? ss->valueint : -1;
			if (v == 1 && cJSON_IsString(tk)) {
				lock();
				if (peer < S.np && !strcmp(S.p[peer].id, p.id)) {
					util_strlcpy(S.p[peer].tok_out, tk->valuestring, sizeof S.p[peer].tok_out);
					util_strlcpy(S.p[peer].tok_in, mine, sizeof S.p[peer].tok_in);
					S.p[peer].paired = true;
				}
				unlock();
				st = 2;
			} else if (v == 2 || v < 0)
				st = 3;
			cJSON_Delete(s);
			if (v != 0)
				break;
		}
		if (S.pair_cancel)
			st = 0;
	}
	free(m.p);
	lock();
	S.pair_st = st;
	S.rev++;
	unlock();
	if (st == 2)
		save();
}

static void do_catalog(int peer)
{
	SharePeer p;
	lock();
	bool ok = peer >= 0 && peer < S.np && S.p[peer].paired;
	if (ok)
		p = S.p[peer];
	unlock();
	ShareItem *out = ok ? malloc(sizeof(ShareItem) * SHARE_ITEMS) : NULL;
	int n = 0;
	char q[64];
	snprintf(q, sizeof q, "t=%s", p.tok_out);
	Mem m = {NULL, 0, 256 * 1024};
	long hc;
	ok = out && peer_http(&p, HTTPC_METHOD_GET, "catalog", q, &m, &hc) == DLF_OK && hc == 200 && m.p;
	if (ok) {
		cJSON *j = cJSON_Parse(m.p), *e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "items"))
		{
			if (n >= SHARE_ITEMS)
				break;
			ShareItem *it = &out[n];
			memset(it, 0, sizeof *it);
			const cJSON *i = cJSON_GetObjectItemCaseSensitive(e, "i"), *nm = cJSON_GetObjectItemCaseSensitive(e, "n"), *au = cJSON_GetObjectItemCaseSensitive(e, "a"),
			            *k = cJSON_GetObjectItemCaseSensitive(e, "k"), *sz = cJSON_GetObjectItemCaseSensitive(e, "s"), *ic = cJSON_GetObjectItemCaseSensitive(e, "ic");
			if (!cJSON_IsNumber(i) || !cJSON_IsString(nm))
				continue;
			it->id = i->valueint;
			util_strlcpy(it->name, nm->valuestring, sizeof it->name);
			if (cJSON_IsString(au))
				util_strlcpy(it->author, au->valuestring, sizeof it->author);
			it->kind = cJSON_IsNumber(k) ? k->valueint : SK_3DSX;
			it->size = cJSON_IsNumber(sz) ? (u64)sz->valuedouble : 0;
			it->has_icon = cJSON_IsTrue(ic);
			n++;
		}
		cJSON_Delete(j);
		// los iconos, uno a uno
		for (int k = 0; k < n && !S.quit; k++) {
			if (!out[k].has_icon)
				continue;
			char q2[96];
			snprintf(q2, sizeof q2, "t=%s&i=%d", p.tok_out, out[k].id);
			Mem im = {NULL, 0, 48 * 48 * 2 + 16};
			if (peer_http(&p, HTTPC_METHOD_GET, "icon", q2, &im, &hc) == DLF_OK && hc == 200 && im.n == 48 * 48 * 2)
				memcpy(out[k].icon, im.p, 48 * 48 * 2);
			else
				out[k].has_icon = false;
			free(im.p);
		}
	}
	free(m.p);
	lock();
	if (S.cat_peer == peer) {
		free(S.cat);
		S.cat = ok ? out : NULL, S.ncat = ok ? n : 0, out = NULL;
		S.cat_loading = false, S.cat_failed = !ok;
		S.rev++;
	}
	unlock();
	free(out);
}

static void set_item_st(int peer, int item, int st)
{
	lock();
	if (S.cat_peer == peer)
		for (int k = 0; k < S.ncat; k++)
			if (S.cat[k].id == item)
				S.cat[k].st = st;
	S.rev++;
	unlock();
}

static void do_req(int peer, int item)
{
	SharePeer p;
	ShareItem it = {0};
	lock();
	bool ok = peer >= 0 && peer < S.np && S.p[peer].paired;
	if (ok)
		p = S.p[peer];
	for (int k = 0; k < S.ncat; k++)
		if (S.cat[k].id == item)
			it = S.cat[k];
	unlock();
	if (!ok)
		return;
	set_item_st(peer, item, SHR_WAIT);
	char q[96];
	snprintf(q, sizeof q, "t=%s&i=%d", p.tok_out, item);
	Mem m = {NULL, 0, 4096};
	long hc;
	int rid = -1;
	if (peer_http(&p, HTTPC_METHOD_POST, "req", q, &m, &hc) == DLF_OK && hc == 200 && m.p) {
		cJSON *j = cJSON_Parse(m.p);
		const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "rid");
		rid = cJSON_IsNumber(r) ? r->valueint : -1;
		cJSON_Delete(j);
	}
	int v = rid >= 0 ? 0 : -1;
	for (int k = 0; v == 0 && k < 180 && !S.quit; k++) { // hasta 3 minutos a que lo acepten
		char q2[96];
		snprintf(q2, sizeof q2, "t=%s&rid=%d", p.tok_out, rid);
		free(m.p), m.p = NULL, m.n = 0;
		if (peer_http(&p, HTTPC_METHOD_GET, "req_st", q2, &m, &hc) == DLF_OK && hc == 200 && m.p) {
			cJSON *s = cJSON_Parse(m.p);
			const cJSON *ss = cJSON_GetObjectItemCaseSensitive(s, "st");
			v = cJSON_IsNumber(ss) ? ss->valueint : -1;
			cJSON_Delete(s);
		}
		if (v == 0)
			svcSleepThread(1000000000ll);
	}
	free(m.p);
	if (v != 1) {
		set_item_st(peer, item, v == 2 ? SHR_REFUSED : SHR_FAIL);
		return;
	}
	// aceptado: se baja como cualquier descarga (un .3dsx, a /3ds; un CIA, a /Downloads y a instalar)
	char url[400];
	struct in_addr ia = {p.ip};
	snprintf(url, sizeof url, "http://%s:%d/api/share/file?t=%s&rid=%d", inet_ntoa(ia), p.port ? p.port : 80, p.tok_out, rid);
	char fname[80];
	snprintf(fname, sizeof fname, "%s.%s", it.name[0] ? it.name : "app", it.kind == SK_CIA ? "cia" : "3dsx");
	for (char *c = fname; *c; c++)
		if (strchr("\\/:*?\"<>|", *c))
			*c = '_';
	const char *dir = it.kind == SK_CIA ? DL_DIR : "/3ds";
	fsx_mkdirs(dir);
	u32 did = 0;
	if (dl_add(url, dir, "", it.kind == SK_CIA ? DLA_INSTALL : DLA_NONE, p.name, &did) == 0) {
		dl_set_name(did, fname);
		set_item_st(peer, item, SHR_DOWNLOADING);
	} else
		set_item_st(peer, item, SHR_FAIL);
}

static void push(int type, int peer, int item)
{
	lock();
	if (S.nj < 8)
		S.job[S.nj].type = type, S.job[S.nj].peer = peer, S.job[S.nj].item = item, S.nj++;
	unlock();
	LightEvent_Signal(&S.ev);
}

static void thread_main(void *arg)
{
	(void)arg;
	u64 last_disc = 0;
	while (!S.quit) {
		open_sock();
		if (S.sock >= 0) {
			struct pollfd pf = {S.sock, POLLIN, 0};
			while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN))
				udp_read();
		}
		lock();
		bool have = S.nj > 0;
		int type = 0, peer = 0, item = 0;
		if (have) {
			type = S.job[0].type, peer = S.job[0].peer, item = S.job[0].item;
			memmove(S.job, S.job + 1, sizeof S.job[0] * --S.nj);
		}
		// las que no se ven hace más de 1 minuto: sin conexión
		u64 now = util_ms();
		for (int i = 0; i < S.np; i++)
			if (S.p[i].online && now - S.seen[i] > 60000)
				S.p[i].online = false, S.rev++;
		unlock();
		if (have) {
			if (type == J_DISCOVER)
				do_discover(), last_disc = now;
			else if (type == J_PAIR)
				do_pair(peer);
			else if (type == J_CATALOG)
				do_catalog(peer);
			else if (type == J_REQ)
				do_req(peer, item);
			continue;
		}
		if (last_disc && now - last_disc > 20000) // mientras se usa, se vuelve a preguntar de vez en cuando
			do_discover(), last_disc = now;
		LightEvent_WaitTimeout(&S.ev, 150 * 1000000ll);
	}
}

/* ---------------------------------------------------------------- uso */

void share_init(void)
{
	if (S.thr)
		return;
	LightLock_Init(&S.lock);
	LightEvent_Init(&S.ev, RESET_ONESHOT);
	load();
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	S.thr = threadCreate(thread_main, NULL, 32 * 1024, prio + 1, -2, false);
}

void share_exit(void)
{
	S.quit = true;
	if (S.thr) {
		LightEvent_Signal(&S.ev);
		threadJoin(S.thr, 3000000000ull);
		threadFree(S.thr);
		S.thr = NULL;
	}
	if (S.sock >= 0)
		closesocket(S.sock), S.sock = -1;
}

const char *share_my_name(void)
{
	return G.console;
}

void share_discover(void)
{
	push(J_DISCOVER, 0, 0);
}

bool share_add_ip(const char *ip)
{
	struct in_addr a;
	if (!inet_aton(ip, &a))
		return false;
	open_sock();
	char m[256];
	hello_msg(m, sizeof m, '?');
	udp_send(a.s_addr, m); // (contesta con su id y su nombre, y entra en la lista)
	return true;
}

int share_peers(SharePeer *out, int max)
{
	lock();
	int n = S.np < max ? S.np : max;
	memcpy(out, S.p, n * sizeof *out);
	unlock();
	return n;
}

u32 share_rev(void)
{
	return S.rev;
}

bool share_pair(int peer, char *code, size_t cn)
{
	lock();
	bool ok = peer >= 0 && peer < S.np && S.pair_st != 1;
	if (ok) {
		snprintf(S.pair_code, sizeof S.pair_code, "%04u", (unsigned)(svcGetSystemTick() % 10000));
		util_strlcpy(code, S.pair_code, cn);
		S.pair_peer = peer, S.pair_st = 1, S.pair_cancel = false;
		S.rev++;
	}
	unlock();
	if (ok)
		push(J_PAIR, peer, 0);
	return ok;
}

int share_pair_state(int *peer, char *code, size_t cn)
{
	lock();
	int st = S.pair_st;
	if (peer)
		*peer = S.pair_peer;
	if (code)
		util_strlcpy(code, S.pair_code, cn);
	unlock();
	return st;
}

void share_pair_cancel(void)
{
	S.pair_cancel = true;
	lock();
	if (S.pair_st != 1)
		S.pair_st = 0;
	unlock();
}

void share_set_trusted(int peer, bool on)
{
	lock();
	if (peer >= 0 && peer < S.np)
		S.p[peer].trusted = on;
	S.rev++;
	unlock();
	save();
}

void share_unpair(int peer)
{
	lock();
	if (peer >= 0 && peer < S.np) {
		S.p[peer].paired = S.p[peer].trusted = false;
		S.p[peer].tok_in[0] = S.p[peer].tok_out[0] = 0;
	}
	S.rev++;
	unlock();
	save();
}

void share_open_catalog(int peer)
{
	lock();
	S.cat_peer = peer;
	free(S.cat);
	S.cat = NULL, S.ncat = 0, S.cat_loading = true, S.cat_failed = false;
	S.rev++;
	unlock();
	push(J_CATALOG, peer, 0);
}

int share_catalog(ShareItem *out, int max, int *peer, bool *loading, bool *failed)
{
	lock();
	int n = S.ncat < max ? S.ncat : max;
	if (n)
		memcpy(out, S.cat, n * sizeof *out);
	*peer = S.cat_peer, *loading = S.cat_loading, *failed = S.cat_failed;
	unlock();
	return n;
}

void share_request(int peer, int item)
{
	push(J_REQ, peer, item);
}

int share_mine(ShareItem *out, int max)
{
	refresh_mine(false);
	lock();
	int n = S.nmine < max ? S.nmine : max;
	if (n)
		memcpy(out, S.mine, n * sizeof *out);
	unlock();
	return n;
}

void share_hide(const char *path, bool hide)
{
	lock();
	int i = 0;
	while (i < S.nhide && strcmp(S.hide[i], path))
		i++;
	if (hide && i == S.nhide && S.nhide < SH_HIDE)
		util_strlcpy(S.hide[S.nhide++], path, sizeof S.hide[0]);
	else if (!hide && i < S.nhide)
		memmove(S.hide[i], S.hide[i + 1], (S.nhide - i - 1) * sizeof S.hide[0]), S.nhide--;
	for (int k = 0; k < S.nmine; k++)
		if (!strcmp(S.mine[k].path, path))
			S.mine[k].hidden = hide;
	S.rev++;
	unlock();
	save();
}

bool share_prompt(SharePrompt *out)
{
	bool got = false;
	lock();
	for (int i = 0; i < S.nrq && !got; i++) {
		Req *r = &S.rq[i];
		if (r->st || r->asked || util_ms() - r->t > 110000)
			continue;
		r->asked = true;
		memset(out, 0, sizeof *out);
		out->rid = r->rid, out->kind = r->kind, out->size = r->size;
		util_strlcpy(out->who, r->kind == SP_PAIR ? r->pname : r->peer >= 0 && r->peer < S.np ? S.p[r->peer].name : "3DS", sizeof out->who);
		util_strlcpy(out->code, r->code, sizeof out->code);
		util_strlcpy(out->item, r->name, sizeof out->item);
		got = true;
	}
	unlock();
	return got;
}

void share_answer(int rid, bool yes)
{
	bool saved = false;
	lock();
	for (int i = 0; i < S.nrq; i++) {
		Req *r = &S.rq[i];
		if (r->rid != rid || r->st)
			continue;
		r->st = yes ? 1 : 2;
		if (yes && r->kind == SP_PAIR) { // vinculada: entra en la lista con los dos tokens
			rand_hex(r->tok_mine, 32);
			int k = 0;
			while (k < S.np && strcmp(S.p[k].id, r->pid))
				k++;
			if (k == S.np && S.np < SHARE_PEERS)
				memset(&S.p[k], 0, sizeof S.p[k]), S.np++;
			if (k < S.np) {
				SharePeer *p = &S.p[k];
				util_strlcpy(p->id, r->pid, sizeof p->id);
				util_strlcpy(p->name, r->pname, sizeof p->name);
				p->ip = r->ip, p->port = r->port, p->paired = true, p->online = true;
				util_strlcpy(p->tok_out, r->tok_theirs, sizeof p->tok_out);
				util_strlcpy(p->tok_in, r->tok_mine, sizeof p->tok_in);
				S.seen[k] = util_ms();
				saved = true;
			}
		}
	}
	S.rev++;
	unlock();
	if (saved)
		save();
}

/* ---------------------------------------------------------------- servidor */

static Req *new_req(void)
{
	if (S.nrq == SH_REQS) // la más vieja sale
		memmove(S.rq, S.rq + 1, sizeof S.rq[0] * --S.nrq);
	Req *r = &S.rq[S.nrq++];
	memset(r, 0, sizeof *r);
	r->rid = ++S.next_rid;
	r->t = util_ms();
	return r;
}

void share_srv_hello(char *out, size_t n)
{
	hello_msg(out, n, '!');
}

int share_srv_pair(const char *from, const char *name, const char *code, const char *tok, u32 ip, int port)
{
	if (strlen(from) != 16 || strlen(tok) != 32 || strlen(code) != 4)
		return -1;
	lock();
	Req *r = new_req();
	r->kind = SP_PAIR;
	util_strlcpy(r->pid, from, sizeof r->pid);
	util_strlcpy(r->pname, name[0] ? name : "3DS", sizeof r->pname);
	util_strlcpy(r->code, code, sizeof r->code);
	util_strlcpy(r->tok_theirs, tok, sizeof r->tok_theirs);
	r->ip = ip, r->port = port ? port : 80;
	int rid = r->rid;
	S.rev++;
	unlock();
	return rid;
}

static Req *find_req(int rid)
{
	for (int i = 0; i < S.nrq; i++)
		if (S.rq[i].rid == rid)
			return &S.rq[i];
	return NULL;
}

int share_srv_pair_st(int rid, char *tok, size_t tn)
{
	lock();
	Req *r = find_req(rid);
	int st = !r || r->kind != SP_PAIR ? -1 : r->st == 0 && util_ms() - r->t > 120000 ? 2 : r->st;
	if (st == 1)
		util_strlcpy(tok, r->tok_mine, tn);
	unlock();
	return st;
}

int share_srv_auth(const char *tok, u32 ip)
{
	if (!tok || strlen(tok) != 32)
		return -1;
	lock();
	int k = -1;
	for (int i = 0; i < S.np && k < 0; i++)
		if (S.p[i].paired && !strcmp(S.p[i].tok_in, tok))
			k = i;
	if (k >= 0 && ip) { // (su IP de ahora, por si cambió)
		S.p[k].ip = ip, S.p[k].online = true;
		S.seen[k] = util_ms();
	}
	unlock();
	return k;
}

char *share_srv_catalog(int peer)
{
	(void)peer;
	refresh_mine(false);
	cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
	lock();
	for (int i = 0; i < S.nmine; i++) {
		const ShareItem *it = &S.mine[i];
		if (it->hidden)
			continue;
		cJSON *e = cJSON_CreateObject();
		cJSON_AddNumberToObject(e, "i", it->id);
		cJSON_AddStringToObject(e, "n", it->name);
		cJSON_AddStringToObject(e, "a", it->author);
		cJSON_AddNumberToObject(e, "k", it->kind);
		cJSON_AddNumberToObject(e, "s", (double)it->size);
		cJSON_AddBoolToObject(e, "ic", it->has_icon);
		cJSON_AddItemToArray(a, e);
	}
	unlock();
	cJSON_AddItemToObject(j, "items", a);
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	return s;
}

static ShareItem *mine_item(int item)
{
	for (int i = 0; i < S.nmine; i++)
		if (S.mine[i].id == item && !S.mine[i].hidden)
			return &S.mine[i];
	return NULL;
}

bool share_srv_icon(int peer, int item, u16 *px)
{
	(void)peer;
	lock();
	ShareItem *it = mine_item(item);
	bool ok = it && it->has_icon;
	if (ok)
		memcpy(px, it->icon, sizeof it->icon);
	unlock();
	return ok;
}

int share_srv_req(int peer, int item)
{
	lock();
	ShareItem *it = mine_item(item);
	int rid = -1;
	if (it && peer >= 0 && peer < S.np) {
		Req *r = new_req();
		r->kind = SP_SEND, r->peer = peer;
		util_strlcpy(r->path, it->path, sizeof r->path);
		util_strlcpy(r->name, it->name, sizeof r->name);
		r->size = it->size;
		if (S.p[peer].trusted) // de confianza: sin preguntar
			r->st = 1, r->asked = true;
		rid = r->rid;
		S.rev++;
	}
	unlock();
	return rid;
}

int share_srv_req_st(int peer, int rid)
{
	lock();
	Req *r = find_req(rid);
	int st = !r || r->kind != SP_SEND || r->peer != peer ? -1 : r->st == 0 && util_ms() - r->t > 180000 ? 2 : r->st;
	unlock();
	return st;
}

bool share_srv_file(int peer, int rid, char *path, size_t n, char *who, size_t wn)
{
	lock();
	Req *r = find_req(rid);
	bool ok = r && r->kind == SP_SEND && r->peer == peer && r->st == 1;
	if (ok) {
		util_strlcpy(path, r->path, n);
		util_strlcpy(who, peer < S.np ? S.p[peer].name : "3DS", wn);
	}
	unlock();
	if (!ok)
		return false;
	// justo antes de enviar, otra vez: que siga siendo homebrew (un .3dsx, o un CIA sin cifrar)
	const char *x = strrchr(path, '.');
	return x && (!strcasecmp(x, ".3dsx") || (!strcasecmp(x, ".cia") && share_cia_homebrew(path)));
}
