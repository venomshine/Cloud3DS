/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#define _GNU_SOURCE
#include "server.h"
#include "fsx.h"
#include "ops.h"
#include "names.h"
#include "install.h"
#include "state.h"
#include "util.h"
#include "dl.h"
#include "hash.h"
#include "music.h"
#include "live.h"
#include "titles.h"
#include "remote.h"
#include "zipjob.h"
#include "search.h"
#include "trash.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOC_BUFSIZE  0x100000
#define MAX_CONN     8
#define IN_MAX       8192
#define SMALL_BODY   4096
#define FILE_CHUNK   (64 * 1024)
#define UP_BUF       (256 * 1024)
#define SYS_PORTS    {80, 8080, 5000}

typedef struct {
	char *p;
	size_t len, cap;
} Buf;

typedef enum { CS_FREE, CS_HEAD, CS_BODY, CS_UPLOAD, CS_SEND } CState;

typedef struct {
	CState st;
	int fd;
	char ip[16];
	u64 last_io;
	bool any_data;

	char in[IN_MAX + 1];
	int in_len, hdr_len;

	/* petición */
	char method[8];
	char target[2048];
	char *path, *query;
	u64 clen;
	bool has_clen, keepalive, expect100;
	char token[33];
	char ua[192];
	char range[64];
	int dev; /* índice en G.dev o -1 */

	/* respuesta */
	Buf out;
	size_t out_off;
	int ffd;
	u64 f_left;
	int xid;
	u64 x_done;
	struct ZipStream *zs; /* descarga como ZIP, generado mientras se manda */

	/* subida */
	int ufd;
	u64 u_total, u_done;
	bool u_over;
	char u_tmp[1024], u_final[1024], u_name[256];
	bool u_edit; /* guardado desde el editor de texto de la web */
	u8 *ubuf;
	size_t ubuf_len;
} Conn;

static Conn conns[MAX_CONN];
static int lfd = -1;
static Thread thr;
static volatile bool quit, restart;
static u32 *soc_buf;
static bool soc_ok;
static u8 *page;
static size_t page_len;
static u8 *fbuf; /* lectura de archivos compartida: un envío por vuelta */

/* ---------- utilidades ---------- */

static void buf_need(Buf *b, size_t extra)
{
	if (b->len + extra + 1 <= b->cap)
		return;
	size_t nc = b->cap ? b->cap : 4096;
	while (nc < b->len + extra + 1)
		nc *= 2;
	char *np = realloc(b->p, nc);
	if (!np)
		return;
	b->p = np;
	b->cap = nc;
}

static void buf_add(Buf *b, const void *d, size_t n)
{
	buf_need(b, n);
	if (b->len + n + 1 > b->cap)
		return;
	memcpy(b->p + b->len, d, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void buf_printf(Buf *b, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0)
		buf_add(b, tmp, n < (int)sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void buf_json_str(Buf *b, const char *s)
{
	buf_puts(b, "\"");
	for (const u8 *p = (const u8 *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			char e[2] = {'\\', (char)*p};
			buf_add(b, e, 2);
		} else if (*p < 0x20) {
			buf_printf(b, "\\u%04x", *p);
		} else {
			buf_add(b, p, 1);
		}
	}
	buf_puts(b, "\"");
}

static bool qget(const char *q, const char *key, char *out, size_t n)
{
	if (!q)
		return false;
	size_t kl = strlen(key);
	const char *p = q;
	while (p && *p) {
		if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
			p += kl + 1;
			const char *e = strchr(p, '&');
			size_t l = e ? (size_t)(e - p) : strlen(p);
			if (l >= n)
				return false;
			memcpy(out, p, l);
			out[l] = 0;
			return util_url_decode(out) == 0;
		}
		p = strchr(p, '&');
		if (p)
			p++;
	}
	return false;
}

static bool qflag(const char *q, const char *key)
{
	char v[8];
	return qget(q, key, v, sizeof v) && strcmp(v, "1") == 0;
}

static const char *mime_of(const char *name)
{
	static const struct {
		const char *ext, *mime;
	} M[] = {
	    {"png", "image/png"},        {"jpg", "image/jpeg"},       {"jpeg", "image/jpeg"},
	    {"gif", "image/gif"},        {"bmp", "image/bmp"},        {"webp", "image/webp"},
	    {"mp4", "video/mp4"},        {"webm", "video/webm"},      {"mp3", "audio/mpeg"},
	    {"wav", "audio/wav"},        {"ogg", "audio/ogg"},        {"txt", "text/plain; charset=utf-8"},
	    {"ini", "text/plain; charset=utf-8"}, {"log", "text/plain; charset=utf-8"},
	    {"json", "application/json"}, {"cfg", "text/plain; charset=utf-8"},
	    {"xml", "text/plain; charset=utf-8"}, {"pdf", "application/pdf"},
	    {"mpo", "image/jpeg"}, /* foto 3D de la cámara: el navegador enseña la primera imagen */
	    {"m4a", "audio/mp4"},        {"flac", "audio/flac"},      {"mov", "video/quicktime"},
	    {"md", "text/plain; charset=utf-8"},  {"csv", "text/plain; charset=utf-8"},
	    {"yml", "text/plain; charset=utf-8"}, {"yaml", "text/plain; charset=utf-8"},
	    {"conf", "text/plain; charset=utf-8"}, {"gm9", "text/plain; charset=utf-8"},
	    {"lua", "text/plain; charset=utf-8"}, {"srt", "text/plain; charset=utf-8"},
	    {"nfo", "text/plain; charset=utf-8"}, {"toml", "text/plain; charset=utf-8"},
	};
	const char *dot = strrchr(name, '.');
	if (dot)
		for (size_t i = 0; i < ARRAY_LEN(M); i++)
			if (strcasecmp(dot + 1, M[i].ext) == 0)
				return M[i].mime;
	return "application/octet-stream";
}

static void parse_ua(const char *ua, char *name, size_t n, int *kind)
{
	const char *os = "Navegador";
	*kind = 1;
	if (strstr(ua, "iPhone"))
		os = "iPhone", *kind = 0;
	else if (strstr(ua, "iPad"))
		os = "iPad", *kind = 2;
	else if (strstr(ua, "Android"))
		os = "Android", *kind = strstr(ua, "Mobile") ? 0 : 2;
	else if (strstr(ua, "Windows"))
		os = "Windows";
	else if (strstr(ua, "CrOS"))
		os = "Chromebook";
	else if (strstr(ua, "Macintosh"))
		os = "Mac";
	else if (strstr(ua, "Linux"))
		os = "Linux";
	else if (strstr(ua, "Nintendo"))
		os = "Nintendo", *kind = 0;

	const char *br = NULL;
	if (strstr(ua, "Edg"))
		br = "Edge";
	else if (strstr(ua, "OPR") || strstr(ua, "Opera"))
		br = "Opera";
	else if (strstr(ua, "SamsungBrowser"))
		br = "Samsung Internet";
	else if (strstr(ua, "Firefox") || strstr(ua, "FxiOS"))
		br = "Firefox";
	else if (strstr(ua, "Chrome") || strstr(ua, "CriOS"))
		br = "Chrome";
	else if (strstr(ua, "Safari"))
		br = "Safari";
	else if (strstr(ua, "curl"))
		br = "curl";
	if (br)
		snprintf(name, n, "%s · %s", os, br);
	else
		util_strlcpy(name, os, n);
}

static void sdpath(char *out, size_t n, const char *p)
{
	snprintf(out, n, "sdmc:%s", p);
}

static const char *basename_of(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void parent_of(char *out, size_t n, const char *p)
{
	util_strlcpy(out, p, n);
	char *s = strrchr(out, '/');
	if (s == out)
		out[1] = 0;
	else if (s)
		*s = 0;
}

/* ---------- respuestas ---------- */

static const char *status_text(int code)
{
	switch (code) {
	case 100: return "Continue";
	case 200: return "OK";
	case 204: return "No Content";
	case 206: return "Partial Content";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 416: return "Range Not Satisfiable";
	case 429: return "Too Many Requests";
	case 507: return "Insufficient Storage";
	default:  return code >= 500 ? "Internal Server Error" : "Error";
	}
}

static void resp_head(Conn *c, int code, const char *ctype, u64 clen, const char *extra)
{
	c->out.len = 0;
	c->out_off = 0;
	buf_printf(&c->out,
	           "HTTP/1.1 %d %s\r\n"
	           "Server: Cloud3DS\r\n"
	           "Content-Type: %s\r\n"
	           "Content-Length: %llu\r\n"
	           "Connection: %s\r\n"
	           "Cache-Control: no-store\r\n"
	           "X-Content-Type-Options: nosniff\r\n",
	           code, status_text(code), ctype, (unsigned long long)clen,
	           c->keepalive ? "keep-alive" : "close");
	if (extra)
		buf_puts(&c->out, extra);
	buf_puts(&c->out, "\r\n");
	c->st = CS_SEND;
}

static void resp(Conn *c, int code, const char *ctype, const void *body, size_t n, const char *extra)
{
	resp_head(c, code, ctype, n, extra);
	if (n && strcmp(c->method, "HEAD") != 0)
		buf_add(&c->out, body, n);
}

static void resp_json(Conn *c, int code, Buf *b)
{
	resp(c, code, "application/json; charset=utf-8", b->p ? b->p : "{}", b->p ? b->len : 2, NULL);
}

static void resp_err(Conn *c, int code, const char *err)
{
	char j[128];
	int n = snprintf(j, sizeof j, "{\"ok\":false,\"error\":\"%s\"}", err);
	resp(c, code, "application/json; charset=utf-8", j, n, NULL);
}

static void resp_ok(Conn *c)
{
	resp(c, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, NULL);
}

/* ---------- conexiones ---------- */

static void zs_free(Conn *c);

static void conn_close(Conn *c)
{
	if (c->fd >= 0)
		closesocket(c->fd);
	if (c->ffd >= 0)
		close(c->ffd);
	if (c->ufd >= 0) {
		/* subida a medias: borra el temporal */
		close(c->ufd);
		unlink(c->u_tmp);
		state_lock();
		Device *d = c->dev >= 0 ? &G.dev[c->dev] : NULL;
		xfer_end(c->xid, XS_CANCELED);
		event_add(EV_UPLOAD_FAIL, c->u_name, d ? d->name : "", c->u_total);
		state_unlock();
		util_log("upload cancelada: %s (%llu/%llu)", c->u_final, (unsigned long long)c->u_done,
		         (unsigned long long)c->u_total);
	} else if (c->xid) {
		state_lock();
		xfer_end(c->xid, XS_CANCELED);
		state_unlock();
	}
	zs_free(c);
	free(c->out.p);
	free(c->ubuf);
	memset(c, 0, sizeof *c);
	c->fd = c->ffd = c->ufd = -1;
	c->st = CS_FREE;
}

static void conn_reset_req(Conn *c)
{
	c->method[0] = 0;
	c->path = c->query = NULL;
	c->clen = 0;
	c->has_clen = c->expect100 = false;
	c->token[0] = c->ua[0] = c->range[0] = 0;
	c->dev = -1;
	c->out_off = 0;
	c->out.len = 0;
	c->f_left = 0;
	c->xid = 0;
	c->x_done = 0;
}

/* ---------- autenticación ---------- */

static int find_dev(const char *tok)
{
	if (strlen(tok) != 32)
		return -1;
	for (int i = 0; i < MAX_DEVICES; i++)
		if (G.dev[i].used && strcmp(G.dev[i].token, tok) == 0)
			return i;
	return -1;
}

static int add_dev(const char *ua, const char *ip)
{
	int slot = -1;
	u64 oldest = ~0ull;
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (!G.dev[i].used) {
			slot = i;
			break;
		}
		if (G.dev[i].last_seen < oldest) {
			oldest = G.dev[i].last_seen;
			slot = i;
		}
	}
	Device *d = &G.dev[slot];
	memset(d, 0, sizeof *d);
	d->used = true;
	util_rand_hex(d->token, 32);
	parse_ua(ua, d->model, sizeof d->model, &d->kind);
	util_strlcpy(d->name, d->model, sizeof d->name); /* hasta que se encuentre su nombre en la red */
	util_strlcpy(d->ip, ip, sizeof d->ip);
	d->created = d->last_seen = util_unix_now();
	d->last_ms = util_ms();
	G.dirty_cfg = true;
	G.rev++;
	return slot;
}

static u64 lock_until;

static void h_login(Conn *c, const char *body)
{
	char k[16] = "", pin[8] = "";
	qget(body, "k", k, sizeof k);
	qget(body, "pin", pin, sizeof pin);
	u64 now = util_ms();
	state_lock();
	if (now < lock_until) {
		state_unlock();
		resp_err(c, 429, "locked");
		return;
	}
	bool ok = (k[0] && strcmp(k, G.qr_key) == 0) || (pin[0] && strcmp(pin, G.pin) == 0);
	if (!ok) {
		G.pin_fails++;
		/* cada 5 fallos, bloqueo cada vez más largo: 30 s, 1 min, 2 min… hasta 30 min.
		 * El PIN no cambia solo: así el usuario puede aprendérselo. */
		if (G.pin_fails % 5 == 0) {
			int step = G.pin_fails / 5 - 1;
			u64 secs = 30ull << (step > 6 ? 6 : step);
			if (secs > 1800)
				secs = 1800;
			lock_until = now + secs * 1000;
			event_add(EV_PIN_LOCK, "", c->ip, secs);
		}
		state_unlock();
		util_log("login fallido desde %s", c->ip);
		resp_err(c, 403, "bad_pin");
		return;
	}
	G.pin_fails = 0;
	int i = add_dev(c->ua, c->ip);
	char tok[33], name[48];
	util_strlcpy(tok, G.dev[i].token, sizeof tok);
	util_strlcpy(name, G.dev[i].name, sizeof name);
	event_add(EV_PAIRED, "", name, 0);
	state_unlock();

	char hdr[160];
	snprintf(hdr, sizeof hdr,
	         "Set-Cookie: c3d=%s; Max-Age=31536000; Path=/; HttpOnly; SameSite=Strict\r\n", tok);
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"device\":");
	buf_json_str(&b, name);
	buf_puts(&b, "}");
	resp(c, 200, "application/json; charset=utf-8", b.p, b.len, hdr);
	free(b.p);
}

/* ---------- la misma sesión por la IP y por el nombre .local ----------
 * Para el navegador son dos webs distintas, cada una con su cookie. Si entra por una en la
 * que aún no tiene sesión, la web va a la otra (/api/handoff): allí, si ya estaba vinculado,
 * la consola crea un código de un solo uso (1 minuto) y vuelve con él; la primera lo canjea
 * por la cookie del mismo dispositivo. Así no hace falta volver a vincularlo. */
#define HANDOFF_N 4
static struct {
	char code[33], tok[33];
	u64 exp;
} handoff[HANDOFF_N];

/* «back» solo puede ser esta consola: su IP o su nombre .local (con el puerto, si no es el 80) */
static bool handoff_back_ok(const char *back)
{
	char a[96], b[112], ip[24];
	util_strlcpy(ip, G.ip ? inet_ntoa((struct in_addr){G.ip}) : "-", sizeof ip);
	const char *hosts[2] = {ip, G.net_name};
	for (int i = 0; i < 2; i++) {
		snprintf(a, sizeof a, "http://%s%s", hosts[i], i ? ".local" : "");
		snprintf(b, sizeof b, "%s:%d", a, G.port);
		if (!strcasecmp(back, a) || !strcasecmp(back, b))
			return true;
	}
	return false;
}

/* la página que hace de puente: pide el código con la cookie de este sitio y vuelve */
static void h_handoff_page(Conn *c)
{
	static const char html[] =
	    "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width\">"
	    "<title>Cloud3DS</title><body style=\"background:#0d121c\"><script>"
	    "var b=new URLSearchParams(location.search).get('back')||'';"
	    "fetch('/api/handoff_mint',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
	    "body:'back='+encodeURIComponent(b)}).then(function(r){return r.json()})"
	    ".then(function(j){location.replace(j.url||b+'/#ho=-')})"
	    ".catch(function(){if(/^http:\\/\\/[^\\/]+$/.test(b))location.replace(b+'/#ho=-')})"
	    "</script>";
	resp(c, 200, "text/html; charset=utf-8", html, sizeof html - 1, "Cache-Control: no-store\r\n");
}

static void h_handoff_mint(Conn *c, const char *body)
{
	char back[96] = "";
	qget(body, "back", back, sizeof back);
	state_lock();
	bool ok = handoff_back_ok(back);
	char code[33] = "-";
	if (ok && c->dev >= 0) {
		int slot = 0;
		for (int i = 1; i < HANDOFF_N; i++)
			if (handoff[i].exp < handoff[slot].exp)
				slot = i;
		util_rand_hex(handoff[slot].code, 32);
		util_strlcpy(handoff[slot].tok, G.dev[c->dev].token, sizeof handoff[slot].tok);
		handoff[slot].exp = util_ms() + 60000;
		util_strlcpy(code, handoff[slot].code, sizeof code);
	}
	state_unlock();
	if (!ok) {
		resp_err(c, 400, "back");
		return;
	}
	Buf b = {0};
	char url[160];
	snprintf(url, sizeof url, "%s/#ho=%s", back, code);
	buf_puts(&b, "{\"ok\":true,\"url\":");
	buf_json_str(&b, url);
	buf_puts(&b, "}");
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_handoff_claim(Conn *c, const char *body)
{
	char code[40] = "", tok[33] = "", name[48] = "";
	qget(body, "k", code, sizeof code);
	u64 now = util_ms();
	state_lock();
	for (int i = 0; i < HANDOFF_N; i++)
		if (handoff[i].exp > now && strlen(code) == 32 && !strcmp(handoff[i].code, code)) {
			util_strlcpy(tok, handoff[i].tok, sizeof tok);
			memset(&handoff[i], 0, sizeof handoff[i]); /* un solo uso */
		}
	int d = tok[0] ? find_dev(tok) : -1;
	if (d >= 0)
		util_strlcpy(name, G.dev[d].name, sizeof name);
	state_unlock();
	if (d < 0) {
		resp_err(c, 403, "expired");
		return;
	}
	char hdr[160];
	snprintf(hdr, sizeof hdr, "Set-Cookie: c3d=%s; Max-Age=31536000; Path=/; HttpOnly; SameSite=Strict\r\n", tok);
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"device\":");
	buf_json_str(&b, name);
	buf_puts(&b, "}");
	resp(c, 200, "application/json; charset=utf-8", b.p, b.len, hdr);
	free(b.p);
}

/* 1×1 transparente: la web la pide al otro nombre para saber si este navegador llega a él */
static void h_ping_gif(Conn *c)
{
	static const unsigned char gif[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x21, 0xf9, 4, 1, 0, 0, 0, 0,
	                                    0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0, 0x3b};
	resp(c, 200, "image/gif", gif, sizeof gif, "Cache-Control: no-store\r\n");
}

/* ---------- manejadores ---------- */

static void h_index(Conn *c)
{
	if (!page) {
		resp(c, 500, "text/plain", "romfs", 5, NULL);
		return;
	}
	resp(c, 200, "text/html; charset=utf-8", page, page_len,
	     "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
}

static bool revoked_token(const char *tok)
{
	if (strlen(tok) != 32)
		return false;
	bool r = false;
	state_lock();
	for (int i = 0; i < MAX_REVOKED; i++)
		if (strcmp(G.revoked[i], tok) == 0)
			r = true;
	state_unlock();
	return r;
}

static void h_hello(Conn *c)
{
	Buf b = {0};
	bool revoked = c->dev < 0 && revoked_token(c->token); /* toma el cerrojo: antes del bloque */
	state_lock();
	buf_puts(&b, "{\"app\":\"Cloud3DS\",\"version\":\"" APP_VERSION "\",\"console\":");
	buf_json_str(&b, G.console);
	buf_printf(&b, ",\"authed\":%s,\"revoked\":%s", c->dev >= 0 ? "true" : "false", revoked ? "true" : "false");
	/* las dos formas de entrar: para pasar la sesión de una a otra (h_handoff) */
	buf_printf(&b, ",\"ip\":\"%s\",\"local\":", G.ip ? inet_ntoa((struct in_addr){G.ip}) : "");
	if (G.mdns && (G.remote_ok & 1)) {
		char ln[80];
		snprintf(ln, sizeof ln, "%s.local", G.net_name);
		buf_json_str(&b, ln);
	} else
		buf_puts(&b, "\"\"");
	buf_puts(&b, "}");
	state_unlock();
	resp_json(c, 200, &b);
	free(b.p);
}

/* la web lo pregunta cada poco: si cambia, vuelve a leer la carpeta abierta */
/* ---------- capturas al momento: lo nuevo en /luma/screenshots (Rosalina) ---------- */
#define SHOTS_DIR "/luma/screenshots"
static u64 web_seen_ms; /* la última vez que una web preguntó: sin web abierta no se mira la carpeta */

/* lo que identifica una captura: el nombre sin «_top», «_bot» ni la extensión
 * («2026-10-27_21-10-05.123» o, en Luma antiguo, «0007» de «top_0007.bmp») */
static bool shot_key(const char *n, char *key, size_t kn, int *which)
{
	size_t l = strlen(n);
	if (l < 5 || strcasecmp(n + l - 4, ".bmp"))
		return false;
	char b[128];
	snprintf(b, sizeof b, "%.*s", (int)(l - 4), n);
	char *p;
	if ((p = strstr(b, "_top_right")))
		return false; // el ojo derecho del 3D: se queda la de arriba normal
	if ((p = strstr(b, "_top")) && !p[4])
		*p = 0, *which = 0;
	else if ((p = strstr(b, "_bot")) && !p[4])
		*p = 0, *which = 1;
	else if (!strncmp(b, "top_", 4))
		memmove(b, b + 4, strlen(b + 4) + 1), *which = 0;
	else if (!strncmp(b, "bot_", 4))
		memmove(b, b + 4, strlen(b + 4) + 1), *which = 1;
	else
		return false;
	snprintf(key, kn, "%s", b);
	return true;
}

typedef struct {
	char key[128], top[128], bot[128];
} ShotScan;

static bool shot_cb(const FsxEntry *e, void *u)
{
	ShotScan *s = u;
	char k[128];
	int w;
	if (e->dir || !shot_key(e->name, k, sizeof k, &w))
		return true;
	int c = strcmp(k, s->key);
	if (c > 0) { // una más nueva
		snprintf(s->key, sizeof s->key, "%s", k);
		s->top[0] = s->bot[0] = 0;
	}
	if (c >= 0)
		snprintf(w ? s->bot : s->top, sizeof s->top, "%s", e->name);
	return true;
}

static void shots_tick(void)
{
	static u64 last, pend_t;
	static char newest[128], sent[128];
	static bool primed;
	u64 now = util_ms();
	if (now - last < 1000 || !G.shots_live || now - web_seen_ms > 15000)
		return;
	last = now;
	ShotScan s = {{0}};
	snprintf(s.key, sizeof s.key, "%s", newest);
	fsx_list(SHOTS_DIR, false, shot_cb, &s);
	if (strcmp(s.key, newest) > 0)
		pend_t = now; // una nueva: se espera a tener las dos pantallas (Rosalina escribe una y luego otra)
	snprintf(newest, sizeof newest, "%s", s.key);
	if (!primed) { // las que ya estaban no se mandan
		primed = true;
		snprintf(sent, sizeof sent, "%s", newest);
		return;
	}
	if (!strcmp(newest, sent) || (!s.top[0] && !s.bot[0]) || ((!s.top[0] || !s.bot[0]) && now - pend_t < 4000))
		return;
	snprintf(sent, sizeof sent, "%s", newest);
	state_lock();
	util_strlcpy(G.shot_top, s.top, sizeof G.shot_top);
	util_strlcpy(G.shot_bot, s.bot, sizeof G.shot_bot);
	G.shot_seen = 0;
	G.shot_rev++;
	G.rev++;
	state_unlock();
	util_log("captura nueva: %s %s", s.top, s.bot);
}

static void h_rev(Conn *c)
{
	char b[96];
	web_seen_ms = util_ms();
	state_lock();
	int n = snprintf(b, sizeof b, "{\"fs\":%lu,\"shot\":%lu}", (unsigned long)G.fsrev, (unsigned long)(G.shots_live ? G.shot_rev : 0));
	state_unlock();
	resp(c, 200, "application/json", b, n, NULL);
}

/* la última captura: la web la pide al ver que cambió «shot» (y así se cuenta a cuántos llegó) */
static void h_shot(Conn *c)
{
	Buf b = {0};
	state_lock();
	if (G.shot_rev && (G.shot_top[0] || G.shot_bot[0])) {
		G.shot_seen++;
		G.rev++; // para que la consola vea el número
	}
	buf_printf(&b, "{\"ok\":true,\"rev\":%lu,\"dir\":\"" SHOTS_DIR "\",\"top\":", (unsigned long)G.shot_rev);
	buf_json_str(&b, G.shot_top);
	buf_puts(&b, ",\"bot\":");
	buf_json_str(&b, G.shot_bot);
	buf_puts(&b, "}");
	state_unlock();
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_info(Conn *c)
{
	u64 fr = 0, tot = 0;
	fsx_space(&fr, &tot);
	Buf b = {0};
	state_lock();
	buf_puts(&b, "{\"ok\":true,\"version\":\"" APP_VERSION "\",\"console\":");
	buf_json_str(&b, G.console);
	buf_puts(&b, ",\"device\":");
	buf_json_str(&b, G.dev[c->dev].name);
	buf_puts(&b, ",\"model\":");
	buf_json_str(&b, G.dev[c->dev].model);
	buf_printf(&b, ",\"named\":%d", G.dev[c->dev].named);
	buf_printf(&b, ",\"readonly\":%s,\"protect\":%s,\"free\":%llu,\"total\":%llu}",
	           G.readonly ? "true" : "false", G.protect_sys ? "true" : "false",
	           (unsigned long long)fr, (unsigned long long)tot);
	state_unlock();
	resp_json(c, 200, &b);
	free(b.p);
}

typedef struct {
	Buf *b;
	int n;
	const char *dir;
} ListCtx;

static bool list_cb(const FsxEntry *e, void *u)
{
	ListCtx *x = u;
	/* temporales de subidas y copias en curso (o de una cortada): no se enseñan */
	size_t l = strlen(e->name);
	if (!e->dir && !strncmp(e->name, ".c3d_", 5) && l > 5 && !strcmp(e->name + l - 5, ".part"))
		return true;
	if (e->dir && !strcmp(x->dir, "/3ds/Cloud3DS") && !strcmp(e->name, "trash")) // la papelera va aparte
		return true;
	if (x->n++)
		buf_puts(x->b, ",");
	buf_puts(x->b, "{\"n\":");
	buf_json_str(x->b, e->name);
	buf_printf(x->b, ",\"d\":%d,\"s\":%llu,\"t\":%llu", e->dir ? 1 : 0, (unsigned long long)e->size,
	           (unsigned long long)e->mtime);
	char full[1300];
	snprintf(full, sizeof full, "%s%s%s", x->dir, strcmp(x->dir, "/") ? "/" : "", e->name);
	state_lock();
	int col = mark_get(full);
	state_unlock();
	buf_printf(x->b, col ? ",\"c\":%d}" : "}", col);
	return x->n < 5000;
}

enum { SYS_NONE, SYS_BLOCK, SYS_CONFIRM };
static bool check_write(Conn *c, const char *p, int sys);
static void pct_encode(Buf *b, const char *s);

/* ---------- descargar como ZIP ----------
 * POST zip_prep?d=carpeta con los nombres (uno por línea) en el cuerpo → {id};
 * GET zip?t=id lo manda sin comprimir (store), generándolo mientras sale: no
 * ocupa sitio en la SD y se sabe su tamaño exacto desde el principio. */
typedef struct ZipStream {
	ZipItem *it;
	int n, i, ci, phase;
	char base[512];
	u32 *crc, *off, *dos;
	int fd;
	u64 left, pos;
	u32 cur_crc, cd_start;
} ZipStream;

#define ZPREP_N 4
static struct {
	u32 id;
	u64 t;
	char base[512];
	char (*names)[256];
	int n;
} ZPREP[ZPREP_N];

static void zs_free(Conn *c)
{
	ZipStream *z = c->zs;
	if (!z)
		return;
	if (z->fd >= 0)
		close(z->fd);
	zip_plan_free(z->it, z->n);
	free(z->crc), free(z->off), free(z->dos);
	free(z);
	c->zs = NULL;
}

static void h_zip_prep(Conn *c, const char *body)
{
	char d[512];
	bool dir;
	u64 sz;
	if (!qget(c->query, "d", d, sizeof d) || !fsx_path_ok(d) || fsx_stat(d, &dir, &sz) < 0 || !dir) {
		resp_err(c, 400, "bad_path");
		return;
	}
	int slot = 0; // el más viejo se reutiliza
	for (int i = 1; i < ZPREP_N; i++)
		if (ZPREP[i].t < ZPREP[slot].t)
			slot = i;
	free(ZPREP[slot].names);
	memset(&ZPREP[slot], 0, sizeof ZPREP[slot]);
	int n = 0;
	for (const char *p = body; *p; p++)
		n += *p == '\n';
	ZPREP[slot].names = calloc(n + 1, sizeof *ZPREP[slot].names);
	if (!ZPREP[slot].names) {
		resp_err(c, 500, "io");
		return;
	}
	n = 0;
	for (const char *p = body; *p;) {
		const char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l && l < 256) {
			memcpy(ZPREP[slot].names[n], p, l);
			ZPREP[slot].names[n][l] = 0;
			if (fsx_name_ok(ZPREP[slot].names[n]))
				n++;
		}
		p += l + (e ? 1 : 0);
	}
	static u32 next_id = 1;
	ZPREP[slot].id = next_id++ ^ (u32)(util_ms() << 8);
	ZPREP[slot].t = util_ms();
	ZPREP[slot].n = n;
	util_strlcpy(ZPREP[slot].base, d, sizeof ZPREP[slot].base);
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"id\":%lu}", (unsigned long)ZPREP[slot].id);
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_zip(Conn *c)
{
	char ts[16], name[300];
	u32 id = qget(c->query, "t", ts, sizeof ts) ? (u32)strtoul(ts, NULL, 10) : 0;
	int slot = -1;
	for (int i = 0; i < ZPREP_N; i++)
		if (id && ZPREP[i].id == id)
			slot = i;
	if (slot < 0) {
		resp_err(c, 404, "not_found");
		return;
	}
	ZipStream *z = calloc(1, sizeof *z);
	if (!z) {
		resp_err(c, 500, "io");
		return;
	}
	z->fd = -1;
	util_strlcpy(z->base, ZPREP[slot].base, sizeof z->base);
	// con una carpeta (o los nombres vacíos: la carpeta entera)
	char (*names)[256] = ZPREP[slot].names;
	int nn = ZPREP[slot].n;
	char whole[1][256];
	if (!nn) {
		const char *b = basename_of(z->base);
		char par[512];
		parent_of(par, sizeof par, z->base);
		util_strlcpy(whole[0], b, sizeof whole[0]);
		util_strlcpy(z->base, par, sizeof z->base);
		names = whole, nn = 1;
	}
	u64 total;
	int r = zip_plan(z->base, names, nn, &z->it, &z->n, &total);
	if (r != ZIP_OK) {
		free(z);
		resp_err(c, r == ZIP_TOO_BIG ? 413 : 404, r == ZIP_TOO_BIG ? "zip_too_big" : "not_found");
		return;
	}
	u64 len = 22;
	for (int i = 0; i < z->n; i++) {
		size_t nl = strlen(z->it[i].rel);
		len += 30 + nl + 46 + nl + (z->it[i].dir ? 0 : z->it[i].size + 16);
	}
	z->crc = calloc(z->n, 4), z->off = calloc(z->n, 4), z->dos = calloc(z->n, 4);
	if (len >= 0xFFFFFFFFull || !z->crc || !z->off || !z->dos) {
		c->zs = z;
		zs_free(c);
		resp_err(c, len >= 0xFFFFFFFFull ? 413 : 500, len >= 0xFFFFFFFFull ? "zip_too_big" : "io");
		return;
	}
	if (nn == 1)
		snprintf(name, sizeof name, "%s.zip", names[0]);
	else
		snprintf(name, sizeof name, "%s.zip", strcmp(z->base, "/") ? basename_of(z->base) : "Cloud3DS");
	Buf h = {0};
	buf_puts(&h, "Content-Disposition: attachment; filename*=UTF-8''");
	pct_encode(&h, name);
	buf_puts(&h, "\r\n");
	resp_head(c, 200, "application/zip", len, h.p);
	free(h.p);
	c->zs = z;
	state_lock();
	c->xid = xfer_begin(XF_DOWNLOAD, name, G.dev[c->dev].name, len);
	state_unlock();
}

/* el siguiente trozo del ZIP en c->out; false si algo falló (un archivo cambió o no se pudo leer) */
static bool zs_fill(Conn *c)
{
	ZipStream *z = c->zs;
	c->out.len = 0;
	c->out_off = 0;
	u8 hdr[700];
	while (c->out.len < FILE_CHUNK && z->phase < 5) {
		if (z->phase == 0) { // cabecera del siguiente
			if (z->i >= z->n) {
				z->phase = 3;
				z->cd_start = (u32)z->pos;
				continue;
			}
			ZipItem *it = &z->it[z->i];
			char sp[1200], ap[1100];
			zip_item_path(z->base, it, ap, sizeof ap);
			z->off[z->i] = (u32)z->pos;
			z->dos[z->i] = zip_dostime(fsx_mtime(ap));
			size_t hl = zip_local_header(hdr, it->rel, z->dos[z->i], it->size, it->dir, true);
			buf_add(&c->out, hdr, hl);
			z->pos += hl;
			if (it->dir) {
				z->i++;
				continue;
			}
			sdpath(sp, sizeof sp, ap);
			z->fd = open(sp, O_RDONLY);
			if (z->fd < 0)
				return false;
			z->left = it->size;
			z->cur_crc = 0;
			z->phase = 1;
		} else if (z->phase == 1) { // sus datos
			if (!z->left) {
				close(z->fd);
				z->fd = -1;
				z->phase = 2;
				continue;
			}
			size_t want = FILE_CHUNK - c->out.len;
			if (want > z->left)
				want = (size_t)z->left;
			buf_need(&c->out, want);
			ssize_t r = read(z->fd, c->out.p + c->out.len, want);
			if (r <= 0)
				return false; // se acortó desde que se empezó
			z->cur_crc = zip_crc(z->cur_crc, c->out.p + c->out.len, (size_t)r);
			c->out.len += (size_t)r;
			z->left -= (u64)r;
			z->pos += (u64)r;
		} else if (z->phase == 2) { // el CRC, detrás
			size_t hl = zip_data_desc(hdr, z->cur_crc, z->it[z->i].size);
			buf_add(&c->out, hdr, hl);
			z->pos += hl;
			z->crc[z->i++] = z->cur_crc;
			z->phase = 0;
		} else if (z->phase == 3) { // índice
			if (z->ci >= z->n) {
				z->phase = 4;
				continue;
			}
			ZipItem *it = &z->it[z->ci];
			size_t hl = zip_central(hdr, it->rel, z->dos[z->ci], z->crc[z->ci], it->size, it->size, z->off[z->ci], it->dir, 0, true);
			buf_add(&c->out, hdr, hl);
			z->pos += hl;
			z->ci++;
		} else { // final
			size_t hl = zip_end(hdr, z->n, (u32)(z->pos - z->cd_start), z->cd_start);
			buf_add(&c->out, hdr, hl);
			z->pos += hl;
			z->phase = 5;
		}
	}
	return true;
}

/* extraer un ZIP de la SD (desde la web): p = el ZIP, new=1 en una carpeta nueva; GET zipjob: cómo va */
static void h_unzip(Conn *c)
{
	char p[1024];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, p, SYS_CONFIRM))
		return;
	char dev[48];
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	state_unlock();
	int r = zipjob_extract(p, qflag(c->query, "new"), dev);
	if (r != ZIP_OK) {
		static const char *const E[] = {"ok", "bad_zip", "zip_unsupported", "io", "no_space", "canceled", "zip_damaged", "zip_too_big", "busy", "empty"};
		resp_err(c, r == ZIP_NO_SPACE ? 507 : r == ZIP_BUSY ? 409 : 400, E[-r]);
		return;
	}
	resp_ok(c);
}

static void h_zipjob(Conn *c)
{
	ZipJob j;
	zipjob_get(&j);
	static const char *const ST[] = {"idle", "run", "done", "fail"};
	static const char *const E[] = {"", "bad_zip", "zip_unsupported", "io", "no_space", "canceled", "zip_damaged", "zip_too_big", "busy", "empty"};
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"st\":\"%s\",\"create\":%s,\"done\":%llu,\"total\":%llu,\"error\":\"%s\",\"result\":", ST[j.st],
	           j.create ? "true" : "false", (unsigned long long)j.done, (unsigned long long)j.total, j.err < 0 && j.err >= -9 ? E[-j.err] : "");
	buf_json_str(&b, j.result);
	buf_puts(&b, "}");
	resp_json(c, 200, &b);
	free(b.p);
}

/* ---------- descargas de Internet: POST dl (url, d, hash, after=install|unzip), GET dls, POST dl_op (id, op) ---------- */
static void h_dl(Conn *c, const char *body)
{
	char url[1024], d[512] = "/", hash[80] = "", after[32] = "";
	// la URL puede venir en el cuerpo (url=…) para no chocar con el límite de la petición
	if (!qget(body, "url", url, sizeof url) && !qget(c->query, "url", url, sizeof url)) {
		resp_err(c, 400, "bad_url");
		return;
	}
	qget(c->query, "d", d, sizeof d);
	qget(c->query, "hash", hash, sizeof hash);
	qget(c->query, "after", after, sizeof after);
	if (!fsx_path_ok(d)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, d, SYS_CONFIRM))
		return;
	char dev[48];
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	state_unlock();
	u32 id = 0;
	int r = dl_add(url, d, hash, (strstr(after, "install") ? DLA_INSTALL : 0) | (strstr(after, "unzip") ? DLA_UNZIP : 0), dev, &id);
	if (r < 0) {
		resp_err(c, r == -3 ? 409 : 400, r == -1 ? "bad_url" : r == -2 ? "bad_hash" : "dl_full");
		return;
	}
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"id\":%lu}", (unsigned long)id);
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_dls(Conn *c)
{
	static DlItem it[DL_MAX];
	int n = dl_list(it, DL_MAX);
	static const char *const ST[] = {"queued", "run", "paused", "done", "fail", "canceled"};
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"rev\":%lu,\"items\":[", (unsigned long)dl_rev());
	for (int i = 0; i < n; i++) {
		DlItem *x = &it[i];
		buf_printf(&b, "%s{\"id\":%lu,\"st\":\"%s\",\"url\":", i ? "," : "", (unsigned long)x->id, ST[x->st]);
		buf_json_str(&b, x->url);
		buf_puts(&b, ",\"host\":");
		buf_json_str(&b, x->host);
		buf_puts(&b, ",\"name\":");
		buf_json_str(&b, x->name);
		buf_puts(&b, ",\"dir\":");
		buf_json_str(&b, x->dir);
		buf_puts(&b, ",\"path\":");
		buf_json_str(&b, x->path);
		buf_printf(&b, ",\"done\":%llu,\"total\":%llu,\"speed\":%llu,\"resumable\":%s,\"hash\":%s,\"hashOk\":%d,\"after\":\"%s\",\"err\":\"%s\"}",
		           (unsigned long long)x->done, (unsigned long long)x->total, (unsigned long long)x->speed, x->resumable ? "true" : "false",
		           x->hash[0] ? "true" : "false", x->hash_ok, x->after == (DLA_INSTALL | DLA_UNZIP) ? "install,unzip" : x->after == DLA_INSTALL ? "install" : x->after == DLA_UNZIP ? "unzip" : "", x->err);
	}
	buf_puts(&b, "]}");
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_dl_op(Conn *c)
{
	char ids[16] = "", op[16] = "";
	qget(c->query, "id", ids, sizeof ids);
	qget(c->query, "op", op, sizeof op);
	int o = !strcmp(op, "pause") ? DL_PAUSE : !strcmp(op, "resume") ? DL_RESUME : !strcmp(op, "cancel") ? DL_CANCEL : !strcmp(op, "remove") ? DL_REMOVE : !strcmp(op, "insecure") ? DL_INSECURE : -1;
	if (o < 0 || dl_op((u32)strtoul(ids, NULL, 10), o) < 0) {
		resp_err(c, 400, "bad_op");
		return;
	}
	resp_ok(c);
}

/* ---------- títulos instalados (solo verlos y copiar su partida; desinstalar, en la consola) ---------- */
static void h_titles(Conn *c)
{
	TitleInfo *t = NULL;
	int n = titles_list(&t);
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"items\":[");
	for (int i = 0; i < n; i++) {
		u64 last = 0;
		int copies = save_backups_count(&t[i], &last);
		char dir[512];
		save_dir(&t[i], dir, sizeof dir);
		buf_printf(&b, "%s{\"id\":\"%016llX\",\"n\":", i ? "," : "", (unsigned long long)t[i].tid);
		buf_json_str(&b, t[i].title);
		buf_puts(&b, ",\"pub\":");
		buf_json_str(&b, t[i].pub);
		buf_puts(&b, ",\"saves\":");
		buf_json_str(&b, dir);
		buf_printf(&b, ",\"v\":%u,\"uv\":%u,\"upd\":%s,\"s\":%llu,\"us\":%llu,\"dlc\":%d,\"ds\":%llu,\"copies\":%d,\"last\":%llu,\"icon\":%s}",
		           t[i].version, t[i].upd_version, t[i].has_upd ? "true" : "false", (unsigned long long)t[i].size,
		           (unsigned long long)t[i].upd_size, t[i].dlc, (unsigned long long)t[i].dlc_size, copies, (unsigned long long)last,
		           t[i].smdh ? "true" : "false");
	}
	buf_puts(&b, "]}");
	free(t);
	resp_json(c, 200, &b);
	free(b.p);
}

/* el icono de un título, 48×48, como BMP (sin dependencias) */
static void h_title_icon(Conn *c)
{
	char ids[24];
	u16 *px = malloc(48 * 48 * 2);
	if (!qget(c->query, "id", ids, sizeof ids) || !px || !title_icon(strtoull(ids, NULL, 16), px)) {
		free(px);
		resp_err(c, 404, "not_found");
		return;
	}
	static u8 bmp[54 + 48 * 48 * 3];
	memset(bmp, 0, 54);
	u32 sz = sizeof bmp;
	bmp[0] = 'B', bmp[1] = 'M';
	memcpy(bmp + 2, &sz, 4);
	bmp[10] = 54, bmp[14] = 40, bmp[18] = 48, bmp[22] = 48, bmp[26] = 1, bmp[28] = 24;
	for (int y = 0; y < 48; y++) // BMP va de abajo arriba, en BGR
		for (int x = 0; x < 48; x++) {
			u16 p = px[(47 - y) * 48 + x];
			u8 *o = bmp + 54 + (y * 48 + x) * 3;
			o[0] = (u8)((p & 31) << 3 | (p & 31) >> 2);
			o[1] = (u8)(((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4);
			o[2] = (u8)((p >> 11) << 3 | (p >> 11) >> 2);
		}
	free(px);
	resp(c, 200, "image/bmp", bmp, sizeof bmp, "Cache-Control: max-age=86400\r\n");
}

static void h_save_backup(Conn *c)
{
	char ids[24];
	if (!qget(c->query, "id", ids, sizeof ids)) {
		resp_err(c, 400, "bad_id");
		return;
	}
	u64 tid = strtoull(ids, NULL, 16);
	TitleInfo *t = NULL;
	int n = titles_list(&t), r = -3;
	char out[600] = "";
	for (int i = 0; i < n; i++)
		if (t[i].tid == tid) {
			r = save_backup(&t[i], out, sizeof out);
			if (r == 0) {
				state_lock();
				event_add(EV_CREATE, t[i].title, G.dev[c->dev].name, 0);
				state_unlock();
			}
		}
	free(t);
	if (r < 0) {
		resp_err(c, r == -1 ? 404 : r == -3 ? 400 : 500, r == -1 ? "no_save" : r == -3 ? "bad_id" : "io");
		return;
	}
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"path\":");
	buf_json_str(&b, out);
	buf_puts(&b, "}");
	resp_json(c, 200, &b);
	free(b.p);
}

/* ---------- escribir desde el PC en el editor de la consola ---------- */
static void h_live_open(Conn *c)
{
	char p[700];
	bool d;
	u64 sz;
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || fsx_stat(p, &d, &sz) < 0 || d) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, p, SYS_BLOCK))
		return;
	char dev[48];
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	state_unlock();
	live_open(p, dev);
	resp_ok(c);
}

/* un trozo: las líneas [i, i+d) pasan a ser t (n=1: se quitan) */
static void h_live_op(Conn *c, const char *body)
{
	char p[700], is[16] = "", ds[16] = "", cs[16] = "0";
	static char t[SMALL_BODY + 1];
	if (!qget(c->query, "p", p, sizeof p) || !qget(c->query, "i", is, sizeof is) || !qget(c->query, "d", ds, sizeof ds)) {
		resp_err(c, 400, "bad_op");
		return;
	}
	qget(c->query, "cur", cs, sizeof cs);
	bool none = qflag(c->query, "n");
	if (!none && !qget(body, "t", t, sizeof t))
		t[0] = 0;
	if (!live_push(p, atoi(is), atoi(ds), none ? NULL : t, atoi(cs))) {
		resp_err(c, 409, "live_off");
		return;
	}
	resp_ok(c);
}

static void h_live(Conn *c)
{
	char rs[16] = "0", path[700];
	qget(c->query, "rev", rs, sizeof rs);
	u32 rev;
	int cur;
	bool dirty, ok;
	char *text = live_get((u32)strtoul(rs, NULL, 10), &rev, &cur, &dirty, &ok, path, sizeof path);
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"on\":%s,\"doc\":%s,\"rev\":%lu,\"cur\":%d,\"dirty\":%s,\"path\":", path[0] ? "true" : "false",
	           ok ? "true" : "false", (unsigned long)rev, cur, dirty ? "true" : "false");
	buf_json_str(&b, path);
	if (text) {
		buf_puts(&b, ",\"text\":");
		buf_json_str(&b, text);
		free(text);
	}
	buf_puts(&b, "}");
	resp_json(c, 200, &b);
	free(b.p);
}

/* ---------- música: la web como mando (GET: qué suena; POST op=toggle|next|prev|stop|shuffle|repeat|play&p=) ---------- */
typedef struct {
	char (*n)[256];
	int count, max;
} AudioList;

static bool audio_cb(const FsxEntry *e, void *u)
{
	AudioList *l = u;
	if (!e->dir && music_is_audio(e->name) && l->count < l->max)
		util_strlcpy(l->n[l->count++], e->name, 256);
	return true;
}

static int cmp_name(const void *a, const void *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}

static void h_music(Conn *c, bool post)
{
	if (post) {
		char op[16] = "";
		qget(c->query, "op", op, sizeof op);
		if (!strcmp(op, "play")) { // esa canción, con su carpeta como lista (por nombre)
			char p[700], dir[700];
			if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || !music_is_audio(p)) {
				resp_err(c, 400, "bad_path");
				return;
			}
			parent_of(dir, sizeof dir, p);
			static char names[500][256];
			AudioList l = {names, 0, 500};
			fsx_list(dir, false, audio_cb, &l);
			qsort(names, l.count, sizeof names[0], cmp_name);
			int start = 0;
			for (int i = 0; i < l.count; i++)
				if (!strcmp(names[i], basename_of(p)))
					start = i;
			if (!l.count || !music_play(dir, names, l.count, start)) {
				MusicState ms;
				music_get(&ms);
				resp_err(c, 500, !strcmp(ms.err, "no_dsp") ? "no_dsp" : "io");
				return;
			}
		} else if (!strcmp(op, "toggle"))
			music_toggle();
		else if (!strcmp(op, "next"))
			music_next();
		else if (!strcmp(op, "prev"))
			music_prev();
		else if (!strcmp(op, "stop"))
			music_stop();
		else if (!strcmp(op, "shuffle"))
			music_shuffle();
		else if (!strcmp(op, "repeat"))
			music_repeat();
		else {
			resp_err(c, 400, "bad_op");
			return;
		}
	}
	MusicState m;
	music_get(&m);
	static const char *const ST[] = {"off", "play", "pause", "error"};
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"st\":\"%s\",\"title\":", ST[m.st]);
	buf_json_str(&b, m.title);
	buf_puts(&b, ",\"artist\":");
	buf_json_str(&b, m.artist);
	buf_puts(&b, ",\"dir\":");
	buf_json_str(&b, m.dir);
	buf_printf(&b, ",\"i\":%d,\"n\":%d,\"pos\":%lu,\"dur\":%lu,\"shuffle\":%s,\"repeat\":%d}", m.idx, m.n, (unsigned long)m.pos_ms,
	           (unsigned long)m.dur_ms, m.shuffle ? "true" : "false", m.repeat);
	resp_json(c, 200, &b);
	free(b.p);
}

/* MD5 y SHA-256: POST empieza (p = archivo), GET dice cómo va (el mismo trabajo que en la consola) */
static void h_hash(Conn *c, bool post)
{
	char p[1024];
	if (post) {
		bool d;
		u64 sz;
		if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || fsx_stat(p, &d, &sz) < 0 || d) {
			resp_err(c, 400, "bad_path");
			return;
		}
		hash_start(p);
	}
	HashJob j;
	hash_get(&j);
	Buf b = {0};
	static const char *const ST[] = {"idle", "run", "done", "error"};
	buf_printf(&b, "{\"ok\":true,\"st\":\"%s\",\"p\":", ST[j.st]);
	buf_json_str(&b, j.path);
	buf_printf(&b, ",\"size\":%llu,\"done\":%llu", (unsigned long long)j.size, (unsigned long long)j.done);
	if (j.st == HASH_DONE) {
		buf_printf(&b, ",\"md5\":\"%s\",\"sha256\":\"%s\",\"check\":\"%s\",\"checkFile\":", j.md5, j.sha,
		           j.check == HCHK_OK ? "ok" : j.check == HCHK_BAD ? "bad" : "");
		buf_json_str(&b, j.check_file);
	}
	buf_puts(&b, "}");
	resp_json(c, 200, &b);
	free(b.p);
}

/* buscar por nombre en p (por defecto toda la SD) y sus subcarpetas */
typedef struct {
	Buf *b;
	int n;
} FindCtx;

#define FIND_MAX 300

static bool find_cb(const char *dir, const FsxEntry *e, void *u)
{
	FindCtx *x = u;
	size_t l = strlen(e->name);
	if (!e->dir && !strncmp(e->name, ".c3d_", 5) && l > 5 && !strcmp(e->name + l - 5, ".part"))
		return true;
	buf_puts(x->b, x->n++ ? ",{\"n\":" : "{\"n\":");
	buf_json_str(x->b, e->name);
	buf_puts(x->b, ",\"p\":");
	buf_json_str(x->b, dir);
	buf_printf(x->b, ",\"d\":%d,\"s\":%llu,\"t\":%llu", e->dir ? 1 : 0, (unsigned long long)e->size,
	           (unsigned long long)e->mtime);
	char full[1300];
	snprintf(full, sizeof full, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e->name);
	state_lock();
	int col = mark_get(full);
	state_unlock();
	buf_printf(x->b, col ? ",\"c\":%d}" : "}", col);
	return x->n < FIND_MAX;
}

static void h_search(Conn *c)
{
	char p[1024] = "/", q[256];
	if (!qget(c->query, "q", q, sizeof q) || !q[0] || (qget(c->query, "p", p, sizeof p) && !fsx_path_ok(p))) {
		resp_err(c, 400, "bad_query");
		return;
	}
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"items\":[");
	FindCtx x = {&b, 0};
	bool tmo = false;
	int dirs = search_walk(p, q, NULL, 20000, &tmo, find_cb, &x);
	buf_printf(&b, "],\"dirs\":%d,\"more\":%s}", dirs, x.n >= FIND_MAX || tmo ? "true" : "false");
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_list(Conn *c)
{
	char p[1024];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"p\":");
	buf_json_str(&b, p);
	buf_printf(&b, ",\"system\":%s,\"items\":[", fsx_is_system(p) ? "true" : "false");
	ListCtx x = {&b, 0, p};
	if (fsx_list(p, !qflag(c->query, "fast"), list_cb, &x) < 0) {
		free(b.p);
		resp_err(c, 404, "not_found");
		return;
	}
	buf_puts(&b, "]}");
	resp_json(c, 200, &b);
	free(b.p);
}

static void pct_encode(Buf *b, const char *s)
{
	for (const u8 *p = (const u8 *)s; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		    strchr("-._~", *p))
			buf_add(b, p, 1);
		else
			buf_printf(b, "%%%02X", *p);
	}
}

static void h_file(Conn *c)
{
	char p[1024], sp[1100];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	bool dir;
	u64 size;
	if (fsx_stat(p, &dir, &size) < 0 || dir) {
		resp_err(c, 404, "not_found");
		return;
	}
	sdpath(sp, sizeof sp, p);
	int fd = open(sp, O_RDONLY);
	if (fd < 0) {
		resp_err(c, 404, "not_found");
		return;
	}
	u64 a = 0, e = size ? size - 1 : 0;
	bool partial = false;
	if (c->range[0] && size) {
		const char *r = c->range;
		if (strncmp(r, "bytes=", 6) == 0) {
			r += 6;
			char *end;
			if (*r == '-') {
				u64 suf = strtoull(r + 1, NULL, 10);
				if (suf > size)
					suf = size;
				a = size - suf;
			} else {
				a = strtoull(r, &end, 10);
				if (*end == '-' && end[1])
					e = strtoull(end + 1, NULL, 10);
			}
			if (e >= size)
				e = size - 1;
			if (a > e) {
				close(fd);
				char h[64];
				snprintf(h, sizeof h, "Content-Range: bytes */%llu\r\n", (unsigned long long)size);
				resp(c, 416, "text/plain", "", 0, h);
				return;
			}
			partial = true;
		}
	}
	u64 len = size ? e - a + 1 : 0;
	if (a && lseek(fd, (off_t)a, SEEK_SET) < 0) {
		close(fd);
		resp_err(c, 500, "io");
		return;
	}
	bool dl = qflag(c->query, "dl");
	Buf h = {0};
	buf_puts(&h, "Accept-Ranges: bytes\r\n");
	buf_printf(&h, "Content-Disposition: %s; filename*=UTF-8''", dl ? "attachment" : "inline");
	pct_encode(&h, basename_of(p));
	buf_puts(&h, "\r\n");
	if (partial)
		buf_printf(&h, "Content-Range: bytes %llu-%llu/%llu\r\n", (unsigned long long)a,
		           (unsigned long long)e, (unsigned long long)size);
	resp_head(c, partial ? 206 : 200, mime_of(p), len, h.p);
	free(h.p);
	if (strcmp(c->method, "HEAD") == 0) {
		close(fd);
		return;
	}
	c->ffd = fd;
	c->f_left = len;
	if (dl && !partial) {
		state_lock();
		const char *dn = G.dev[c->dev].name;
		c->xid = xfer_begin(XF_DOWNLOAD, basename_of(p), dn, len);
		state_unlock();
	}
}


/* Con la protección activa, las carpetas del sistema no se pueden borrar ni
 * renombrar (SYS_BLOCK); subir dentro de ellas pide confirmación (SYS_CONFIRM). */
static bool check_write(Conn *c, const char *p, int sys)
{
	if (G.readonly) {
		resp_err(c, 403, "readonly");
		return false;
	}
	if (sys != SYS_NONE && G.protect_sys && fsx_is_system(p)) {
		if (sys == SYS_BLOCK) {
			resp_err(c, 403, "protected");
			return false;
		}
		if (!qflag(c->query, "force")) {
			resp_err(c, 403, "system");
			return false;
		}
	}
	return true;
}

static void h_mkdir(Conn *c)
{
	char p[1024];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strcmp(p, "/") == 0) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, p, SYS_NONE))
		return;
	bool dir;
	u64 sz;
	if (fsx_stat(p, &dir, &sz) == 0) {
		resp_err(c, 409, "exists");
		return;
	}
	if (fsx_mkdirs(p) < 0) {
		resp_err(c, 500, "io");
		return;
	}
	state_lock();
	event_add(EV_MKDIR, basename_of(p), G.dev[c->dev].name, 0);
	state_unlock();
	resp_ok(c);
}

static void h_rename(Conn *c)
{
	char p[1024], to[256], np[1024], a[1100], b[1100];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strcmp(p, "/") == 0 ||
	    !qget(c->query, "to", to, sizeof to) || !fsx_name_ok(to)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, p, SYS_BLOCK))
		return;
	parent_of(np, sizeof np, p);
	size_t l = strlen(np);
	snprintf(np + l, sizeof np - l, "%s%s", l > 1 ? "/" : "", to);
	bool dir;
	u64 sz;
	if (fsx_stat(p, &dir, &sz) < 0) {
		resp_err(c, 404, "not_found");
		return;
	}
	/* cambiar solo mayúsculas en FAT: el destino «existe» pero es el mismo */
	if (strcasecmp(p, np) != 0 && fsx_stat(np, &dir, &sz) == 0) {
		resp_err(c, 409, "exists");
		return;
	}
	sdpath(a, sizeof a, p);
	sdpath(b, sizeof b, np);
	if (rename(a, b) != 0) {
		resp_err(c, 500, "io");
		return;
	}
	state_lock();
	event_add(EV_RENAME, to, G.dev[c->dev].name, 0);
	mark_moved(p, np);
	state_unlock();
	resp_ok(c);
}

static void h_delete(Conn *c)
{
	char p[1024];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strcmp(p, "/") == 0) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (!check_write(c, p, SYS_BLOCK))
		return;
	bool d;
	u64 sz;
	if (fsx_stat(p, &d, &sz) < 0) {
		resp_err(c, 404, "not_found");
		return;
	}
	// a la papelera (se recupera durante unos días); lo que ya está en ella se borra del todo
	char dev[48];
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	u8 col = (u8)mark_get(p);
	state_unlock();
	// now=1: del todo, sin papelera (cancelar la copia de un CIA recién subido)
	bool now = qflag(c->query, "now");
	if ((now || trash_put(p, dev, col) < 0) && fsx_rmrf(p) < 0) {
		resp_err(c, 500, "io");
		return;
	}
	state_lock();
	event_add(EV_DELETE, basename_of(p), G.dev[c->dev].name, 0);
	mark_deleted(p);
	state_unlock();
	resp_ok(c);
}

/* papelera: lista, recuperar, borrar del todo y vaciar */
static void h_trash(Conn *c)
{
	static TrashItem it[TRASH_MAX]; /* solo el hilo del servidor la usa */
	int n = trash_list(it, TRASH_MAX), cnt;
	u64 total = trash_size(&cnt);
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"days\":%d,\"size\":%llu,\"items\":[", TRASH_DAYS, (unsigned long long)total);
	for (int i = 0; i < n; i++) {
		buf_printf(&b, "%s{\"id\":%lu,\"n\":", i ? "," : "", (unsigned long)it[i].id);
		buf_json_str(&b, it[i].name);
		buf_puts(&b, ",\"f\":");
		buf_json_str(&b, it[i].from);
		buf_puts(&b, ",\"by\":");
		buf_json_str(&b, it[i].device);
		buf_printf(&b, ",\"t\":%llu,\"s\":%llu,\"d\":%s}", (unsigned long long)it[i].when, (unsigned long long)it[i].size,
		           it[i].dir ? "true" : "false");
	}
	buf_puts(&b, "]}");
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_trash_op(Conn *c, int op)
{
	if (G.readonly) {
		resp_err(c, 403, "readonly");
		return;
	}
	char ids[16] = "";
	u32 id = qget(c->query, "id", ids, sizeof ids) ? (u32)strtoul(ids, NULL, 10) : 0;
	TrashItem t;
	if (op != 2 && !trash_get(id, &t)) {
		resp_err(c, 404, "not_found");
		return;
	}
	if (op == 0) { /* recuperar */
		char dst[700];
		if (trash_restore(id, dst, sizeof dst) < 0) {
			resp_err(c, 500, "io");
			return;
		}
		state_lock();
		if (t.color)
			mark_set(dst, t.color);
		event_add(EV_RESTORE, t.name, G.dev[c->dev].name, 0);
		G.rev++;
		state_unlock();
		Buf b = {0};
		buf_puts(&b, "{\"ok\":true,\"path\":");
		buf_json_str(&b, dst);
		buf_puts(&b, "}");
		resp_json(c, 200, &b);
		free(b.p);
		return;
	}
	if (op == 1)
		trash_purge(id);
	else
		trash_empty();
	state_lock();
	G.rev++;
	G.fsrev++;
	state_unlock();
	resp_ok(c);
}

/* copiar y mover: p = origen, to = carpeta de destino, name = otro nombre (opcional),
 * on = replace | keep si ya existe (si no, 409 exists y la web pregunta) */
static bool op_target(Conn *c, bool move, char *src, char *dst, size_t n)
{
	char to[1024], name[256], on[16] = "";
	if (!qget(c->query, "p", src, 1024) || !fsx_path_ok(src) || strcmp(src, "/") == 0 || !qget(c->query, "to", to, sizeof to) ||
	    !fsx_path_ok(to)) {
		resp_err(c, 400, "bad_path");
		return false;
	}
	if (!qget(c->query, "name", name, sizeof name))
		util_strlcpy(name, basename_of(src), sizeof name);
	if (!fsx_name_ok(name)) {
		resp_err(c, 400, "bad_name");
		return false;
	}
	qget(c->query, "on", on, sizeof on);
	if (move ? !check_write(c, src, SYS_BLOCK) : G.readonly ? (resp_err(c, 403, "readonly"), true) : false)
		return false;
	if (!check_write(c, to, SYS_CONFIRM))
		return false;
	bool sd, dd;
	u64 sz;
	if (fsx_stat(src, &sd, &sz) < 0) {
		resp_err(c, 404, "not_found");
		return false;
	}
	if (fsx_stat(to, &dd, &sz) < 0 || !dd) {
		resp_err(c, 404, "no_dir");
		return false;
	}
	snprintf(dst, n, "%s%s%s", to, strcmp(to, "/") ? "/" : "", name);
	if (sd && ops_inside(src, dst) && strcasecmp(src, dst) != 0) {
		resp_err(c, 400, "into_self");
		return false;
	}
	bool same = strcasecmp(src, dst) == 0;
	if (move && same) {
		resp_ok(c); /* ya está ahí */
		return false;
	}
	if (fsx_stat(dst, &dd, &sz) == 0) {
		if (!strcmp(on, "keep") || (same && !move)) {
			ops_free_name(dst, n, to, name);
		} else if (!strcmp(on, "replace")) {
			if (dd != sd) {
				resp_err(c, 409, dd ? "is_dir" : "is_file");
				return false;
			}
		} else {
			Buf b = {0};
			buf_printf(&b, "{\"ok\":false,\"error\":\"exists\",\"dir\":%s}", dd ? "true" : "false");
			resp_json(c, 409, &b);
			free(b.p);
			return false;
		}
	}
	return true;
}

static void h_move(Conn *c)
{
	char src[1024], dst[1024];
	if (!op_target(c, true, src, dst, sizeof dst))
		return;
	int r = ops_move(src, dst);
	if (r != OPS_OK) {
		resp_err(c, r == OPS_NOT_FOUND ? 404 : r == OPS_TYPE ? 409 : 500, r == OPS_NOT_FOUND ? "not_found" : r == OPS_TYPE ? "is_dir" : "io");
		return;
	}
	state_lock();
	event_add(EV_MOVE, basename_of(dst), G.dev[c->dev].name, 0);
	mark_moved(src, dst);
	state_unlock();
	resp_ok(c);
}

static void h_copy(Conn *c)
{
	char src[1024], dst[1024];
	if (!op_target(c, false, src, dst, sizeof dst))
		return;
	char dev[48];
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	state_unlock();
	int r = ops_copy_start(src, dst, dev);
	if (r != OPS_OK) {
		resp_err(c, r == OPS_BUSY ? 409 : r == OPS_NO_SPACE ? 507 : 500, r == OPS_BUSY ? "busy" : r == OPS_NO_SPACE ? "no_space" : "io");
		return;
	}
	resp_ok(c);
}

/* nombre de este dispositivo puesto a mano (vacío: vuelve al automático) */
static void h_devname(Conn *c)
{
	char n[48] = "";
	qget(c->query, "n", n, sizeof n);
	if (n[0] && !util_utf8_valid(n)) {
		resp_err(c, 400, "bad_name");
		return;
	}
	state_lock();
	device_rename(c->dev, n);
	state_unlock();
	resp_ok(c);
}

/* destacar con un color (1-6, 0 lo quita): solo es un dato de Cloud3DS, no toca la SD */
static void h_mark(Conn *c)
{
	char p[1024], cs[4] = "";
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strcmp(p, "/") == 0 || !qget(c->query, "c", cs, sizeof cs)) {
		resp_err(c, 400, "bad_path");
		return;
	}
	int col = atoi(cs);
	if (col < 0 || col > 6 || strlen(p) >= sizeof G.marks[0].path) {
		resp_err(c, 400, "bad_color");
		return;
	}
	state_lock();
	mark_set(p, col);
	G.rev++;
	state_unlock();
	resp_ok(c);
}

/* instalar un CIA de la SD: la consola pregunta; la web sigue el estado con /api/installs */
static void h_install(Conn *c)
{
	char p[1024], err[24] = "", dev[48];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strlen(p) >= 256) {
		resp_err(c, 400, "bad_path");
		return;
	}
	if (G.readonly) {
		resp_err(c, 403, "readonly");
		return;
	}
	state_lock();
	util_strlcpy(dev, G.dev[c->dev].name, sizeof dev);
	G.rev++;
	state_unlock();
	int id = ins_request(p, dev, qflag(c->query, "up"), err, sizeof err);
	if (id < 0) {
		resp_err(c, !strcmp(err, "already") || !strcmp(err, "full") ? 409 : 400, err);
		return;
	}
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"id\":%d}", id);
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_installs(Conn *c)
{
	static InsReq q[MAX_INS]; /* solo el hilo del servidor la usa */
	int n = ins_snapshot(q, MAX_INS);
	static const char *const ST[] = {"free", "pending", "queued", "running", "done", "failed", "kept", "rejected"};
	Buf b = {0};
	buf_puts(&b, "{\"ok\":true,\"items\":[");
	for (int i = 0; i < n; i++) {
		char v[16];
		buf_printf(&b, "%s{\"id\":%d,\"st\":\"%s\",\"path\":", i ? "," : "", q[i].id, ST[q[i].st]);
		buf_json_str(&b, q[i].path);
		buf_puts(&b, ",\"title\":");
		buf_json_str(&b, q[i].title);
		buf_printf(&b, ",\"ver\":\"%s\",\"inst\":%d,\"newer\":%s,\"self\":%s,\"size\":%llu,\"done\":%llu,\"error\":\"%s\",\"deleted\":%s}",
		           ins_ver_text(q[i].ver, v, sizeof v), q[i].inst_ver, q[i].inst_ver > q[i].ver ? "true" : "false", q[i].self ? "true" : "false", (unsigned long long)q[i].size,
		           (unsigned long long)q[i].done, q[i].err, q[i].deleted ? "true" : "false");
	}
	buf_puts(&b, "]}");
	resp_json(c, 200, &b);
	free(b.p);
}

static void h_job(Conn *c)
{
	int st, err;
	u64 done, total;
	ops_job(&st, &err, &done, &total);
	static const char *const ST[] = {"idle", "run", "done", "fail"};
	const char *e = err == OPS_NO_SPACE ? "no_space" : err == OPS_CANCELED ? "canceled" : err == OPS_NOT_FOUND ? "not_found" : err ? "io" : "";
	Buf b = {0};
	buf_printf(&b, "{\"ok\":true,\"st\":\"%s\",\"done\":%llu,\"total\":%llu,\"error\":\"%s\"}", ST[st],
	           (unsigned long long)done, (unsigned long long)total, e);
	resp_json(c, 200, &b);
	free(b.p);
}

static void send_now(int fd, const char *s)
{
	size_t n = strlen(s), o = 0;
	u64 t0 = util_ms();
	while (o < n && util_ms() - t0 < 1000) {
		int r = send(fd, s + o, n - o, 0);
		if (r > 0)
			o += r;
		else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			break;
	}
}

static void upload_finish(Conn *c);

static void h_upload(Conn *c)
{
	char p[1024], dirp[1024];
	if (!qget(c->query, "p", p, sizeof p) || !fsx_path_ok(p) || strcmp(p, "/") == 0) {
		resp_err(c, 400, "bad_path");
		c->keepalive = false;
		return;
	}
	c->keepalive = false; /* si algo falla, el cuerpo sin leer obliga a cerrar */
	parent_of(dirp, sizeof dirp, p);
	if (!check_write(c, dirp, SYS_CONFIRM))
		return;
	if (!c->has_clen) {
		resp_err(c, 411, "length");
		return;
	}
	if (c->clen > 0xFFFFFFFFull) {
		resp_err(c, 413, "too_big"); /* FAT32: máx. 4 GB por archivo */
		return;
	}
	bool dir;
	u64 sz;
	if (fsx_stat(dirp, &dir, &sz) < 0 || !dir) {
		if (!qflag(c->query, "mk") || fsx_mkdirs(dirp) < 0) {
			resp_err(c, 404, "no_dir");
			return;
		}
	}
	bool exists = fsx_stat(p, &dir, &sz) == 0;
	if (exists && dir) {
		resp_err(c, 409, "is_dir");
		return;
	}
	if (exists && !qflag(c->query, "ow")) {
		resp_err(c, 409, "exists");
		return;
	}
	u64 fr = 0, tot = 0;
	if (fsx_space(&fr, &tot) && fr + (exists ? sz : 0) < c->clen + 512 * 1024) {
		trash_make_room(c->clen + 512 * 1024 - fr - (exists ? sz : 0)); // la papelera cede su sitio, lo más viejo primero
		if (fsx_space(&fr, &tot) && fr + (exists ? sz : 0) < c->clen + 512 * 1024) {
			resp_err(c, 507, "no_space");
			return;
		}
	}

	snprintf(c->u_final, sizeof c->u_final, "sdmc:%s", p);
	snprintf(c->u_tmp, sizeof c->u_tmp, "sdmc:%s%s.c3d_%08lx.part", dirp, strlen(dirp) > 1 ? "/" : "",
	         (unsigned long)(svcGetSystemTick() & 0xFFFFFFFF));
	util_strlcpy(c->u_name, basename_of(p), sizeof c->u_name);
	c->u_edit = qflag(c->query, "ed");
	int fd = open(c->u_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		resp_err(c, 500, "io");
		return;
	}
	/* reservar el tamaño de golpe: FAT asigna clústeres seguidos y es más rápido */
	if (c->clen && ftruncate(fd, (off_t)c->clen) != 0) {
		close(fd);
		unlink(c->u_tmp);
		resp_err(c, 507, "no_space");
		return;
	}
	c->ubuf = malloc(UP_BUF);
	if (!c->ubuf) {
		close(fd);
		unlink(c->u_tmp);
		resp_err(c, 500, "memory");
		return;
	}
	c->ufd = fd;
	c->u_total = c->clen;
	c->u_done = 0;
	c->u_over = exists;
	c->ubuf_len = 0;
	c->keepalive = true;
	state_lock();
	c->xid = xfer_begin(XF_UPLOAD, c->u_name, G.dev[c->dev].name, c->clen);
	state_unlock();

	/* bytes del cuerpo que llegaron con la cabecera */
	int extra = c->in_len - c->hdr_len;
	if (extra > 0) {
		if ((u64)extra > c->u_total)
			extra = (int)c->u_total;
		memcpy(c->ubuf, c->in + c->hdr_len, extra);
		c->ubuf_len = extra;
		c->u_done = extra;
		c->hdr_len += extra;
	}
	if (c->expect100)
		send_now(c->fd, "HTTP/1.1 100 Continue\r\n\r\n");
	c->st = CS_UPLOAD;
	if (c->u_done >= c->u_total)
		upload_finish(c);
}

static void upload_flush(Conn *c, bool *err)
{
	if (!c->ubuf_len)
		return;
	ssize_t w = write(c->ufd, c->ubuf, c->ubuf_len);
	if (w != (ssize_t)c->ubuf_len)
		*err = true;
	c->ubuf_len = 0;
}

static void upload_finish(Conn *c)
{
	bool err = false;
	upload_flush(c, &err);
	if (close(c->ufd) != 0)
		err = true;
	c->ufd = -1;
	if (!err && c->u_over)
		unlink(c->u_final);
	if (!err && rename(c->u_tmp, c->u_final) != 0)
		err = true;
	free(c->ubuf);
	c->ubuf = NULL;
	state_lock();
	Device *d = c->dev >= 0 ? &G.dev[c->dev] : NULL;
	xfer_end(c->xid, err ? XS_FAILED : XS_DONE);
	Xfer *x = xfer_get(c->xid);
	u64 ms = x ? x->t_end - x->t_start : 0;
	event_add(err ? EV_UPLOAD_FAIL : c->u_edit ? EV_EDIT : EV_UPLOAD, c->u_name, d ? d->name : "", c->u_total);
	if (!err)
		G.stat_up += c->u_total;
	state_unlock();
	c->xid = 0;
	util_log("upload %s: %s %llu B en %llu ms", err ? "ERROR" : "ok", c->u_final,
	         (unsigned long long)c->u_total, (unsigned long long)ms);
	if (err) {
		unlink(c->u_tmp);
		c->keepalive = false;
		resp_err(c, 500, "io");
	} else {
		resp_ok(c);
	}
}

static void route(Conn *c, const char *body)
{
	const char *m = c->method, *p = c->path;
	bool get = strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0;
	bool post = strcmp(m, "POST") == 0;

	if (get && (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0)) {
		h_index(c);
		return;
	}
	if (strncmp(p, "/api/", 5) != 0) {
		resp(c, 404, "text/plain", "404", 3, NULL);
		return;
	}
	p += 5;

	state_lock();
	c->dev = find_dev(c->token);
	if (c->dev >= 0) {
		Device *d = &G.dev[c->dev];
		u64 now = util_ms();
		if (!d->last_ms || now - d->last_ms > 30000)
			G.rev++; /* vuelve a estar «conectado» */
		d->last_ms = now;
		d->last_seen = util_unix_now();
		util_strlcpy(d->ip, c->ip, sizeof d->ip);
	}
	state_unlock();

	if (strcmp(p, "hello") == 0) {
		h_hello(c);
		return;
	}
	if (strcmp(p, "handoff") == 0 && get) {
		h_handoff_page(c);
		return;
	}
	if (strcmp(p, "handoff_mint") == 0 && post) {
		h_handoff_mint(c, body);
		return;
	}
	if (strcmp(p, "handoff_claim") == 0 && post) {
		h_handoff_claim(c, body);
		return;
	}
	if (strcmp(p, "ping.gif") == 0 && get) {
		h_ping_gif(c);
		return;
	}
	if (strcmp(p, "login") == 0) {
		if (post)
			h_login(c, body);
		else
			resp_err(c, 405, "method");
		return;
	}
	if (c->dev < 0) {
		/* expulsado desde la consola: la web lo explica y se borra la cookie */
		if (revoked_token(c->token))
			resp(c, 401, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"revoked\"}", 30,
			     "Set-Cookie: c3d=; Max-Age=0; Path=/; SameSite=Strict\r\n");
		else
			resp_err(c, 401, "auth");
		return;
	}
	if (strcmp(p, "logout") == 0 && post) {
		state_lock();
		G.dev[c->dev].used = false;
		G.dirty_cfg = true;
		G.rev++;
		state_unlock();
		resp(c, 200, "application/json", "{\"ok\":true}", 11,
		     "Set-Cookie: c3d=; Max-Age=0; Path=/; SameSite=Strict\r\n");
		return;
	}
	if (strcmp(p, "ping") == 0)
		resp_ok(c);
	else if (strcmp(p, "rev") == 0 && get)
		h_rev(c);
	else if (strcmp(p, "info") == 0 && get)
		h_info(c);
	else if (strcmp(p, "hash") == 0)
		h_hash(c, post);
	else if (strcmp(p, "live_open") == 0 && post)
		h_live_open(c);
	else if (strcmp(p, "live_op") == 0 && post)
		h_live_op(c, body);
	else if (strcmp(p, "live") == 0 && get)
		h_live(c);
	else if (strcmp(p, "live_save") == 0 && post) {
		live_request_save();
		resp_ok(c);
	} else if (strcmp(p, "live_close") == 0 && post) {
		live_close();
		resp_ok(c);
	} else if (strcmp(p, "music") == 0)
		h_music(c, post);
	else if (strcmp(p, "shot") == 0 && get)
		h_shot(c);
	else if (strcmp(p, "titles") == 0 && get)
		h_titles(c);
	else if (strcmp(p, "title_icon") == 0 && get)
		h_title_icon(c);
	else if (strcmp(p, "save_backup") == 0 && post)
		h_save_backup(c);
	else if (strcmp(p, "dl") == 0 && post)
		h_dl(c, body);
	else if (strcmp(p, "dls") == 0 && get)
		h_dls(c);
	else if (strcmp(p, "dl_op") == 0 && post)
		h_dl_op(c);
	else if (strcmp(p, "zip_prep") == 0 && post)
		h_zip_prep(c, body);
	else if (strcmp(p, "zip") == 0 && get)
		h_zip(c);
	else if (strcmp(p, "unzip") == 0 && post)
		h_unzip(c);
	else if (strcmp(p, "zipjob") == 0 && get)
		h_zipjob(c);
	else if (strcmp(p, "search") == 0 && get)
		h_search(c);
	else if (strcmp(p, "list") == 0 && get)
		h_list(c);
	else if (strcmp(p, "file") == 0 && get)
		h_file(c);
	else if (strcmp(p, "upload") == 0 && strcmp(m, "PUT") == 0)
		h_upload(c);
	else if (strcmp(p, "mkdir") == 0 && post)
		h_mkdir(c);
	else if (strcmp(p, "rename") == 0 && post)
		h_rename(c);
	else if (strcmp(p, "delete") == 0 && post)
		h_delete(c);
	else if (strcmp(p, "trash") == 0 && get)
		h_trash(c);
	else if (strcmp(p, "trash_restore") == 0 && post)
		h_trash_op(c, 0);
	else if (strcmp(p, "trash_delete") == 0 && post)
		h_trash_op(c, 1);
	else if (strcmp(p, "trash_empty") == 0 && post)
		h_trash_op(c, 2);
	else if (strcmp(p, "move") == 0 && post)
		h_move(c);
	else if (strcmp(p, "copy") == 0 && post)
		h_copy(c);
	else if (strcmp(p, "install") == 0 && post)
		h_install(c);
	else if (strcmp(p, "installs") == 0 && get)
		h_installs(c);
	else if (strcmp(p, "devname") == 0 && post)
		h_devname(c);
	else if (strcmp(p, "mark") == 0 && post)
		h_mark(c);
	else if (strcmp(p, "job") == 0 && get)
		h_job(c);
	else if (strcmp(p, "job_cancel") == 0 && post) {
		ops_cancel();
		resp_ok(c);
	}
	else
		resp_err(c, 404, "unknown");
}

/* ---------- análisis HTTP ---------- */

static bool parse_head(Conn *c)
{
	char *end = strstr(c->in, "\r\n\r\n");
	if (!end)
		return false;
	c->hdr_len = (int)(end - c->in) + 4;
	*end = 0;

	char *save = NULL;
	char *line = strtok_r(c->in, "\r\n", &save);
	if (!line)
		return false;
	char ver[16] = "";
	if (sscanf(line, "%7s %2047s %15s", c->method, c->target, ver) != 3)
		return false;
	c->keepalive = strcmp(ver, "HTTP/1.1") == 0;
	c->path = c->target;
	char *q = strchr(c->target, '?');
	if (q) {
		*q = 0;
		c->query = q + 1;
	}
	while ((line = strtok_r(NULL, "\r\n", &save))) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = 0;
		char *v = colon + 1;
		while (*v == ' ')
			v++;
		if (strcasecmp(line, "Content-Length") == 0) {
			c->clen = strtoull(v, NULL, 10);
			c->has_clen = true;
		} else if (strcasecmp(line, "Connection") == 0) {
			if (strcasestr(v, "close"))
				c->keepalive = false;
			else if (strcasestr(v, "keep-alive"))
				c->keepalive = true;
		} else if (strcasecmp(line, "Cookie") == 0) {
			char *k = strstr(v, "c3d=");
			if (k) {
				k += 4;
				size_t l = strcspn(k, "; ");
				if (l < sizeof c->token) {
					memcpy(c->token, k, l);
					c->token[l] = 0;
				}
			}
		} else if (strcasecmp(line, "User-Agent") == 0) {
			util_strlcpy(c->ua, v, sizeof c->ua);
		} else if (strcasecmp(line, "Range") == 0) {
			util_strlcpy(c->range, v, sizeof c->range);
		} else if (strcasecmp(line, "Expect") == 0) {
			c->expect100 = strcasestr(v, "100-continue") != NULL;
		}
	}
	return true;
}

/* procesa lo que haya en c->in; devuelve false si hay que cerrar */
static bool handle_input(Conn *c)
{
	if (c->st == CS_HEAD) {
		c->in[c->in_len] = 0;
		if (!strstr(c->in, "\r\n\r\n")) {
			if (c->in_len >= IN_MAX)
				return false; /* cabecera demasiado grande */
			return true;
		}
		/* copia de la parte del cuerpo antes de que strtok la toque */
		if (!parse_head(c)) {
			c->keepalive = false;
			resp_err(c, 400, "bad_request");
			return true;
		}
		bool is_upload = strcmp(c->method, "PUT") == 0;
		if (!is_upload && c->has_clen && c->clen > SMALL_BODY) {
			c->keepalive = false;
			resp_err(c, 413, "too_big");
			return true;
		}
		if (!is_upload && c->has_clen && c->clen > 0)
			c->st = CS_BODY;
		else {
			route(c, "");
			return true;
		}
	}
	if (c->st == CS_BODY) {
		if ((u64)(c->in_len - c->hdr_len) < c->clen)
			return true;
		char body[SMALL_BODY + 1];
		memcpy(body, c->in + c->hdr_len, c->clen);
		body[c->clen] = 0;
		c->hdr_len += (int)c->clen;
		route(c, body);
	}
	return true;
}

static void finish_response(Conn *c)
{
	zs_free(c);
	if (c->ffd >= 0) {
		close(c->ffd);
		c->ffd = -1;
	}
	if (c->xid) {
		state_lock();
		Xfer *x = xfer_get(c->xid);
		G.stat_down += x ? x->total : 0;
		if (x)
			event_add(EV_DOWNLOAD, x->name, x->device, x->total);
		xfer_end(c->xid, XS_DONE);
		state_unlock();
		c->xid = 0;
	}
	if (!c->keepalive) {
		conn_close(c);
		return;
	}
	/* datos de la siguiente petición que ya hubieran llegado */
	int rest = c->in_len - c->hdr_len;
	if (rest > 0)
		memmove(c->in, c->in + c->hdr_len, rest);
	c->in_len = rest > 0 ? rest : 0;
	c->hdr_len = 0;
	conn_reset_req(c);
	c->st = CS_HEAD;
	if (c->in_len > 0)
		handle_input(c);
}

static void on_readable(Conn *c)
{
	if (c->st == CS_UPLOAD) {
		bool err = false;
		for (int rounds = 0; rounds < 8; rounds++) {
			u64 left = c->u_total - c->u_done;
			if (!left)
				break;
			size_t room = UP_BUF - c->ubuf_len;
			if (room > left)
				room = (size_t)left;
			int r = recv(c->fd, c->ubuf + c->ubuf_len, room, 0);
			if (r == 0) {
				conn_close(c);
				return;
			}
			if (r < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				conn_close(c);
				return;
			}
			c->last_io = util_ms();
			c->ubuf_len += r;
			c->u_done += r;
			if (c->ubuf_len == UP_BUF)
				upload_flush(c, &err);
			if (err)
				break;
		}
		if (err) {
			util_log("error escribiendo %s", c->u_tmp);
			conn_close(c);
			return;
		}
		state_lock();
		xfer_progress(c->xid, c->u_done);
		if (c->dev >= 0)
			G.dev[c->dev].last_ms = util_ms(); /* sigue «conectado» mientras transfiere */
		state_unlock();
		if (c->u_done >= c->u_total)
			upload_finish(c);
		return;
	}
	if (c->st != CS_HEAD && c->st != CS_BODY)
		return;
	int room = IN_MAX - c->in_len;
	if (room <= 0) {
		conn_close(c);
		return;
	}
	int r = recv(c->fd, c->in + c->in_len, room, 0);
	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		conn_close(c);
		return;
	}
	if (r < 0)
		return;
	c->last_io = util_ms();
	c->any_data = true;
	c->in_len += r;
	if (!handle_input(c))
		conn_close(c);
}

static void on_writable(Conn *c)
{
	for (int rounds = 0; rounds < 8; rounds++) {
		if (c->out_off >= c->out.len) {
			if (c->zs && c->zs->phase < 5) {
				if (!zs_fill(c)) {
					conn_close(c);
					return;
				}
				if (!c->out.len)
					continue;
			} else if (c->ffd >= 0 && c->f_left > 0) {
				size_t want = c->f_left > FILE_CHUNK ? FILE_CHUNK : (size_t)c->f_left;
				ssize_t r = read(c->ffd, fbuf, want);
				if (r <= 0) {
					conn_close(c);
					return;
				}
				c->out.len = 0;
				c->out_off = 0;
				buf_add(&c->out, fbuf, r);
				c->f_left -= r;
			} else {
				finish_response(c);
				return;
			}
		}
		int r = send(c->fd, c->out.p + c->out_off, c->out.len - c->out_off, 0);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			conn_close(c);
			return;
		}
		c->out_off += r;
		c->last_io = util_ms();
		if (c->xid) {
			c->x_done += r;
			state_lock();
			xfer_progress(c->xid, c->x_done);
			if (c->dev >= 0)
				G.dev[c->dev].last_ms = util_ms();
			state_unlock();
		}
	}
}

/* ---------- bucle ---------- */

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void close_all(void)
{
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].st != CS_FREE)
			conn_close(&conns[i]);
	if (lfd >= 0)
		closesocket(lfd);
	lfd = -1;
}

static bool open_listener(int *port_out)
{
	static const int ports[] = SYS_PORTS;
	for (size_t i = 0; i < ARRAY_LEN(ports); i++) {
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
		if (fd < 0)
			return false;
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_port = htons(ports[i]);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 8) == 0) {
			set_nonblock(fd);
			lfd = fd;
			*port_out = ports[i];
			return true;
		}
		closesocket(fd);
	}
	return false;
}

static void accept_new(void)
{
	struct sockaddr_in a;
	socklen_t al = sizeof a;
	int fd = accept(lfd, (struct sockaddr *)&a, &al);
	if (fd < 0)
		return;
	Conn *slot = NULL;
	for (int i = 0; i < MAX_CONN; i++)
		if (conns[i].st == CS_FREE) {
			slot = &conns[i];
			break;
		}
	if (!slot) {
		/* sin hueco: suelta la conexión inactiva más antigua */
		Conn *old = NULL;
		for (int i = 0; i < MAX_CONN; i++)
			if (conns[i].st == CS_HEAD && conns[i].in_len == 0 &&
			    (!old || conns[i].last_io < old->last_io))
				old = &conns[i];
		if (!old) {
			closesocket(fd);
			return;
		}
		conn_close(old);
		slot = old;
	}
	set_nonblock(fd);
	int bs = 64 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bs, sizeof bs);
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bs, sizeof bs);
	memset(slot, 0, sizeof *slot);
	slot->fd = fd;
	slot->ffd = slot->ufd = -1;
	slot->dev = -1;
	slot->st = CS_HEAD;
	slot->last_io = util_ms();
	inet_ntop(AF_INET, &a.sin_addr, slot->ip, sizeof slot->ip);
}

static void timeouts(void)
{
	u64 now = util_ms();
	for (int i = 0; i < MAX_CONN; i++) {
		Conn *c = &conns[i];
		if (c->st == CS_FREE)
			continue;
		u64 idle = now - c->last_io;
		u64 lim = c->st == CS_HEAD ? (c->any_data ? 15000 : 5000) : 30000;
		if (idle > lim)
			conn_close(c);
	}
}

static void load_page(void)
{
	FILE *f = fopen("romfs:/web/index.html.gz", "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	page = malloc(n);
	if (page && fread(page, 1, n, f) == (size_t)n)
		page_len = n;
	fclose(f);
}

static void serve_loop(void)
{
	struct pollfd pf[MAX_CONN + 1 + 8];
	int idx[MAX_CONN + 1 + 8];
	u64 last_ip_check = util_ms();
	u32 rrev = 0xFFFFFFFF;
	while (!quit && !restart) {
		if (rrev != G.remote_rev) { // cloud3ds.local y FBI remoto: al empezar y al cambiarlos en Ajustes
			state_lock();
			rrev = G.remote_rev;
			bool md = G.mdns, fb = G.fbi_remote;
			char nm[41];
			util_strlcpy(nm, G.net_name, sizeof nm);
			u32 ip = G.ip;
			int port = G.port;
			state_unlock();
			remote_open(ip, port, md, fb, nm);
			state_lock();
			G.remote_ok = (remote_mdns_ready() ? 1 : 0) | (remote_fbi_ready() ? 2 : 0);
			G.rev++;
			state_unlock();
		}
		int n = 0;
		bool room = false;
		for (int i = 0; i < MAX_CONN; i++) {
			Conn *c = &conns[i];
			if (c->st == CS_FREE) {
				room = true;
				continue;
			}
			if (c->st == CS_HEAD && c->in_len == 0)
				room = true;
			pf[n].fd = c->fd;
			pf[n].events = c->st == CS_SEND ? POLLOUT : POLLIN;
			pf[n].revents = 0;
			idx[n++] = i;
		}
		if (room) {
			pf[n].fd = lfd;
			pf[n].events = POLLIN;
			pf[n].revents = 0;
			idx[n++] = -1;
		}
		int nr = remote_fds(pf + n, 8);
		for (int k = 0; k < nr; k++)
			idx[n + k] = -2;
		int r = poll(pf, n + nr, 20);
		if (r > 0)
			remote_events(pf + n, nr);
		remote_tick();
		shots_tick();
		if (r > 0) {
			for (int k = 0; k < n; k++) {
				if (!pf[k].revents)
					continue;
				if (idx[k] < 0) {
					accept_new();
					continue;
				}
				Conn *c = &conns[idx[k]];
				if (c->st == CS_FREE || c->fd != pf[k].fd)
					continue;
				if (pf[k].revents & (POLLERR | POLLNVAL)) {
					conn_close(c);
					continue;
				}
				if (c->st == CS_SEND)
					on_writable(c);
				else
					on_readable(c);
				/* la respuesta se puede empezar a enviar ya */
				if (c->st == CS_SEND)
					on_writable(c);
			}
		}
		timeouts();
		u64 now = util_ms();
		if (now - last_ip_check > 1000) {
			last_ip_check = now;
			u32 ip = util_ip();
			if (ip != G.ip)
				break;
		}
	}
}

/* soc:U puede no estar listo al abrir la app (p. ej. si la anterior aún lo está
 * soltando): se reintenta cada pocos segundos en vez de rendirse */
static bool net_init(void)
{
	int tries = 0;
	while (!quit) {
		Result r = socInit(soc_buf, SOC_BUFSIZE);
		tries++;
		if (R_SUCCEEDED(r)) {
			util_log("socInit correcto (intento %d)", tries);
			state_lock();
			G.srv_error[0] = 0;
			G.net_err = 0;
			G.rev++;
			state_unlock();
			return true;
		}
		util_log("socInit falló: 0x%08lX (intento %d), reintento en 3 s", (unsigned long)r, tries);
		state_lock();
		util_strlcpy(G.srv_error, "soc", sizeof G.srv_error);
		G.net_err = (u32)r;
		G.rev++;
		state_unlock();
		for (int i = 0; i < 30 && !quit; i++)
			svcSleepThread(100 * 1000000ll);
	}
	return false;
}

static void server_thread(void *arg)
{
	(void)arg;
	soc_ok = net_init();
	if (soc_ok)
		names_start();
	int last_wifi = -2;
	u64 last_wifi_t = 0;
	bool logged_noip = false;
	while (!quit) {
		restart = false;
		u32 ip = util_ip();
		/* estado real del Wi-Fi (ac:u), para el log y para la pantalla */
		u64 now = util_ms();
		int wifi = last_wifi;
		if (!ip && (now - last_wifi_t > 2000 || last_wifi == -2)) {
			wifi = util_wifi_status();
			last_wifi_t = now;
		} else if (ip) {
			wifi = 3;
		}
		if (wifi != last_wifi) {
			util_log("wifi: estado %d, ip %s", wifi, ip ? inet_ntoa((struct in_addr){ip}) : "-");
			last_wifi = wifi;
		}
		state_lock();
		G.ip = ip;
		G.wifi = wifi;
		G.running = false;
		G.rev++;
		state_unlock();
		if (!ip) {
			if (!logged_noip) {
				util_log("esperando IP (gethostid = 0)");
				logged_noip = true;
			}
			svcSleepThread(500 * 1000000ll);
			continue;
		}
		logged_noip = false;
		int port = 0;
		if (!open_listener(&port)) {
			state_lock();
			util_strlcpy(G.srv_error, "listen", sizeof G.srv_error);
			G.rev++;
			state_unlock();
			util_log("no se pudo abrir el puerto (errno %d)", errno);
			svcSleepThread(2000 * 1000000ll);
			continue;
		}
		state_lock();
		G.port = port;
		G.running = true;
		G.srv_error[0] = 0;
		G.rev++;
		state_unlock();
		util_log("servidor en %s:%d", inet_ntoa((struct in_addr){ip}), port);
		serve_loop();
		remote_close();
		close_all();
	}
	state_lock();
	G.running = false;
	state_unlock();
}

void server_start(void)
{
	for (int i = 0; i < MAX_CONN; i++) {
		conns[i].st = CS_FREE;
		conns[i].fd = conns[i].ffd = conns[i].ufd = -1;
	}
	load_page();
	fbuf = malloc(FILE_CHUNK);
	soc_buf = memalign(0x1000, SOC_BUFSIZE);
	if (!soc_buf) {
		util_strlcpy(G.srv_error, "mem", sizeof G.srv_error);
		util_log("sin memoria para soc:U");
		return;
	}
	util_log("Cloud3DS %s: arrancando la red", APP_VERSION);
	quit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	/* en New 3DS, al núcleo 2; si no, al de la app con más prioridad que la interfaz */
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	thr = NULL;
	if (n3ds)
		thr = threadCreate(server_thread, NULL, 64 * 1024, prio - 1, 2, false);
	if (!thr)
		thr = threadCreate(server_thread, NULL, 64 * 1024, prio - 1, 0, false);
	if (!thr) {
		util_strlcpy(G.srv_error, "thread", sizeof G.srv_error);
		util_log("no se pudo crear el hilo del servidor");
	}
}

void server_stop(void)
{
	quit = true;
	if (thr) {
		threadJoin(thr, 3000000000ull);
		threadFree(thr);
		thr = NULL;
	}
	names_stop();
	if (soc_ok)
		socExit();
	soc_ok = false;
	free(page);
	page = NULL;
	free(fbuf);
	fbuf = NULL;
}

void server_restart(void)
{
	restart = true;
}
