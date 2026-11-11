/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "dl.h"
#include "cJSON.h"
#include "digest.h"
#include "fsx.h"
#include "install.h"
#include "ops.h"
#include "state.h"
#include "util.h"
#include "zipjob.h"

#include <ctype.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef TEST_CA_SD /* solo en compilaciones de prueba (make TEST_CA=1): raíces de la SD, para probar tras un proxy */
#define CA_BUNDLE "sdmc:/3ds/Cloud3DS/test_ca.pem"
#else
#define CA_BUNDLE "romfs:/certs/cacert.pem"
#endif
#define UA "Cloud3DS/" APP_VERSION " (Nintendo 3DS)"
/* el navegador se presenta como un navegador (y elige móvil o escritorio) y quiere saber a dónde
 * acabó la petición tras las redirecciones: por hilo, para no mezclarse con las descargas */
static __thread const char *t_ua;
static __thread char t_final[1024];
static __thread char t_ctype[128]; /* el Content-Type de la última respuesta (con su charset) */

void dl_set_thread_ua(const char *ua)
{
	t_ua = ua;
}

/* cookies del navegador (por hilo): qué mandar a una dirección y qué guardar de una respuesta */
static __thread size_t (*t_ck_get)(const char *url, char *out, size_t n);
static __thread void (*t_ck_set)(const char *url, const char *set_cookie);

void dl_set_thread_cookies(size_t (*get)(const char *, char *, size_t), void (*set)(const char *, const char *))
{
	t_ck_get = get, t_ck_set = set;
}

const char *dl_last_url(void)
{
	return t_final;
}
const char *dl_last_ctype(void)
{
	return t_ctype;
}
#define HC_ROOTS "romfs:/certs/httpc_roots.bin"
#define HC_BUF (256 * 1024)

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	DlItem it[DL_MAX];
	int n;
	u32 next, rev;
	volatile u32 cur;           /* la que se está bajando */
	volatile bool pause, cancel; /* para la actual */
	bool ready;
} D;

/* strcasestr no está en newlib */
static const char *ci_find(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++)
		if (!strncasecmp(h, n, l))
			return h;
	return NULL;
}

static void lock(void) { LightLock_Lock(&D.lock); }
static void unlock(void) { LightLock_Unlock(&D.lock); }

static DlItem *find(u32 id)
{
	for (int i = 0; i < D.n; i++)
		if (D.it[i].id == id)
			return &D.it[i];
	return NULL;
}

static void part_path(u32 id, const char *dir, char *out, size_t n)
{
	snprintf(out, n, "%s%s.c3d_dl%08lx.part", dir, strcmp(dir, "/") ? "/" : "", (unsigned long)id);
}

bool dl_url_ok(const char *url)
{
	size_t l = strlen(url);
	if (l < 10 || l >= sizeof D.it[0].url)
		return false;
	if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))
		return false;
	for (const char *p = url; *p; p++)
		if ((unsigned char)*p <= ' ')
			return false;
	return true;
}

static void host_of(const char *url, char *out, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t k = strcspn(p, "/:?#");
	snprintf(out, n, "%.*s", (int)(k < n - 1 ? k : n - 1), p);
}

/* lo último de la ruta de un enlace, sin ?… ni #…, con los %xx traducidos y limpio para FAT */
static void name_from_url(const char *url, char *out, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	const char *path = strchr(p, '/');
	char tmp[512] = "";
	if (path) {
		size_t k = strcspn(path, "?#");
		const char *s = path + k;
		while (s > path && s[-1] == '/')
			s--;
		const char *b = s;
		while (b > path && b[-1] != '/')
			b--;
		snprintf(tmp, sizeof tmp, "%.*s", (int)(s - b), b);
		util_url_decode(tmp);
	}
	size_t o = 0;
	for (const char *q = tmp; *q && o + 1 < n; q++)
		out[o++] = (unsigned char)*q < 32 || strchr("\\/:*?\"<>|", *q) ? '_' : *q;
	out[o] = 0;
	while (o && (out[o - 1] == '.' || out[o - 1] == ' '))
		out[--o] = 0;
}

int dl_add(const char *url, const char *dir, const char *hash, int after, const char *device, u32 *id)
{
	if (!D.ready || !dl_url_ok(url) || !fsx_path_ok(dir))
		return -1;
	char h[80] = "";
	size_t hn = 0;
	for (const char *p = hash ? hash : ""; *p && hn < 64; p++)
		if (isxdigit((unsigned char)*p))
			h[hn++] = (char)tolower((unsigned char)*p);
	h[hn] = 0;
	if (hn && hn != 32 && hn != 64)
		return -2;
	lock();
	if (D.n == DL_MAX) { // llena: se van las terminadas más viejas
		for (int i = 0; i < D.n && D.n == DL_MAX; i++)
			if (D.it[i].st == DL_DONE || D.it[i].st == DL_CANCELED) {
				memmove(&D.it[i], &D.it[i + 1], (D.n - i - 1) * sizeof D.it[0]);
				D.n--;
				i--;
			}
	}
	if (D.n == DL_MAX) {
		unlock();
		return -3;
	}
	DlItem *it = &D.it[D.n++];
	memset(it, 0, sizeof *it);
	it->id = ++D.next;
	it->st = DL_QUEUED;
	it->hash_ok = -1;
	it->after = after;
	util_strlcpy(it->url, url, sizeof it->url);
	util_strlcpy(it->dir, dir, sizeof it->dir);
	util_strlcpy(it->hash, h, sizeof it->hash);
	util_strlcpy(it->device, device ? device : "", sizeof it->device);
	host_of(url, it->host, sizeof it->host);
	name_from_url(url, it->name, sizeof it->name);
	if (id)
		*id = it->id;
	D.rev++;
	unlock();
	LightEvent_Signal(&D.ev);
	return 0;
}

int dl_list(DlItem *out, int max)
{
	lock();
	int n = D.n < max ? D.n : max;
	memcpy(out, D.it, n * sizeof *out);
	unlock();
	return n;
}

void dl_set_name(u32 id, const char *name)
{
	lock();
	DlItem *it = find(id);
	if (it && name[0]) {
		util_strlcpy(it->name, name, sizeof it->name);
		it->fixed_name = true;
		D.rev++;
	}
	unlock();
}

u32 dl_rev(void)
{
	return D.rev;
}

int dl_active(void)
{
	int k = 0;
	lock();
	for (int i = 0; i < D.n; i++)
		k += D.it[i].st == DL_QUEUED || D.it[i].st == DL_RUN;
	unlock();
	return k;
}

int dl_op(u32 id, int op)
{
	lock();
	DlItem *it = find(id);
	int r = it ? 0 : -1;
	if (it) {
		char part[700];
		part_path(it->id, it->dir, part, sizeof part);
		switch (op) {
		case DL_PAUSE:
			if (it->installing) // AM no puede seguir donde se quedó
				r = -3;
			else if (it->st == DL_RUN && D.cur == id)
				D.pause = true;
			else if (it->st == DL_QUEUED)
				it->st = DL_PAUSED;
			break;
		case DL_INSECURE: // solo tras un error de certificado, y solo esta descarga
			if (it->st == DL_FAIL && !strcmp(it->err, "tls")) {
				it->insecure = true;
				it->st = DL_QUEUED;
				it->err[0] = 0;
			}
			break;
		case DL_RESUME:
			if (it->st == DL_PAUSED || it->st == DL_FAIL) {
				it->st = DL_QUEUED;
				it->err[0] = 0;
			}
			break;
		case DL_CANCEL:
			if (it->st == DL_RUN && D.cur == id)
				D.cancel = true;
			else if (it->st == DL_QUEUED || it->st == DL_PAUSED || it->st == DL_FAIL) {
				it->st = DL_CANCELED;
				char sp[720];
				snprintf(sp, sizeof sp, "sdmc:%s", part);
				remove(sp);
			}
			break;
		case DL_REMOVE:
			if (it->st == DL_RUN)
				r = -2;
			else {
				if (it->st != DL_DONE) {
					char sp[720];
					snprintf(sp, sizeof sp, "sdmc:%s", part);
					remove(sp);
				}
				int i = (int)(it - D.it);
				memmove(&D.it[i], &D.it[i + 1], (D.n - i - 1) * sizeof D.it[0]);
				D.n--;
			}
			break;
		}
		D.rev++;
	}
	unlock();
	LightEvent_Signal(&D.ev);
	return r;
}

/* ---------------------------------------------------------------- GitHub */

typedef struct {
	char *p;
	size_t n, max;
} Mem;

static size_t mem_cb(char *d, size_t s, size_t m, void *u)
{
	Mem *b = u;
	size_t k = s * m;
	if (b->n + k > b->max)
		return 0;
	char *np = realloc(b->p, b->n + k + 1);
	if (!np)
		return 0;
	memcpy(np + b->n, d, k);
	b->p = np, b->n += k, b->p[b->n] = 0;
	return k;
}

/* ---------------------------------------------------------------- httpc
 * Se descarga con el servicio HTTP del sistema (httpc), como FBI: el TLS lo hace el propio
 * sistema y va mucho más rápido que libcurl+mbedTLS en la CPU de la app. El certificado se
 * comprueba con las raíces del sistema más las de romfs/certs/httpc_roots.bin (las modernas que
 * le faltan). Si httpc no consigue conectar (p. ej. un certificado que no sabe comprobar), se
 * prueba con libcurl, que lleva todas las raíces de Mozilla. Sin comprobar el certificado solo
 * se baja si el usuario lo pide para esa descarga. */
static u32 hc_chain;
static bool hc_ready;

static void hc_init(void)
{
	if (R_FAILED(httpcInit(0)))
		return;
	hc_ready = true;
	if (R_FAILED(httpcCreateRootCertChain(&hc_chain))) {
		hc_chain = 0;
		return;
	}
	u32 h;
	for (int id = SSLC_DefaultRootCert_Nintendo_CA; id <= SSLC_DefaultRootCert_DigiCert_EV; id++)
		httpcRootCertChainAddDefaultCert(hc_chain, (SSLC_DefaultRootCert)id, &h);
	FILE *f = fopen(HC_ROOTS, "rb");
	int n = 0;
	if (f) {
		u8 len[4];
		while (fread(len, 1, 4, f) == 4) {
			u32 l = len[0] | len[1] << 8 | len[2] << 16 | (u32)len[3] << 24;
			u8 *der = l < 8192 ? malloc(l) : NULL;
			if (!der || fread(der, 1, l, f) != l) {
				free(der);
				break;
			}
			if (R_SUCCEEDED(httpcRootCertChainAddCert(hc_chain, der, l, &h)))
				n++;
			free(der);
		}
		fclose(f);
	}
	util_log("dl: httpc listo, %d raíces añadidas", n);
}

enum { HC_OK, HC_NOCONN, HC_HTTP, HC_NET, HC_ABORT, HC_WRITE };

/* abre la petición (siguiendo redirecciones) y la deja lista para leer el cuerpo */
static int hc_open(httpcContext *ctx, char *url, size_t un, bool verify, u64 range_from, const char *accept, u32 *code)
{
	for (int redir = 0; redir < 10; redir++) {
		Result r = httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 0);
		if (R_FAILED(r)) {
			util_log("dl: httpc abrir 0x%08lX", (unsigned long)r);
			return HC_NOCONN;
		}
		httpcSetSSLOpt(ctx, verify ? SSLCOPT_Default : SSLCOPT_DisableVerify);
		if (verify && hc_chain)
			httpcSelectRootCertChain(ctx, hc_chain);
		httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
		httpcAddRequestHeaderField(ctx, "User-Agent", t_ua ? t_ua : UA);
		httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");
		if (t_ua) // el navegador: el idioma, como cualquier navegador
			httpcAddRequestHeaderField(ctx, "Accept-Language", "es-ES,es;q=0.9,en;q=0.7");
		if (t_ua && !range_from) // y comprimido: HTML, CSS y JavaScript bajan unas 4 veces menos (lo
			httpcAddRequestHeaderField(ctx, "Accept-Encoding", "gzip"); // descomprime fetch_net, en el navegador)
		if (t_ck_get) {
			char ck[2048];
			if (t_ck_get(url, ck, sizeof ck))
				httpcAddRequestHeaderField(ctx, "Cookie", ck);
		}
		if (accept)
			httpcAddRequestHeaderField(ctx, "Accept", accept);
		if (range_from) {
			char rg[40];
			snprintf(rg, sizeof rg, "bytes=%llu-", (unsigned long long)range_from);
			httpcAddRequestHeaderField(ctx, "Range", rg);
		}
		r = httpcBeginRequest(ctx);
		if (R_SUCCEEDED(r))
			r = httpcGetResponseStatusCodeTimeout(ctx, code, 30ull * 1000000000ull);
		if (R_FAILED(r)) {
			util_log("dl: httpc %s -> 0x%08lX", url, (unsigned long)r);
			httpcCloseContext(ctx);
			return HC_NOCONN;
		}
		if (t_ck_set) { // httpc da la primera Set-Cookie (la de sesión, casi siempre)
			char sc[1024] = "";
			if (R_SUCCEEDED(httpcGetResponseHeader(ctx, "Set-Cookie", sc, sizeof sc)) && sc[0])
				t_ck_set(url, sc);
		}
		if (*code >= 300 && *code < 400 && *code != 304) {
			char loc[1024] = "";
			httpcGetResponseHeader(ctx, "Location", loc, sizeof loc);
			httpcCloseContext(ctx);
			if (!loc[0])
				return HC_HTTP;
			if (loc[0] == '/') { // relativa: mismo esquema y servidor
				const char *p = strstr(url, "://");
				size_t hl = p ? (size_t)(strchr(p + 3, '/') ? strchr(p + 3, '/') - url : (ptrdiff_t)strlen(url)) : 0;
				char nu[1024];
				snprintf(nu, sizeof nu, "%.*s%s", (int)hl, url, loc);
				util_strlcpy(url, nu, un);
			} else
				util_strlcpy(url, loc, un);
			continue;
		}
		return HC_OK;
	}
	return HC_HTTP;
}

/* para otros módulos (el actualizador): una petición GET con httpc, comprobando el certificado */
int dl_fetch(const char *url0, const char *accept, size_t (*wr)(char *, size_t, size_t, void *), void (*tot)(void *, u64), void *u, long *code_out)
{
	if (!hc_ready)
		return DLF_NOCONN;
	char url[1024];
	util_strlcpy(url, url0, sizeof url);
	httpcContext ctx;
	u32 code = 0;
	int e = hc_open(&ctx, url, sizeof url, true, 0, accept, &code);
	if (e != HC_OK)
		return e == HC_NOCONN ? DLF_NOCONN : DLF_FAIL;
	util_strlcpy(t_final, url, sizeof t_final);
	t_ctype[0] = 0;
	httpcGetResponseHeader(&ctx, "Content-Type", t_ctype, sizeof t_ctype);
	*code_out = code;
	if (code >= 400) {
		httpcCloseContext(&ctx);
		return DLF_OK;
	}
	u8 *buf = malloc(64 * 1024);
	Result r = -1;
	u32 dl = 0, total = 0, before;
	int out = buf ? DLF_OK : DLF_FAIL;
	while (buf) {
		before = dl;
		r = httpcReceiveDataTimeout(&ctx, buf, 64 * 1024, 30ull * 1000000000ull);
		httpcGetDownloadSizeState(&ctx, &dl, &total);
		if (tot && total)
			tot(u, total);
		if (dl > before && wr((char *)buf, 1, dl - before, u) != dl - before) {
			out = DLF_FAIL;
			break;
		}
		if (r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
			if (R_FAILED(r))
				out = DLF_FAIL;
			break;
		}
	}
	free(buf);
	httpcCloseContext(&ctx);
	return out;
}

/* petición general con httpc (para la nube): método, cabeceras «Nombre: valor» (lista acabada en
 * NULL), cuerpo opcional y respuesta por wr. Comprueba el certificado. Sigue redirecciones solo en GET */
static int dl_request_curl(int method, const char *url, const char *const *headers, const void *body, size_t blen,
                           size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out);
static void common_opts(CURL *c, char *eb);
static size_t curl_hdr_cb(char *d, size_t sz, size_t nm, void *u);
static CURLSH *SH;

/* servidores con los que httpc no conecta (TLS moderno: Microsoft, por ejemplo): directos a libcurl */
static char NOHC[16][96];
static int NOHC_I;
static LightLock NOHC_L;
static bool NOHC_OK;

static void nohc_key(const char *url, char *h, size_t n)
{
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	size_t l = strcspn(p, "/?#");
	snprintf(h, n, "%.*s", (int)(l < n - 1 ? l : n - 1), p);
}

static bool nohc_has(const char *url)
{
	if (!NOHC_OK)
		return false;
	char h[96];
	nohc_key(url, h, sizeof h);
	bool r = false;
	LightLock_Lock(&NOHC_L);
	for (int i = 0; i < 16 && !r; i++)
		r = NOHC[i][0] && !strcmp(NOHC[i], h);
	LightLock_Unlock(&NOHC_L);
	return r;
}

static void nohc_add(const char *url)
{
	if (!NOHC_OK || nohc_has(url))
		return;
	LightLock_Lock(&NOHC_L);
	nohc_key(url, NOHC[NOHC_I], sizeof NOHC[0]);
	NOHC_I = (NOHC_I + 1) % 16;
	LightLock_Unlock(&NOHC_L);
}

static int dl_request_hc(int method, const char *url0, const char *const *headers, const void *body, size_t blen,
                         size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out);

/* petición general (la nube, los formularios del navegador): con httpc y, si no conecta (su TLS es
 * antiguo y muchos servidores ya no lo aceptan), con libcurl */
int dl_request(int method, const char *url0, const char *const *headers, const void *body, size_t blen,
               size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out)
{
	int r = DLF_NOCONN;
	bool skip = nohc_has(url0);
	if (!skip && hc_ready)
		r = dl_request_hc(method, url0, headers, body, blen, wr, u, code_out);
	if (r != DLF_NOCONN)
		return r;
	if (!skip)
		util_log("http: httpc no conecta con %.80s: libcurl", url0);
	r = dl_request_curl(method, url0, headers, body, blen, wr, u, code_out);
	if (r == DLF_OK && !skip)
		nohc_add(url0);
	return r;
}

static size_t sink_cb(char *d, size_t s, size_t m, void *u)
{
	(void)d, (void)u;
	return s * m;
}

static int dl_request_curl(int method, const char *url, const char *const *headers, const void *body, size_t blen,
                           size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out)
{
	CURL *c = curl_easy_init();
	if (!c)
		return DLF_FAIL;
	char eb[CURL_ERROR_SIZE] = "";
	struct curl_slist *h = NULL;
	for (int i = 0; headers && headers[i]; i++)
		h = curl_slist_append(h, headers[i]);
	h = curl_slist_append(h, "Expect:"); // sin «100-continue»: algunos servidores no lo contestan
	common_opts(c, eb);
	if (t_ua)
		curl_easy_setopt(c, CURLOPT_USERAGENT, t_ua);
	if (SH)
		curl_easy_setopt(c, CURLOPT_SHARE, SH);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, method == HTTPC_METHOD_GET ? 1L : 0L);
	if (method == HTTPC_METHOD_POST || method == HTTPC_METHOD_PUT) {
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(body ? blen : 0));
	}
	if (method == HTTPC_METHOD_PUT)
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
	else if (method == HTTPC_METHOD_DELETE)
		curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	else if (method == HTTPC_METHOD_HEAD)
		curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
	char ck[2048];
	if (t_ck_get && t_ck_get(url, ck, sizeof ck))
		curl_easy_setopt(c, CURLOPT_COOKIE, ck);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_hdr_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr ? wr : sink_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, u);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	util_log("http: curl %.80s -> %d, HTTP %ld %s", url, (int)r, code, eb);
	*code_out = code;
	return r == CURLE_OK ? DLF_OK : (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT) ? DLF_NOCONN : DLF_FAIL;
}

static int dl_request_hc(int method, const char *url0, const char *const *headers, const void *body, size_t blen,
                         size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out)
{
	char url[2048];
	util_strlcpy(url, url0, sizeof url);
	httpcContext ctx;
	u32 code = 0;
	Result r;
	for (int redir = 0;; redir++) {
		if (R_FAILED(httpcOpenContext(&ctx, (HTTPC_RequestMethod)method, url, 0)))
			return DLF_NOCONN;
		httpcSetSSLOpt(&ctx, SSLCOPT_Default);
		if (hc_chain)
			httpcSelectRootCertChain(&ctx, hc_chain);
		httpcSetKeepAlive(&ctx, HTTPC_KEEPALIVE_ENABLED);
		httpcAddRequestHeaderField(&ctx, "User-Agent", t_ua ? t_ua : UA);
		if (t_ck_get) {
			char ck[2048];
			if (t_ck_get(url, ck, sizeof ck))
				httpcAddRequestHeaderField(&ctx, "Cookie", ck);
		}
		for (int i = 0; headers && headers[i]; i++) {
			char name[64];
			const char *c = strchr(headers[i], ':');
			if (!c || c - headers[i] >= (int)sizeof name)
				continue;
			snprintf(name, sizeof name, "%.*s", (int)(c - headers[i]), headers[i]);
			const char *v = c + 1;
			while (*v == ' ')
				v++;
			httpcAddRequestHeaderField(&ctx, name, v);
		}
		if (body && blen)
			httpcAddPostDataRaw(&ctx, (const u32 *)body, blen);
		r = httpcBeginRequest(&ctx);
		if (R_SUCCEEDED(r))
			r = httpcGetResponseStatusCodeTimeout(&ctx, &code, 60ull * 1000000000ull);
		if (R_FAILED(r)) {
			util_log("http: %s -> 0x%08lX", url, (unsigned long)r);
			httpcCloseContext(&ctx);
			return DLF_NOCONN;
		}
		if (t_ck_set) {
			char sc[1024] = "";
			if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Set-Cookie", sc, sizeof sc)) && sc[0])
				t_ck_set(url, sc);
		}
		if (method == HTTPC_METHOD_GET && code >= 300 && code < 400 && code != 304 && redir < 8) {
			char loc[2048] = "";
			httpcGetResponseHeader(&ctx, "Location", loc, sizeof loc);
			httpcCloseContext(&ctx);
			if (!loc[0])
				return DLF_FAIL;
			util_strlcpy(url, loc, sizeof url);
			continue;
		}
		break;
	}
	*code_out = code;
	u8 *buf = malloc(64 * 1024);
	u32 dl = 0, total = 0, before;
	int out = buf ? DLF_OK : DLF_FAIL;
	while (buf) {
		before = dl;
		r = httpcReceiveDataTimeout(&ctx, buf, 64 * 1024, 60ull * 1000000000ull);
		httpcGetDownloadSizeState(&ctx, &dl, &total);
		if (dl > before && wr && wr((char *)buf, 1, dl - before, u) != dl - before) {
			out = DLF_FAIL;
			break;
		}
		if (r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
			if (R_FAILED(r))
				out = DLF_FAIL;
			break;
		}
	}
	free(buf);
	httpcCloseContext(&ctx);
	return out;
}

/* una respuesta pequeña entera en memoria (la API de GitHub) */
static int hc_get_mem(const char *url0, const char *accept, Mem *m, long *code_out)
{
	if (!hc_ready)
		return HC_NOCONN;
	char url[1024];
	util_strlcpy(url, url0, sizeof url);
	httpcContext ctx;
	u32 code = 0;
	int e = hc_open(&ctx, url, sizeof url, true, 0, accept, &code);
	if (e != HC_OK)
		return e;
	*code_out = code;
	u8 *buf = malloc(64 * 1024);
	Result r;
	u32 dl = 0, tot = 0, before;
	do {
		before = dl;
		r = buf ? httpcReceiveDataTimeout(&ctx, buf, 64 * 1024, 30ull * 1000000000ull) : -1;
		httpcGetDownloadSizeState(&ctx, &dl, &tot);
		if (buf && dl > before && mem_cb((char *)buf, 1, dl - before, m) != dl - before) {
			r = -1;
			break;
		}
	} while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && !D.quit && !D.cancel);
	free(buf);
	httpcCloseContext(&ctx);
	return R_FAILED(r) && r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING ? HC_NET : HC_OK;
}


static int quit_cb(void *u, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
	(void)u, (void)a, (void)b, (void)c, (void)d;
	return D.quit || D.cancel;
}

/* más búfer de recepción en el socket: con el de serie, la consola pide los datos a sorbos */
static int sockopt_cb(void *u, curl_socket_t fd, curlsocktype purpose)
{
	(void)purpose;
	if (!u) // solo las descargas grandes: el búfer de soc:U (1 MB) es para todos los sockets, y con
		return CURL_SOCKOPT_OK; // varios a la vez (el navegador baja la página, su CSS e imágenes) se agotaba
	int sz = 256 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	return CURL_SOCKOPT_OK;
}

/* conexiones y sesiones TLS compartidas entre peticiones (y hilos) del navegador: la segunda
 * petición al mismo servidor reutiliza la conexión o al menos la sesión TLS, sin volver a hacer el
 * saludo entero (en la CPU de la 3DS, segundos por petición) */
static CURLSH *SH;
static LightLock SH_LOCK[CURL_LOCK_DATA_LAST];
static void sh_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u)
{
	(void)h, (void)a, (void)u;
	LightLock_Lock(&SH_LOCK[d]);
}
static void sh_unlock(CURL *h, curl_lock_data d, void *u)
{
	(void)h, (void)u;
	LightLock_Unlock(&SH_LOCK[d]);
}
static void sh_init(void)
{
	for (int i = 0; i < CURL_LOCK_DATA_LAST; i++)
		LightLock_Init(&SH_LOCK[i]);
	SH = curl_share_init();
	if (!SH)
		return;
	curl_share_setopt(SH, CURLSHOPT_LOCKFUNC, sh_lock);
	curl_share_setopt(SH, CURLSHOPT_UNLOCKFUNC, sh_unlock);
	curl_share_setopt(SH, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_share_setopt(SH, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_share_setopt(SH, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

static void common_opts(CURL *c, char *eb)
{
	curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
	curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, eb);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
	curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE); /* raíces de Mozilla, siempre verificado */
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024);
}

static size_t curl_hdr_cb(char *d, size_t sz, size_t nm, void *u)
{
	size_t n = sz * nm;
	if (t_ck_set && n > 11 && !strncasecmp(d, "Set-Cookie:", 11)) {
		char v[1024];
		size_t l = n - 11 < sizeof v - 1 ? n - 11 : sizeof v - 1;
		memcpy(v, d + 11, l);
		v[l] = 0;
		v[strcspn(v, "\r\n")] = 0;
		t_ck_set((const char *)u, v[0] == ' ' ? v + 1 : v);
	}
	return n;
}

/* como dl_fetch, pero con libcurl (TLS moderno de mbedTLS): para lo que httpc no logra */
int dl_fetch_curl(const char *url, const char *accept, size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code_out)
{
	CURL *c = curl_easy_init();
	if (!c)
		return DLF_FAIL;
	char eb[CURL_ERROR_SIZE] = "", acc[160];
	snprintf(acc, sizeof acc, "Accept: %s", accept ? accept : "*/*");
	struct curl_slist *h = curl_slist_append(NULL, acc);
	if (t_ua)
		h = curl_slist_append(h, "Accept-Language: es-ES,es;q=0.9,en;q=0.7");
	common_opts(c, eb);
	if (t_ua)
		curl_easy_setopt(c, CURLOPT_USERAGENT, t_ua);
	if (SH)
		curl_easy_setopt(c, CURLOPT_SHARE, SH);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // gzip/deflate: baja menos y libcurl lo descomprime
	char ck[2048];
	if (t_ck_get && t_ck_get(url, ck, sizeof ck))
		curl_easy_setopt(c, CURLOPT_COOKIE, ck);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_hdr_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *)url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wr);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, u);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, quit_cb);
	CURLcode r = curl_easy_perform(c);
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	char *eff = NULL;
	curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
	if (eff)
		util_strlcpy(t_final, eff, sizeof t_final);
	char *ct = NULL;
	curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
	util_strlcpy(t_ctype, ct ? ct : "", sizeof t_ctype);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	util_log("dl: curl %.80s -> %d, HTTP %ld %s", url, (int)r, code, eb);
	*code_out = code;
	return r == CURLE_OK ? DLF_OK : (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT) ? DLF_NOCONN : DLF_FAIL;
}

static const char *curl_err(CURLcode r, long code)
{
	if (r == CURLE_COULDNT_RESOLVE_HOST || r == CURLE_COULDNT_CONNECT || r == CURLE_OPERATION_TIMEDOUT)
		return "offline";
	if (r == CURLE_PEER_FAILED_VERIFICATION || r == CURLE_SSL_CONNECT_ERROR || r == CURLE_SSL_CACERT_BADFILE)
		return "tls";
	if (r != CURLE_OK)
		return "net";
	if (code == 404 || code == 410)
		return "not_found";
	if (code == 403 || code == 429)
		return "denied";
	if (code >= 400)
		return "http";
	return NULL;
}

/* github.com/dueño/repo[/releases[/latest|/tag/x]] → el .cia de esa release
 * (los enlaces directos, /releases/download/…, se bajan tal cual) */
static const char *github_resolve(DlItem *it, char *url, size_t un, char *name, size_t nn)
{
	const char *p = NULL;
	if (!strncasecmp(it->url, "https://github.com/", 19))
		p = it->url + 19;
	else if (!strncasecmp(it->url, "http://github.com/", 18))
		p = it->url + 18;
	if (!p || strstr(p, "/releases/download/") || strstr(p, "/raw/") || strstr(p, "/archive/") || strstr(p, "/blob/"))
		return NULL;
	char owner[100], repo[100], rest[300] = "";
	if (sscanf(p, "%99[^/]/%99[^/?#]%299s", owner, repo, rest) < 2)
		return NULL;
	size_t rl = strlen(repo);
	if (rl > 4 && !strcmp(repo + rl - 4, ".git"))
		repo[rl - 4] = 0;
	char api[400];
	const char *tag = strstr(rest, "/releases/tag/");
	if (tag) {
		char t[120];
		snprintf(t, sizeof t, "%s", tag + 14);
		t[strcspn(t, "/?#")] = 0;
		snprintf(api, sizeof api, "https://api.github.com/repos/%s/%s/releases/tags/%s", owner, repo, t);
	} else if (!rest[0] || rest[0] == '?' || rest[0] == '#' || !strncmp(rest, "/releases", 9))
		snprintf(api, sizeof api, "https://api.github.com/repos/%s/%s/releases/latest", owner, repo);
	else
		return NULL; // otra página de GitHub: se baja tal cual
	Mem m = {NULL, 0, 512 * 1024};
	long code = 0;
	int hr = hc_get_mem(api, "application/vnd.github+json", &m, &code);
	util_log("dl: github %s -> httpc %d, HTTP %ld", api, hr, code);
	const char *e = hr == HC_OK ? curl_err(CURLE_OK, code) : hr == HC_NOCONN ? "?" : "net";
	if (e && strcmp(e, "?")) {
		free(m.p);
		return e;
	}
	if (e) { // httpc no conectó: con libcurl
	free(m.p);
	m.p = NULL, m.n = 0;
	CURL *c = curl_easy_init();
	if (!c)
		return "net";
	char eb[CURL_ERROR_SIZE] = "";
	struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
	common_opts(c, eb);
	curl_easy_setopt(c, CURLOPT_URL, api);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, quit_cb);
	CURLcode r = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	curl_slist_free_all(h);
	util_log("dl: github %s -> curl %d, HTTP %ld %s", api, (int)r, code, eb);
	e = curl_err(r, code);
	if (e) {
		free(m.p);
		return e;
	}
	}
	cJSON *j = cJSON_Parse(m.p ? m.p : "");
	free(m.p);
	const cJSON *a, *assets = cJSON_GetObjectItemCaseSensitive(j, "assets");
	bool found = false;
	cJSON_ArrayForEach(a, assets)
	{
		const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, "name"), *u = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
		if (!cJSON_IsString(n) || !cJSON_IsString(u))
			continue;
		size_t l = strlen(n->valuestring);
		if (l > 4 && !strcasecmp(n->valuestring + l - 4, ".cia")) {
			util_strlcpy(url, u->valuestring, un);
			util_strlcpy(name, n->valuestring, nn);
			found = true;
			break;
		}
	}
	cJSON_Delete(j);
	return found ? NULL : "no_cia";
}

/* ---------------------------------------------------------------- bajar */

typedef struct {
	DlItem *snap;      /* copia de trabajo (fuera del cerrojo) */
	char part[700];
	FILE *f;
	u64 base;          /* lo que ya había (al reanudar) */
	long status;
	bool ranged;       /* el servidor respondió 206 */
	bool accept_ranges;
	char cd_name[256]; /* de Content-Disposition */
	int xid;
	u64 t_last, d_last, speed;
	bool no_space;
	/* instalar mientras baja: primero se juntan los primeros KB para saber qué CIA es */
	int smode;         /* SM_* */
	u8 *hb;
	size_t hn;
	InsStream *is;
	char serr[24];
	u64 sdone;         /* lo que ya recibió AM */
	bool blocked;      /* un título del sistema: ni se instala ni se guarda */
} Job;

enum { SM_FILE, SM_PROBE, SM_STREAM };
#define SM_HEAD_MAX (128 * 1024)

static void cd_filename(const char *v, char *out, size_t n)
{
	const char *p = ci_find(v, "filename*=");
	char tmp[400] = "";
	if (p) {
		p += 10;
		const char *q = strstr(p, "''");
		p = q ? q + 2 : p;
		snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, ";\r\n"), p);
		util_url_decode(tmp);
	} else if ((p = ci_find(v, "filename="))) {
		p += 9;
		if (*p == '"') {
			p++;
			snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, "\"\r\n"), p);
		} else
			snprintf(tmp, sizeof tmp, "%.*s", (int)strcspn(p, ";\r\n"), p);
	}
	const char *b = strrchr(tmp, '/');
	b = b ? b + 1 : tmp;
	const char *b2 = strrchr(b, '\\');
	b = b2 ? b2 + 1 : b;
	size_t o = 0;
	for (; *b && o + 1 < n; b++)
		out[o++] = (unsigned char)*b < 32 || strchr("\\/:*?\"<>|", *b) ? '_' : *b;
	out[o] = 0;
	while (o && (out[o - 1] == '.' || out[o - 1] == ' '))
		out[--o] = 0;
}

static size_t head_cb(char *d, size_t s, size_t m, void *u)
{
	Job *j = u;
	size_t k = s * m;
	char line[512];
	snprintf(line, sizeof line, "%.*s", (int)(k < sizeof line - 1 ? k : sizeof line - 1), d);
	if (!strncasecmp(line, "HTTP/", 5)) { // empieza otra respuesta (redirección): se olvida lo anterior
		const char *sp = strchr(line, ' ');
		j->status = sp ? strtol(sp + 1, NULL, 10) : 0;
		j->accept_ranges = false;
		j->cd_name[0] = 0;
	} else if (!strncasecmp(line, "Content-Disposition:", 20))
		cd_filename(line + 20, j->cd_name, sizeof j->cd_name);
	else if (!strncasecmp(line, "Accept-Ranges:", 14) && ci_find(line + 14, "bytes"))
		j->accept_ranges = true;
	else if (!strncasecmp(line, "Content-Range:", 14))
		j->accept_ranges = true;
	return k;
}

static size_t file_write(Job *j, const char *d, size_t k);
static void dl_mark(u32 id, bool installing);

static size_t body_cb(char *d, size_t s, size_t m, void *u)
{
	Job *j = u;
	size_t k = s * m;
	if (j->smode == SM_STREAM) {
		if (!ins_stream_write(j->is, d, k, j->serr, sizeof j->serr))
			return 0;
		j->sdone += k;
		return k;
	}
	if (j->smode == SM_PROBE) {
		u8 *nb = realloc(j->hb, j->hn + k);
		if (!nb)
			return 0;
		memcpy(nb + j->hn, d, k);
		j->hb = nb, j->hn += k;
		int need = ins_stream_need(j->hb, j->hn);
		if (need >= 0 && need <= SM_HEAD_MAX && (size_t)need > j->hn)
			return k; // aún no se sabe: se sigue juntando
		if (need >= 0 && need <= SM_HEAD_MAX)
			j->is = ins_stream_begin(j->hb, j->hn, j->serr, sizeof j->serr);
		if (j->is) {
			j->smode = SM_STREAM;
			dl_mark(j->snap->id, true);
			bool ok = ins_stream_write(j->is, j->hb, j->hn, j->serr, sizeof j->serr);
			j->sdone = j->hn;
			free(j->hb), j->hb = NULL, j->hn = 0;
			return ok ? k : 0;
		}
		if (!strcmp(j->serr, "system_title")) {
			j->blocked = true;
			return 0;
		}
		// no es un CIA o ahora no se puede (otra instalación, Cloud3DS…): se guarda y se ofrece al terminar
		j->smode = SM_FILE;
		size_t hn = j->hn;
		u8 *hb = j->hb;
		j->hb = NULL, j->hn = 0;
		size_t w = file_write(j, (const char *)hb, hn);
		free(hb);
		return w == hn ? k : 0;
	}
	return file_write(j, d, k);
}

static size_t file_write(Job *j, const char *d, size_t k)
{
	if (!j->f) { // primeros datos: 206 sigue donde se quedó; 200 empieza de cero
		char sp[720];
		snprintf(sp, sizeof sp, "sdmc:%s", j->part);
		j->ranged = j->status == 206 && j->base > 0;
		if (!j->ranged)
			j->base = 0;
		j->f = fopen(sp, j->ranged ? "ab" : "wb");
		if (!j->f)
			return 0;
		setvbuf(j->f, NULL, _IOFBF, 256 * 1024); // a la SD en bloques grandes
	}
	return fwrite(d, 1, k, j->f) == k ? k : 0;
}

static int prog_cb(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
	(void)ut, (void)un;
	Job *j = u;
	if (j->status >= 200 && j->status < 300 && dt > 0) {
		u64 total = (j->status == 206 ? j->base : 0) + (u64)dt;
		if (j->snap->total != total) {
			j->snap->total = total;
			state_lock();
			Xfer *x = xfer_get(j->xid);
			if (x)
				x->total = total;
			state_unlock();
			u64 fr = 0, tot = 0, have = (j->status == 206 ? j->base : 0) + (u64)dn;
			if (fsx_space(&fr, &tot) && fr < total - have + 512 * 1024) {
				j->no_space = true;
				return 1;
			}
		}
	}
	u64 done = (j->ranged || (j->status == 206) ? j->base : 0) + (u64)dn;
	u64 now = util_ms();
	if (now - j->t_last >= 500) {
		u64 dd = done > j->d_last ? done - j->d_last : 0;
		u64 sp = dd * 1000 / (now - j->t_last ? now - j->t_last : 1);
		j->speed = j->speed ? (j->speed * 3 + sp) / 4 : sp;
		j->t_last = now, j->d_last = done;
		lock();
		DlItem *it = find(j->snap->id);
		if (it) {
			it->done = done, it->total = j->snap->total, it->speed = j->speed;
			D.rev++;
		}
		unlock();
		state_lock();
		xfer_progress(j->xid, done);
		state_unlock();
	}
	return D.quit || D.pause || D.cancel;
}

/* la descarga con httpc: cabeceras como head_cb y el cuerpo por body_cb y prog_cb */
static int hc_body(Job *j, const char *url0, bool verify, long *code_out, char *eff, size_t en)
{
	char url[2048];
	util_strlcpy(url, url0, sizeof url);
	httpcContext ctx;
	u32 code = 0;
	int e = hc_open(&ctx, url, sizeof url, verify, j->base, NULL, &code);
	if (e != HC_OK)
		return e;
	*code_out = code;
	j->status = code;
	util_strlcpy(eff, url, en);
	char v[512];
	if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Content-Disposition", v, sizeof v)) && v[0])
		cd_filename(v, j->cd_name, sizeof j->cd_name);
	if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Accept-Ranges", v, sizeof v)) && ci_find(v, "bytes"))
		j->accept_ranges = true;
	if (R_SUCCEEDED(httpcGetResponseHeader(&ctx, "Content-Range", v, sizeof v)) && v[0])
		j->accept_ranges = true;
	if (code >= 400) {
		httpcCloseContext(&ctx);
		return HC_HTTP;
	}
	u8 *buf = malloc(HC_BUF);
	if (!buf) {
		httpcCloseContext(&ctx);
		return HC_NET;
	}
	u32 dl = 0, tot = 0, before;
	Result r;
	int out = HC_OK;
	do {
		before = dl;
		r = httpcReceiveDataTimeout(&ctx, buf, HC_BUF, 30ull * 1000000000ull);
		httpcGetDownloadSizeState(&ctx, &dl, &tot);
		if (dl > before && body_cb((char *)buf, 1, dl - before, j) != dl - before) {
			out = HC_WRITE;
			break;
		}
		if (prog_cb(j, tot, dl, 0, 0)) {
			out = HC_ABORT;
			break;
		}
	} while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING);
	if (out == HC_OK && R_FAILED(r)) {
		util_log("dl: httpc recibir 0x%08lX tras %lu bytes", (unsigned long)r, (unsigned long)dl);
		out = HC_NET;
	}
	free(buf);
	httpcCloseContext(&ctx);
	return out;
}

static void dl_mark(u32 id, bool installing)
{
	lock();
	DlItem *it = find(id);
	if (it) {
		it->installing = installing;
		D.rev++;
	}
	unlock();
}

/* comprueba el MD5 o SHA-256 del archivo; 1 coincide, 0 no */
static int check_hash(const char *path, const char *want)
{
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", path);
	FILE *f = fopen(sp, "rb");
	u8 *b = malloc(64 * 1024);
	if (!f || !b) {
		if (f)
			fclose(f);
		free(b);
		return 0;
	}
	bool md5 = strlen(want) == 32;
	Md5 m;
	Sha256 s;
	md5_init(&m);
	sha256_init(&s);
	size_t n;
	while ((n = fread(b, 1, 64 * 1024, f)) > 0 && !D.quit)
		md5 ? md5_update(&m, b, n) : sha256_update(&s, b, n);
	fclose(f);
	free(b);
	u8 d[32];
	char hex[65];
	if (md5) {
		md5_final(&m, d);
		digest_hex(d, 16, hex);
	} else {
		sha256_final(&s, d);
		digest_hex(d, 32, hex);
	}
	return !strcmp(hex, want);
}

static void set_state(u32 id, int st, const char *err, const DlItem *snap)
{
	lock();
	DlItem *it = find(id);
	if (it) {
		it->st = st;
		util_strlcpy(it->err, err ? err : "", sizeof it->err);
		if (snap) {
			it->done = snap->done, it->total = snap->total, it->speed = 0, it->resumable = snap->resumable;
			util_strlcpy(it->name, snap->name, sizeof it->name);
			util_strlcpy(it->path, snap->path, sizeof it->path);
			it->hash_ok = snap->hash_ok;
			it->after_done = snap->after_done;
		}
	}
	D.rev++;
	unlock();
}

/* final de una descarga que se instalaba mientras bajaba (o de un título del sistema, que se corta) */
static void stream_done(DlItem *s, Job *j, CURLcode r, long code, const char *gname, const char *url)
{
	bool ok = !j->blocked && r == CURLE_OK && (!code || (code >= 200 && code < 300)) && !D.cancel && !D.quit && !j->no_space;
	const char *ie = j->is ? ins_stream_end(j->is, ok) : NULL;
	j->is = NULL;
	s->resumable = false;
	s->installing = false;
	s->done = j->sdone;
	util_log("dl: %s -> instalando al bajar: %s (%llu bytes)", url, j->blocked ? "sistema" : ok ? (ie ? ie : "ok") : "cortado",
	         (unsigned long long)j->sdone);
	int xs = XS_FAILED;
	if (j->blocked)
		set_state(s->id, DL_FAIL, "system_title", s);
	else if (D.cancel) {
		set_state(s->id, DL_CANCELED, NULL, s);
		xs = XS_CANCELED;
	} else if (!ok)
		set_state(s->id, DL_FAIL, j->no_space ? "no_space" : j->serr[0] ? j->serr : (curl_err(r == CURLE_HTTP_RETURNED_ERROR ? CURLE_OK : r, code) ?: "net"), s);
	else if (ie)
		set_state(s->id, DL_FAIL, ie, s);
	else {
		char nm[256];
		util_strlcpy(nm, s->fixed_name ? s->name : gname[0] ? gname : j->cd_name[0] ? j->cd_name : s->name[0] ? s->name : "descarga", sizeof nm);
		if (!s->fixed_name && !j->cd_name[0] && !gname[0] && !s->name[0])
			name_from_url(url, nm, sizeof nm);
		util_strlcpy(s->name, nm, sizeof s->name);
		s->total = j->sdone;
		s->installed = true;
		s->after_done = true;
		s->path[0] = 0;
		xs = XS_DONE;
		state_lock();
		event_add(EV_FETCH, nm, s->host, j->sdone);
		event_add(EV_INSTALL, nm, s->device[0] ? s->device : s->host, j->sdone);
		state_unlock();
		set_state(s->id, DL_DONE, NULL, s);
	}
	lock();
	DlItem *it = find(s->id);
	if (it) {
		it->installing = false;
		it->installed = s->installed;
	}
	unlock();
	state_lock();
	xfer_progress(j->xid, j->sdone);
	xfer_end(j->xid, xs);
	state_unlock();
}

static void run(DlItem *s)
{
	Job j = {0};
	j.snap = s;
	part_path(s->id, s->dir, j.part, sizeof j.part);
	char url[2048], gname[256] = "";
	util_strlcpy(url, s->url, sizeof url);
	const char *e = github_resolve(s, url, sizeof url, gname, sizeof gname);
	if (e) {
		set_state(s->id, DL_FAIL, e, NULL);
		return;
	}
	if (gname[0]) {
		util_strlcpy(s->name, gname, sizeof s->name);
		set_state(s->id, DL_RUN, NULL, s);
	}
	if (fsx_mkdirs(s->dir) < 0) {
		set_state(s->id, DL_FAIL, "io", NULL);
		return;
	}
	bool d;
	u64 have = 0;
	if (s->resumable && fsx_stat(j.part, &d, &have) == 0 && !d)
		j.base = have;
	// pedido desde la consola, sin suma que comprobar y desde el principio: un .cia se instala mientras baja
	if ((s->after & DLA_STREAM) && (s->after & DLA_INSTALL) && !s->hash[0] && !j.base)
		j.smode = SM_PROBE;
	state_lock();
	j.xid = xfer_begin(XF_FETCH, s->name[0] ? s->name : s->host, s->host, s->total);
	state_unlock();
	j.t_last = util_ms();
	j.d_last = j.base;
	char eb[CURL_ERROR_SIZE] = "";
	CURLcode r = CURLE_FAILED_INIT;
	long code = 0;
	char eff[1024] = "";
	int hr = hc_ready ? hc_body(&j, url, !s->insecure, &code, eff, sizeof eff) : HC_NOCONN;
	const char *engine = "httpc";
	if (hr == HC_OK)
		r = CURLE_OK;
	else if (hr == HC_HTTP)
		r = CURLE_HTTP_RETURNED_ERROR;
	else if (hr == HC_ABORT)
		r = CURLE_ABORTED_BY_CALLBACK;
	else if (hr == HC_WRITE)
		r = CURLE_WRITE_ERROR;
	else if (hr == HC_NET)
		r = CURLE_RECV_ERROR;
	if (hr == HC_NOCONN && !j.f) { // httpc no pudo conectar: con libcurl y las raíces de Mozilla
		engine = "curl";
		j.status = 0, j.accept_ranges = false, j.cd_name[0] = 0;
		CURL *c = curl_easy_init();
		if (c) {
			common_opts(c, eb);
			curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, (void *)1); // descarga grande: búfer de socket grande
			if (s->insecure) { // lo pidió el usuario para esta descarga
				curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
				curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
			}
			curl_easy_setopt(c, CURLOPT_URL, url);
			curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, head_cb);
			curl_easy_setopt(c, CURLOPT_HEADERDATA, &j);
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_cb);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, &j);
			curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, prog_cb);
			curl_easy_setopt(c, CURLOPT_XFERINFODATA, &j);
			curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
			if (j.base)
				curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)j.base);
			r = curl_easy_perform(c);
			curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
			char *ce = NULL;
			curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &ce);
			if (ce)
				util_strlcpy(eff, ce, sizeof eff);
			curl_easy_cleanup(c);
		}
	}
	if (!j.cd_name[0] && eff[0] && !gname[0]) // el nombre del enlace final (tras las redirecciones)
		name_from_url(eff, j.cd_name, sizeof j.cd_name);
	if (j.smode == SM_PROBE && j.hn) { // tan pequeño que no llegó a saberse qué era: a la SD
		j.smode = SM_FILE;
		if (file_write(&j, (const char *)j.hb, j.hn) != j.hn)
			r = CURLE_WRITE_ERROR;
	}
	free(j.hb);
	j.hb = NULL;
	if (j.smode == SM_STREAM || j.blocked) {
		stream_done(s, &j, r, code, gname, url);
		return;
	}
	bool wrote_ok = true;
	if (j.f)
		wrote_ok = fclose(j.f) == 0;
	j.f = NULL;
	s->resumable = j.accept_ranges;
	u64 got = j.base;
	{
		bool dd;
		u64 sz;
		if (fsx_stat(j.part, &dd, &sz) == 0)
			got = sz;
	}
	s->done = got;
	util_log("dl: %s -> %s %d, HTTP %ld, %llu bytes %s", url, engine, (int)r, code, (unsigned long long)got, eb);
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", j.part);
	int xs = XS_FAILED;
	if (D.cancel) {
		remove(sp);
		set_state(s->id, DL_CANCELED, NULL, s);
		xs = XS_CANCELED;
	} else if (D.pause) {
		set_state(s->id, DL_PAUSED, NULL, s);
		xs = XS_PAUSED;
	} else if (j.no_space) {
		remove(sp);
		set_state(s->id, DL_FAIL, "no_space", s);
	} else if (r != CURLE_OK || !wrote_ok || (code && (code < 200 || code >= 300))) {
		const char *er = !wrote_ok || r == CURLE_WRITE_ERROR ? "io" : curl_err(r == CURLE_HTTP_RETURNED_ERROR ? CURLE_OK : r, code);
		if (!s->resumable)
			remove(sp);
		set_state(s->id, DL_FAIL, er ? er : "net", s);
	} else {
		// listo: su nombre (si ya existe, «x (2)»), la comprobación y lo de después
		char nm[256];
		util_strlcpy(nm, s->fixed_name ? s->name : gname[0] ? gname : j.cd_name[0] ? j.cd_name : s->name[0] ? s->name : "descarga", sizeof nm);
		if (!fsx_name_ok(nm))
			util_strlcpy(nm, "descarga", sizeof nm);
		util_strlcpy(s->name, nm, sizeof s->name);
		ops_free_name(s->path, sizeof s->path, s->dir, nm);
		char fp[720];
		snprintf(fp, sizeof fp, "sdmc:%s", s->path);
		if (rename(sp, fp) != 0) {
			remove(sp);
			set_state(s->id, DL_FAIL, "io", s);
		} else {
			s->total = got;
			if (s->hash[0])
				s->hash_ok = check_hash(s->path, s->hash);
			xs = XS_DONE;
			state_lock();
			event_add(EV_FETCH, nm, s->host, got);
			state_unlock();
			size_t l = strlen(nm);
			bool cia = l > 4 && !strcasecmp(nm + l - 4, ".cia"), zip = l > 4 && !strcasecmp(nm + l - 4, ".zip");
			if (s->hash_ok != 0 && (s->after & DLA_INSTALL) && cia && strlen(s->path) < 256) {
				char err[24] = "";
				if (ins_request(s->path, s->device[0] ? s->device : s->host, true, err, sizeof err) < 0)
					util_strlcpy(s->err, err, sizeof s->err);
				s->after_done = true;
			} else if (s->hash_ok != 0 && (s->after & DLA_UNZIP) && zip) {
				for (int k = 0; k < 600 && !D.quit; k++) { // si hay otro ZIP en marcha, se espera a que acabe
					int zr = zipjob_extract(s->path, false, s->device[0] ? s->device : s->host);
					if (zr != ZIP_BUSY) {
						if (zr != ZIP_OK)
							util_strlcpy(s->err, "unzip", sizeof s->err);
						break;
					}
					svcSleepThread(1000 * 1000000ll);
				}
				s->after_done = true;
			}
			set_state(s->id, DL_DONE, s->err[0] ? s->err : NULL, s);
		}
	}
	state_lock();
	xfer_progress(j.xid, got);
	xfer_end(j.xid, xs);
	state_unlock();
}

static void dl_thread(void *arg)
{
	(void)arg;
	while (!D.quit) {
		DlItem s;
		bool have = false;
		lock();
		for (int i = 0; i < D.n; i++)
			if (D.it[i].st == DL_QUEUED) {
				D.it[i].st = DL_RUN;
				D.it[i].err[0] = 0;
				s = D.it[i];
				have = true;
				D.cur = s.id;
				D.pause = D.cancel = false;
				D.rev++;
				break;
			}
		unlock();
		if (!have) {
			LightEvent_WaitTimeout(&D.ev, 1000 * 1000000ll);
			continue;
		}
		if (!util_ip()) { // sin Wi-Fi: se espera un poco antes de darlo por perdido
			for (int k = 0; k < 20 && !util_ip() && !D.quit; k++)
				svcSleepThread(500 * 1000000ll);
		}
		if (!util_ip())
			set_state(s.id, DL_FAIL, "offline", NULL);
		else
			run(&s);
		D.cur = 0;
	}
}

void dl_init(void)
{
	hc_init();
	sh_init();
	LightLock_Init(&NOHC_L);
	NOHC_OK = true;
	LightLock_Init(&D.lock);
	LightEvent_Init(&D.ev, RESET_ONESHOT);
	D.ready = true;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// como el servidor: en New 3DS, al núcleo 2; si no, en el de la app pero por delante de la interfaz
	// (casi siempre espera datos de la red; con menos prioridad, cada fotograma le quitaba la CPU y el TLS iba a sorbos)
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	D.thr = n3ds ? threadCreate(dl_thread, NULL, 64 * 1024, prio - 1, 2, false) : NULL;
	if (!D.thr)
		D.thr = threadCreate(dl_thread, NULL, 64 * 1024, prio - 1, 0, false);
}

void dl_exit(void)
{
	D.quit = true;
	LightEvent_Signal(&D.ev);
	if (D.thr) {
		threadJoin(D.thr, 5000000000ull);
		threadFree(D.thr);
		D.thr = NULL;
	}
	if (hc_ready) {
		if (hc_chain)
			httpcDestroyRootCertChain(hc_chain);
		httpcExit();
		hc_ready = false;
	}
}
