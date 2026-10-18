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
#include "state.h"
#include "util.h"

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

	/* subida */
	int ufd;
	u64 u_total, u_done;
	bool u_over;
	char u_tmp[1024], u_final[1024], u_name[256];
	u8 *ubuf;
	size_t ubuf_len;
} Conn;

static Conn conns[MAX_CONN];
static int lfd = -1;
static Thread thr;
static volatile bool quit, restart;
static u32 *soc_buf;
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
	parse_ua(ua, d->name, sizeof d->name, &d->kind);
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
	buf_printf(&b, ",\"authed\":%s,\"revoked\":%s}", c->dev >= 0 ? "true" : "false", revoked ? "true" : "false");
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
} ListCtx;

static bool list_cb(const FsxEntry *e, void *u)
{
	ListCtx *x = u;
	if (x->n++)
		buf_puts(x->b, ",");
	buf_puts(x->b, "{\"n\":");
	buf_json_str(x->b, e->name);
	buf_printf(x->b, ",\"d\":%d,\"s\":%llu,\"t\":%llu}", e->dir ? 1 : 0, (unsigned long long)e->size,
	           (unsigned long long)e->mtime);
	return x->n < 5000;
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
	ListCtx x = {&b, 0};
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

enum { SYS_NONE, SYS_BLOCK, SYS_CONFIRM };

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
	if (fsx_rmrf(p) < 0) {
		resp_err(c, 404, "not_found");
		return;
	}
	state_lock();
	event_add(EV_DELETE, basename_of(p), G.dev[c->dev].name, 0);
	state_unlock();
	resp_ok(c);
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
		resp_err(c, 507, "no_space");
		return;
	}

	snprintf(c->u_final, sizeof c->u_final, "sdmc:%s", p);
	snprintf(c->u_tmp, sizeof c->u_tmp, "sdmc:%s%s.c3d_%08lx.part", dirp, strlen(dirp) > 1 ? "/" : "",
	         (unsigned long)(svcGetSystemTick() & 0xFFFFFFFF));
	util_strlcpy(c->u_name, basename_of(p), sizeof c->u_name);
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
	event_add(err ? EV_UPLOAD_FAIL : EV_UPLOAD, c->u_name, d ? d->name : "", c->u_total);
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
	else if (strcmp(p, "info") == 0 && get)
		h_info(c);
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
			if (c->ffd >= 0 && c->f_left > 0) {
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
	struct pollfd pf[MAX_CONN + 1];
	int idx[MAX_CONN + 1];
	u64 last_ip_check = util_ms();
	while (!quit && !restart) {
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
		int r = poll(pf, n, 20);
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

static void server_thread(void *arg)
{
	(void)arg;
	while (!quit) {
		restart = false;
		u32 ip = util_ip();
		state_lock();
		G.ip = ip;
		G.running = false;
		G.rev++;
		state_unlock();
		if (!ip) {
			svcSleepThread(500 * 1000000ll);
			continue;
		}
		int port = 0;
		if (!open_listener(&port)) {
			state_lock();
			util_strlcpy(G.srv_error, "listen", sizeof G.srv_error);
			G.rev++;
			state_unlock();
			util_log("no se pudo abrir el puerto");
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
	if (!soc_buf || R_FAILED(socInit(soc_buf, SOC_BUFSIZE))) {
		util_strlcpy(G.srv_error, "soc", sizeof G.srv_error);
		return;
	}
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
}

void server_stop(void)
{
	quit = true;
	if (thr) {
		threadJoin(thr, 3000000000ull);
		threadFree(thr);
		thr = NULL;
	}
	socExit();
	free(page);
	page = NULL;
	free(fbuf);
	fbuf = NULL;
}

void server_restart(void)
{
	restart = true;
}
