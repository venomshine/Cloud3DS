/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Nombre real de los dispositivos vinculados. Los navegadores no lo dan, así
 * que se pregunta a la red, en este orden:
 *   1. mDNS al propio dispositivo (iPhone, iPad, Mac, muchos Android y Linux)
 *   2. NetBIOS al propio dispositivo (Windows)
 *   3. DNS inverso al router (muchos routers conocen el nombre de cada equipo)
 * Va en su propio hilo para no frenar el servidor. */
#include "names.h"
#include "state.h"
#include "util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static Thread thr;
static volatile bool quit;

/* manda una pregunta UDP y espera la respuesta hasta ms milisegundos */
static int udp_ask(const char *ip, int port, const u8 *q, int qn, u8 *out, int outn, int ms)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(ip);
	int n = -1;
	if (sendto(fd, q, qn, 0, (struct sockaddr *)&a, sizeof a) == qn) {
		u64 t0 = util_ms();
		while (!quit && util_ms() - t0 < (u64)ms) {
			struct pollfd pf = {fd, POLLIN, 0};
			if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
				n = recvfrom(fd, out, outn, 0, NULL, NULL);
				break;
			}
			svcSleepThread(20 * 1000000ll);
		}
	}
	closesocket(fd);
	return n;
}

/* nombre DNS (con punteros de compresión) a texto con puntos */
static bool dns_name(const u8 *m, int mn, int off, char *out, size_t n)
{
	size_t o = 0;
	for (int hops = 0; off < mn && hops < 20; hops++) {
		int l = m[off];
		if (l == 0) {
			out[o ? o - 1 : 0] = 0;
			return o > 0;
		}
		if ((l & 0xC0) == 0xC0) {
			if (off + 1 >= mn)
				return false;
			off = ((l & 0x3F) << 8) | m[off + 1];
			continue;
		}
		if (off + 1 + l > mn || o + l + 1 >= n)
			return false;
		memcpy(out + o, m + off + 1, l);
		o += l;
		out[o++] = '.';
		off += 1 + l;
	}
	return false;
}

static int dns_skip(const u8 *m, int mn, int off)
{
	while (off < mn) {
		int l = m[off];
		if (l == 0)
			return off + 1;
		if ((l & 0xC0) == 0xC0)
			return off + 2;
		off += 1 + l;
	}
	return mn;
}

/* pregunta PTR de «d.c.b.a.in-addr.arpa» */
static int ptr_query(u8 *q, const char *ip, u16 cls)
{
	unsigned a, b, c, d;
	if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return 0;
	memset(q, 0, 12);
	q[1] = 0x42; /* id */
	q[5] = 1;    /* una pregunta */
	char name[64];
	snprintf(name, sizeof name, "%u.%u.%u.%u.in-addr.arpa", d, c, b, a);
	int o = 12;
	for (char *s = name, *dot; *s; s = dot ? dot + 1 : s + strlen(s)) {
		dot = strchr(s, '.');
		int l = dot ? (int)(dot - s) : (int)strlen(s);
		q[o++] = l;
		memcpy(q + o, s, l);
		o += l;
		if (!dot)
			break;
	}
	q[o++] = 0;
	q[o++] = 0, q[o++] = 12;               /* PTR */
	q[o++] = cls >> 8, q[o++] = cls & 0xFF; /* IN (y bit de respuesta unicast en mDNS) */
	return o;
}

static bool ptr_answer(const u8 *m, int mn, char *out, size_t n)
{
	if (mn < 12)
		return false;
	int qd = m[4] << 8 | m[5], an = m[6] << 8 | m[7];
	int off = 12;
	for (int i = 0; i < qd; i++)
		off = dns_skip(m, mn, off) + 4;
	for (int i = 0; i < an && off + 10 <= mn; i++) {
		off = dns_skip(m, mn, off);
		if (off + 10 > mn)
			break;
		int type = m[off] << 8 | m[off + 1], rl = m[off + 8] << 8 | m[off + 9];
		off += 10;
		if (type == 12 && dns_name(m, mn, off, out, n))
			return true;
		off += rl;
	}
	return false;
}

static bool by_mdns(const char *ip, char *out, size_t n)
{
	u8 q[128], r[1500];
	int qn = ptr_query(q, ip, 0x8001);
	int rn = qn ? udp_ask(ip, 5353, q, qn, r, sizeof r, 900) : -1;
	return rn > 0 && ptr_answer(r, rn, out, n);
}

static bool by_netbios(const char *ip, char *out, size_t n)
{
	/* NBSTAT de «*»: el equipo responde con su tabla de nombres */
	u8 q[50] = {0x13, 0x37, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 32};
	q[13] = 'C', q[14] = 'K';
	for (int i = 15; i < 45; i++)
		q[i] = 'A';
	q[45] = 0, q[46] = 0, q[47] = 0x21, q[48] = 0, q[49] = 1;
	u8 r[1024];
	int rn = udp_ask(ip, 137, q, sizeof q, r, sizeof r, 900);
	if (rn < 57)
		return false;
	int off = dns_skip(r, rn, 12) + 10; /* nombre, tipo, clase, TTL, longitud */
	if (off >= rn)
		return false;
	int cnt = r[off++];
	for (int i = 0; i < cnt && off + 18 <= rn; i++, off += 18) {
		u8 suffix = r[off + 15], flags = r[off + 16];
		if (suffix == 0 && !(flags & 0x80)) { /* nombre del equipo, no de grupo */
			char nm[16];
			memcpy(nm, r + off, 15);
			nm[15] = 0;
			for (int k = 14; k >= 0 && nm[k] == ' '; k--)
				nm[k] = 0;
			if (nm[0]) {
				util_strlcpy(out, nm, n);
				return true;
			}
		}
	}
	return false;
}

static bool by_router(const char *ip, char *out, size_t n)
{
	struct in_addr a;
	a.s_addr = inet_addr(ip);
	struct hostent *h = gethostbyaddr(&a, sizeof a, AF_INET);
	if (!h || !h->h_name || !h->h_name[0] || !strcmp(h->h_name, ip))
		return false;
	util_strlcpy(out, h->h_name, n);
	return true;
}

/* «Marios-iPhone.local» → «Marios-iPhone»; «DESKTOP-AB12.lan» → «DESKTOP-AB12» */
static void tidy(char *s)
{
	char *dot = strchr(s, '.');
	if (dot)
		*dot = 0;
	if (!util_utf8_valid(s))
		s[0] = 0;
}

static void names_thread(void *arg)
{
	(void)arg;
	while (!quit) {
		char ip[16] = "", tok[33] = "";
		state_lock();
		for (int i = 0; i < MAX_DEVICES; i++) {
			Device *d = &G.dev[i];
			if (d->used && d->named == NAME_AUTO && d->ip[0] && strcmp(d->tried_ip, d->ip)) {
				util_strlcpy(d->tried_ip, d->ip, sizeof d->tried_ip);
				util_strlcpy(ip, d->ip, sizeof ip);
				util_strlcpy(tok, d->token, sizeof tok);
				break;
			}
		}
		state_unlock();
		if (!ip[0]) {
			for (int i = 0; i < 20 && !quit; i++)
				svcSleepThread(100 * 1000000ll);
			continue;
		}
		char nm[96] = "";
		const char *how = by_mdns(ip, nm, sizeof nm) ? "mDNS" : by_netbios(ip, nm, sizeof nm) ? "NetBIOS"
		                : by_router(ip, nm, sizeof nm)                                  ? "router"
		                                                                                : NULL;
		if (how)
			tidy(nm);
		util_log("nombre de %s: %s (%s)", ip, how && nm[0] ? nm : "-", how ? how : "sin respuesta");
		if (!how || !nm[0])
			continue;
		state_lock();
		for (int i = 0; i < MAX_DEVICES; i++) {
			Device *d = &G.dev[i];
			if (d->used && !strcmp(d->token, tok) && d->named == NAME_AUTO) {
				util_strlcpy(d->name, nm, sizeof d->name);
				d->named = NAME_FOUND;
				G.dirty_cfg = true;
				G.rev++;
			}
		}
		state_unlock();
	}
}

void names_start(void)
{
	quit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	thr = threadCreate(names_thread, NULL, 16 * 1024, prio + 2, -2, false);
}

void names_stop(void)
{
	quit = true;
	if (thr) {
		threadJoin(thr, 3000000000ull);
		threadFree(thr);
		thr = NULL;
	}
}
