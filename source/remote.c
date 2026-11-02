/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "remote.h"
#include "dl.h"
#include "util.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_ADDR 0xE00000FB /* 224.0.0.251 */
#define FBI_MAX 4
#define FBI_URLS 32
#define FBI_PAYLOAD (FBI_URLS * 1024)

typedef struct {
	int fd;
	u8 *buf;
	u32 need, got; /* need = 0: aún leyendo los 4 bytes de la longitud */
	u8 len4[4];
	u32 ids[FBI_URLS];
	int nids;
	bool waiting;  /* enlaces en la cola: se responde cuando acaben */
	u64 t0;
} Fbi;

static struct {
	int mfd, lfd;
	u32 ip;
	char name[48];
	Fbi c[FBI_MAX];
	u64 announce_at;
	int announces;
} R = {.mfd = -1, .lfd = -1};

bool remote_name_ok(const char *n)
{
	size_t l = strlen(n);
	if (l < 1 || l > 40 || n[0] == '-' || n[l - 1] == '-')
		return false;
	for (size_t i = 0; i < l; i++)
		if (!isalnum((unsigned char)n[i]) && n[i] != '-')
			return false;
	return true;
}

static void nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void fbi_drop(Fbi *f, bool ack)
{
	if (f->fd >= 0) {
		if (ack) { // la app del PC espera este byte para cerrar su servidor
			u8 z = 0;
			send(f->fd, &z, 1, 0);
		}
		closesocket(f->fd);
	}
	free(f->buf);
	memset(f, 0, sizeof *f);
	f->fd = -1;
}

void remote_close(void)
{
	if (R.mfd >= 0)
		closesocket(R.mfd);
	if (R.lfd >= 0)
		closesocket(R.lfd);
	R.mfd = R.lfd = -1;
	for (int i = 0; i < FBI_MAX; i++)
		if (R.c[i].fd > 0 || R.c[i].buf)
			fbi_drop(&R.c[i], true);
}

void remote_open(u32 ip, int web_port, bool mdns, bool fbi, const char *name)
{
	remote_close();
	for (int i = 0; i < FBI_MAX; i++)
		R.c[i].fd = -1;
	R.ip = ip;
	util_strlcpy(R.name, name && remote_name_ok(name) ? name : "cloud3ds", sizeof R.name);
	if (!ip)
		return;
	if (mdns) {
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_port = htons(MDNS_PORT);
		a.sin_addr.s_addr = INADDR_ANY;
		int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		struct ip_mreq m;
		m.imr_multiaddr.s_addr = htonl(MDNS_ADDR);
		m.imr_interface.s_addr = ip;
		u8 ttl = 255;
		if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 &&
		    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0) {
			setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
			nonblock(s);
			R.mfd = s;
			R.announces = 0;
			R.announce_at = util_ms() + 200;
			util_log("mDNS: %s.local listo", R.name);
		} else {
			util_log("mDNS: no se pudo abrir (%d)", errno);
			if (s >= 0)
				closesocket(s);
		}
	}
	if (fbi && web_port != REMOTE_FBI_PORT) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_port = htons(REMOTE_FBI_PORT);
		a.sin_addr.s_addr = INADDR_ANY;
		if (s >= 0 && bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 2) == 0) {
			nonblock(s);
			R.lfd = s;
			util_log("FBI remoto: escuchando en el puerto %d", REMOTE_FBI_PORT);
		} else {
			util_log("FBI remoto: no se pudo abrir el puerto %d (%d)", REMOTE_FBI_PORT, errno);
			if (s >= 0)
				closesocket(s);
		}
	}
}

bool remote_fbi_ready(void) { return R.lfd >= 0; }
bool remote_mdns_ready(void) { return R.mfd >= 0; }

int remote_fds(struct pollfd *pf, int max)
{
	int n = 0;
	if (R.mfd >= 0 && n < max)
		pf[n].fd = R.mfd, pf[n].events = POLLIN, pf[n].revents = 0, n++;
	if (R.lfd >= 0 && n < max)
		pf[n].fd = R.lfd, pf[n].events = POLLIN, pf[n].revents = 0, n++;
	for (int i = 0; i < FBI_MAX && n < max; i++)
		if (R.c[i].fd >= 0 && !R.c[i].waiting)
			pf[n].fd = R.c[i].fd, pf[n].events = POLLIN, pf[n].revents = 0, n++;
	return n;
}

/* ---------------------------------------------------------------- mDNS */

/* lee un nombre (con punteros de compresión) como «a.b.c»; devuelve dónde sigue o -1 */
static int dns_name(const u8 *p, int len, int off, char *out, size_t n)
{
	size_t o = 0;
	int next = -1, jumps = 0;
	while (off < len) {
		u8 l = p[off];
		if (l == 0) {
			if (next < 0)
				next = off + 1;
			out[o < n ? o : n - 1] = 0;
			return next;
		}
		if ((l & 0xC0) == 0xC0) {
			if (off + 1 >= len || ++jumps > 16)
				return -1;
			if (next < 0)
				next = off + 2;
			off = ((l & 0x3F) << 8) | p[off + 1];
			continue;
		}
		if (off + 1 + l > len)
			return -1;
		if (o && o + 1 < n)
			out[o++] = '.';
		for (int i = 0; i < l && o + 1 < n; i++)
			out[o++] = (char)p[off + 1 + i];
		off += 1 + l;
	}
	return -1;
}

static int put_name(u8 *b, const char *host)
{
	int o = 0;
	size_t l = strlen(host);
	b[o++] = (u8)l;
	memcpy(b + o, host, l);
	o += (int)l;
	b[o++] = 5;
	memcpy(b + o, "local", 5);
	o += 5;
	b[o++] = 0;
	return o;
}

/* respuesta con el registro A; legacy: pregunta de un cliente normal (puerto ≠ 5353), con su id y la pregunta */
static void mdns_send(u16 id, bool legacy, const struct sockaddr_in *to)
{
	u8 b[160];
	int o = 0;
	b[o++] = id >> 8, b[o++] = id & 255;
	b[o++] = 0x84, b[o++] = 0; // respuesta, con autoridad
	b[o++] = 0, b[o++] = legacy ? 1 : 0; // preguntas
	b[o++] = 0, b[o++] = 1;               // respuestas
	b[o++] = 0, b[o++] = 0, b[o++] = 0, b[o++] = 0;
	if (legacy) {
		o += put_name(b + o, R.name);
		b[o++] = 0, b[o++] = 1, b[o++] = 0, b[o++] = 1;
	}
	o += put_name(b + o, R.name);
	b[o++] = 0, b[o++] = 1;                 // A
	b[o++] = legacy ? 0 : 0x80, b[o++] = 1; // IN (y «cache flush»: somos los únicos con ese nombre)
	u32 ttl = 120;
	b[o++] = ttl >> 24, b[o++] = ttl >> 16, b[o++] = ttl >> 8, b[o++] = ttl;
	b[o++] = 0, b[o++] = 4;
	memcpy(b + o, &R.ip, 4); // ya en orden de red
	o += 4;
	sendto(R.mfd, b, o, 0, (const struct sockaddr *)to, sizeof *to);
}

static void mdns_read(void)
{
	u8 p[1500];
	struct sockaddr_in from;
	socklen_t fl = sizeof from;
	int len = recvfrom(R.mfd, p, sizeof p, 0, (struct sockaddr *)&from, &fl);
	if (len < 12 || (p[2] & 0x80)) // corto o es una respuesta
		return;
	u16 id = (u16)(p[0] << 8 | p[1]);
	int qd = p[4] << 8 | p[5], off = 12;
	char want[64];
	snprintf(want, sizeof want, "%s.local", R.name);
	for (int q = 0; q < qd && off < len; q++) {
		char nm[256];
		off = dns_name(p, len, off, nm, sizeof nm);
		if (off < 0 || off + 4 > len)
			return;
		u16 type = (u16)(p[off] << 8 | p[off + 1]), cls = (u16)(p[off + 2] << 8 | p[off + 3]);
		off += 4;
		if (strcasecmp(nm, want) || (type != 1 && type != 255) || (cls & 0x7FFF) != 1)
			continue;
		bool legacy = ntohs(from.sin_port) != MDNS_PORT;
		struct sockaddr_in to = from;
		if (!legacy && !(cls & 0x8000)) { // a todos, por multicast
			to.sin_family = AF_INET;
			to.sin_port = htons(MDNS_PORT);
			to.sin_addr.s_addr = htonl(MDNS_ADDR);
		}
		mdns_send(legacy ? id : 0, legacy, &to);
		return;
	}
}

/* ---------------------------------------------------------------- FBI */

static void fbi_accept(void)
{
	int s = accept(R.lfd, NULL, NULL);
	if (s < 0)
		return;
	for (int i = 0; i < FBI_MAX; i++)
		if (R.c[i].fd < 0) {
			nonblock(s);
			memset(&R.c[i], 0, sizeof R.c[i]);
			R.c[i].fd = s;
			R.c[i].t0 = util_ms();
			return;
		}
	closesocket(s); // demasiadas a la vez
}

static void fbi_payload(Fbi *f)
{
	f->buf[f->got] = 0;
	char *p = (char *)f->buf;
	while (*p && f->nids < FBI_URLS) {
		while (*p && (unsigned char)*p <= ' ')
			p++;
		char *e = p;
		while (*e && (unsigned char)*e > ' ')
			e++;
		char c = *e;
		*e = 0;
		u32 id;
		if (*p && dl_add(p, "/cias", "", DLA_INSTALL, "PC (FBI)", &id) == 0)
			f->ids[f->nids++] = id;
		*e = c;
		p = e;
	}
	util_log("FBI remoto: %d enlaces a la cola", f->nids);
	free(f->buf);
	f->buf = NULL;
	if (f->nids)
		f->waiting = true;
	else
		fbi_drop(f, true);
}

static void fbi_read(Fbi *f)
{
	for (;;) {
		int r;
		if (!f->need) {
			r = recv(f->fd, f->len4 + f->got, 4 - f->got, 0);
			if (r > 0 && (f->got += r) == 4) {
				f->need = (u32)f->len4[0] << 24 | f->len4[1] << 16 | f->len4[2] << 8 | f->len4[3];
				f->got = 0;
				if (!f->need || f->need > FBI_PAYLOAD || !(f->buf = malloc(f->need + 1))) {
					fbi_drop(f, true);
					return;
				}
			}
		} else {
			r = recv(f->fd, f->buf + f->got, f->need - f->got, 0);
			if (r > 0 && (f->got += r) == f->need) {
				fbi_payload(f);
				return;
			}
		}
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			fbi_drop(f, false);
			return;
		}
		if (r < 0)
			return;
	}
}

void remote_events(struct pollfd *pf, int n)
{
	for (int k = 0; k < n; k++) {
		if (!pf[k].revents)
			continue;
		if (pf[k].fd == R.mfd)
			mdns_read();
		else if (pf[k].fd == R.lfd)
			fbi_accept();
		else
			for (int i = 0; i < FBI_MAX; i++)
				if (R.c[i].fd == pf[k].fd && !R.c[i].waiting) {
					if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL))
						fbi_drop(&R.c[i], false);
					else
						fbi_read(&R.c[i]);
				}
	}
}

void remote_tick(void)
{
	u64 now = util_ms();
	if (R.mfd >= 0 && R.announces < 2 && now >= R.announce_at) { // al empezar se anuncia (dos veces, como pide el RFC)
		struct sockaddr_in to = {0};
		to.sin_family = AF_INET;
		to.sin_port = htons(MDNS_PORT);
		to.sin_addr.s_addr = htonl(MDNS_ADDR);
		mdns_send(0, false, &to);
		R.announces++;
		R.announce_at = now + 1000;
	}
	static u64 last;
	if (now - last < 1000)
		return;
	last = now;
	static DlItem it[DL_MAX];
	int n = -1;
	for (int i = 0; i < FBI_MAX; i++) {
		Fbi *f = &R.c[i];
		if (f->fd < 0)
			continue;
		if (!f->waiting) {
			if (now - f->t0 > 30000) // no mandó nada
				fbi_drop(f, false);
			continue;
		}
		if (n < 0)
			n = dl_list(it, DL_MAX);
		bool pending = false;
		for (int k = 0; k < f->nids && !pending; k++)
			for (int j = 0; j < n; j++)
				if (it[j].id == f->ids[k] && (it[j].st == DL_QUEUED || it[j].st == DL_RUN || it[j].st == DL_PAUSED))
					pending = true;
		if (!pending || now - f->t0 > 4 * 3600 * 1000ull)
			fbi_drop(f, true);
	}
}
