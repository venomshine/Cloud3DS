/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "reader.h"
#include "cJSON.h"
#include "fsx.h"
#include "util.h"

#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define RL_FILE APP_DIR "/lector.json"
#define NSLOT 8      // páginas dibujadas que se guardan
#define NPRE 4       // pedidas por adelantado
#define STORE (16u << 20)

/* newlib no trae timegm (MuPDF la usa para las fechas de los PDF): en la consola no hay zona horaria, así que
 * mktime ya trabaja en UTC */
time_t timegm(struct tm *tm)
{
	return mktime(tm);
}

int rd_kind(const char *path)
{
	const char *x = strrchr(path, '.');
	if (!x)
		return -1;
	x++;
	return !strcasecmp(x, "pdf") ? RK_PDF : !strcasecmp(x, "cbz") ? RK_CBZ : !strcasecmp(x, "epub") ? RK_EPUB
	     : !strcasecmp(x, "docx") ? RK_DOCX : !strcasecmp(x, "txt") ? RK_TXT : -1;
}

/* ================================================================ documento: el hilo de MuPDF */

typedef struct {
	RdReq r;
	u32 gen;      // de qué paginación es
	u16 *px;
	int w, h;
	u64 used;
	bool busy;    // (dibujándose)
} Slot;

static struct {
	Thread thr;
	LightEvent ev;
	LightLock lk;
	volatile bool quit;
	bool init;
	// lo pedido
	char want_path[256];
	bool want_open, want_close;
	float lw, lh, lem;
	int lkeep;
	u32 lgen;
	bool want_layout;
	RdReq want;
	bool has_want;
	RdReq pre[NPRE];
	int npre;
	char cov_want[256];
	int ctw, cth;
	// estado
	int st, pages;
	bool reflow;
	char err[96], title[96];
	u32 gen;              // cambia al abrir o recolocar: lo dibujado antes ya no vale
	u32 lgen_done;
	int relaid_page;
	Slot c[NSLOT];
	u64 tick;
	// portada
	char cov_path[256];
	u16 *cov_px;
	int cov_w, cov_h, cov_tw, cov_th;
	bool cov_fail;
} R;

static void lk(void) { LightLock_Lock(&R.lk); }
static void ulk(void) { LightLock_Unlock(&R.lk); }

static bool req_eq(const RdReq *a, const RdReq *b)
{
	return a->page == b->page && a->tw == b->tw && a->th == b->th && a->rot == b->rot && a->fit == b->fit;
}

// (con el cerrojo) la página dibujada que coincide, o NULL
static Slot *find_slot(const RdReq *r)
{
	for (int i = 0; i < NSLOT; i++)
		if (R.c[i].px && R.c[i].gen == R.gen && req_eq(&R.c[i].r, r))
			return &R.c[i];
	return NULL;
}

static void clear_slots(void)
{
	for (int i = 0; i < NSLOT; i++) {
		free(R.c[i].px);
		R.c[i].px = NULL;
	}
}

// ---- solo en el hilo
static fz_context *ctx;
static fz_document *doc;

static void to565(fz_pixmap *pix, u16 *out)
{
	int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix), n = fz_pixmap_components(ctx, pix);
	ptrdiff_t st = fz_pixmap_stride(ctx, pix);
	const unsigned char *s = fz_pixmap_samples(ctx, pix);
	for (int y = 0; y < h; y++) {
		const unsigned char *p = s + y * st;
		u16 *o = out + y * w;
		for (int x = 0; x < w; x++, p += n)
			o[x] = (u16)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
	}
}

// dibuja una página de d dentro de tw×th (o a lo ancho), girada; NULL si no se pudo
static u16 *render(fz_document *d, int page, int tw, int th, int rot, int fit, int *ow, int *oh)
{
	fz_page *pg = NULL;
	fz_pixmap *pix = NULL;
	u16 *out = NULL;
	fz_var(pg);
	fz_var(pix);
	fz_var(out);
	fz_try(ctx)
	{
		pg = fz_load_page(ctx, d, page);
		fz_rect b = fz_bound_page(ctx, pg);
		float pw = b.x1 - b.x0, ph = b.y1 - b.y0;
		if (pw < 1 || ph < 1)
			fz_throw(ctx, FZ_ERROR_GENERIC, "página vacía");
		if (rot == 90 || rot == 270) {
			float t = pw;
			pw = ph, ph = t;
		}
		float s = fit ? tw / pw : fminf(tw / pw, th / ph);
		if (pw * s > 1024)
			s = 1024 / pw;
		if (ph * s > 1024)
			s = 1024 / ph;
		fz_matrix m = fz_pre_rotate(fz_scale(s, s), (float)rot);
		pix = fz_new_pixmap_from_page(ctx, pg, m, fz_device_rgb(ctx), 0);
		int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix);
		out = malloc((size_t)w * h * 2);
		if (!out)
			fz_throw(ctx, FZ_ERROR_GENERIC, "sin memoria");
		to565(pix, out);
		*ow = w, *oh = h;
	}
	fz_always(ctx)
	{
		fz_drop_pixmap(ctx, pix);
		fz_drop_page(ctx, pg);
	}
	fz_catch(ctx)
	{
		util_log("lector: página %d: %s", page + 1, fz_caught_message(ctx));
		free(out);
		out = NULL;
	}
	return out;
}

static void do_close(void)
{
	if (doc) {
		fz_drop_document(ctx, doc);
		doc = NULL;
	}
	lk();
	clear_slots();
	R.gen++;
	R.st = RD_NONE, R.pages = 0, R.title[0] = 0;
	ulk();
	fz_empty_store(ctx);
}

static void do_open(const char *path)
{
	do_close();
	char sd[300];
	snprintf(sd, sizeof sd, "sdmc:%s", path);
	int pages = 0;
	bool reflow = false;
	char title[96] = "", err[96] = "";
	fz_try(ctx)
	{
		doc = fz_open_document(ctx, sd);
		if (fz_needs_password(ctx, doc) && !fz_authenticate_password(ctx, doc, ""))
			fz_throw(ctx, FZ_ERROR_GENERIC, "password");
		reflow = fz_is_document_reflowable(ctx, doc);
		if (reflow)
			fz_layout_document(ctx, doc, 300, 420, 11);
		pages = fz_count_pages(ctx, doc);
		if (fz_lookup_metadata(ctx, doc, FZ_META_INFO_TITLE, title, sizeof title) < 0)
			title[0] = 0;
	}
	fz_catch(ctx)
	{
		util_strlcpy(err, fz_caught_message(ctx), sizeof err);
		util_log("lector: no se pudo abrir %s: %s", path, err);
		if (doc)
			fz_drop_document(ctx, doc);
		doc = NULL;
	}
	lk();
	R.gen++;
	if (doc) {
		R.st = RD_OPEN, R.pages = pages, R.reflow = reflow;
		util_strlcpy(R.title, title, sizeof R.title);
	} else {
		R.st = RD_ERROR;
		util_strlcpy(R.err, !strcmp(err, "password") ? "password" : "open", sizeof R.err);
	}
	ulk();
	util_log("lector: %s abierto: %d páginas%s", path, pages, reflow ? " (se recoloca)" : "");
}

static void do_layout(float w, float h, float em, int keep, u32 lgen)
{
	int page = keep, pages = 0;
	fz_try(ctx)
	{
		fz_bookmark mark = 0;
		bool have = keep > 0;
		if (have)
			mark = fz_make_bookmark(ctx, doc, fz_location_from_page_number(ctx, doc, keep));
		fz_layout_document(ctx, doc, w, h, em);
		pages = fz_count_pages(ctx, doc);
		page = have ? fz_page_number_from_location(ctx, doc, fz_lookup_bookmark(ctx, doc, mark)) : 0;
	}
	fz_catch(ctx)
	{
		util_log("lector: recolocar: %s", fz_caught_message(ctx));
		pages = 0;
	}
	if (page < 0)
		page = 0;
	lk();
	clear_slots();
	R.gen++;
	if (pages > 0)
		R.pages = pages;
	if (page >= R.pages)
		page = R.pages - 1;
	R.relaid_page = page < 0 ? 0 : page;
	R.lgen_done = lgen;
	ulk();
}

static void do_render(const RdReq *r)
{
	lk();
	u32 gen = R.gen;
	bool have = find_slot(r) != NULL || !doc || r->page < 0 || r->page >= R.pages;
	ulk();
	if (have)
		return;
	int w = 0, h = 0;
	u16 *px = render(doc, r->page, r->tw, r->th, r->rot, r->fit, &w, &h);
	lk();
	if (px && gen == R.gen) {
		// a la plaza menos usada
		int k = 0;
		for (int i = 0; i < NSLOT; i++) {
			if (!R.c[i].px) {
				k = i;
				break;
			}
			if (R.c[i].used < R.c[k].used)
				k = i;
		}
		free(R.c[k].px);
		R.c[k].r = *r, R.c[k].gen = gen, R.c[k].px = px, R.c[k].w = w, R.c[k].h = h, R.c[k].used = ++R.tick;
		px = NULL;
	}
	ulk();
	free(px);
}

static void do_cover(const char *path, int tw, int th)
{
	char sd[300];
	snprintf(sd, sizeof sd, "sdmc:%s", path);
	fz_document *d = NULL;
	u16 *px = NULL;
	int w = 0, h = 0;
	fz_var(d);
	fz_try(ctx)
	{
		d = fz_open_document(ctx, sd);
		if (fz_needs_password(ctx, d))
			fz_throw(ctx, FZ_ERROR_GENERIC, "password");
		if (fz_is_document_reflowable(ctx, d))
			fz_layout_document(ctx, d, 300, 420, 11);
	}
	fz_catch(ctx)
	{
		if (d)
			fz_drop_document(ctx, d);
		d = NULL;
	}
	if (d) {
		px = render(d, 0, tw, th, 0, 0, &w, &h);
		fz_drop_document(ctx, d);
	}
	lk();
	if (!strcmp(R.cov_want, path)) {
		free(R.cov_px);
		util_strlcpy(R.cov_path, path, sizeof R.cov_path);
		R.cov_px = px, R.cov_w = w, R.cov_h = h, R.cov_tw = tw, R.cov_th = th, R.cov_fail = !px;
		R.cov_want[0] = 0;
		px = NULL;
	}
	ulk();
	free(px);
}

static void thread_main(void *arg)
{
	(void)arg;
	ctx = fz_new_context(NULL, NULL, STORE);
	if (!ctx) {
		util_log("lector: no se pudo crear el contexto de MuPDF");
		return;
	}
	fz_try(ctx)
	{
		fz_register_document_handlers(ctx);
		// en una pantalla tan pequeña, márgenes cortos y sin espacio de sobra entre párrafos
		fz_set_user_css(ctx, "@page{margin:10pt 9pt}body{margin:0}p{margin-top:0;margin-bottom:.45em}");
	}
	fz_catch(ctx)
	{
		util_log("lector: MuPDF: %s", fz_caught_message(ctx));
	}
	while (!R.quit) {
		LightEvent_WaitTimeout(&R.ev, 300 * 1000000ll);
		for (;;) {
			int op = 0;
			char path[256];
			RdReq r;
			float lw = 0, lh = 0, lem = 0;
			int keep = 0, ctw = 0, cth = 0;
			u32 lgen = 0;
			lk();
			if (R.want_close)
				op = 1, R.want_close = false;
			else if (R.want_open)
				op = 2, R.want_open = false, util_strlcpy(path, R.want_path, sizeof path);
			else if (R.want_layout && doc)
				op = 3, R.want_layout = false, lw = R.lw, lh = R.lh, lem = R.lem, keep = R.lkeep, lgen = R.lgen;
			else if (R.has_want && doc)
				op = 4, r = R.want, R.has_want = false;
			else if (R.cov_want[0])
				op = 5, util_strlcpy(path, R.cov_want, sizeof path), ctw = R.ctw, cth = R.cth;
			else if (R.npre && doc) {
				op = 4, r = R.pre[0];
				memmove(R.pre, R.pre + 1, sizeof R.pre[0] * --R.npre);
			}
			ulk();
			if (!op || R.quit)
				break;
			if (op == 1)
				do_close();
			else if (op == 2)
				do_open(path);
			else if (op == 3)
				do_layout(lw, lh, lem, keep, lgen);
			else if (op == 4)
				do_render(&r);
			else
				do_cover(path, ctw, cth);
		}
	}
	do_close();
	fz_drop_context(ctx);
	ctx = NULL;
}

static bool start(void)
{
	if (R.thr)
		return true;
	if (!R.init) {
		R.init = true;
		LightLock_Init(&R.lk);
		LightEvent_Init(&R.ev, RESET_ONESHOT);
	}
	R.quit = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	// en New 3DS, al núcleo 2 (dibujar una página pesa); si no, al de la app, por detrás de la interfaz
	R.thr = n3ds ? threadCreate(thread_main, NULL, 256 * 1024, prio + 1, 2, false) : NULL;
	if (!R.thr)
		R.thr = threadCreate(thread_main, NULL, 256 * 1024, prio + 1, -2, false);
	return R.thr != NULL;
}

bool rd_open(const char *path)
{
	if (!start())
		return false;
	lk();
	util_strlcpy(R.want_path, path, sizeof R.want_path);
	R.want_open = true, R.want_layout = R.has_want = false, R.npre = 0;
	R.st = RD_OPENING, R.err[0] = 0, R.pages = 0;
	ulk();
	LightEvent_Signal(&R.ev);
	return true;
}

void rd_close(void)
{
	if (!R.thr)
		return;
	lk();
	R.want_close = true, R.want_open = R.want_layout = R.has_want = false, R.npre = 0;
	R.st = RD_NONE;
	ulk();
	LightEvent_Signal(&R.ev);
}

int rd_state(int *pages, bool *reflow, char *err, size_t en)
{
	if (!R.init)
		return RD_NONE;
	lk();
	int st = R.st;
	if (pages)
		*pages = R.pages;
	if (reflow)
		*reflow = R.reflow;
	if (err)
		util_strlcpy(err, R.err, en);
	ulk();
	return st;
}

void rd_title(char *out, size_t n)
{
	out[0] = 0;
	if (!R.init)
		return;
	lk();
	util_strlcpy(out, R.title, n);
	ulk();
}

u32 rd_layout(float w, float h, float em, int keep)
{
	lk();
	R.lw = w, R.lh = h, R.lem = em, R.lkeep = keep;
	u32 g = ++R.lgen;
	R.want_layout = true, R.has_want = false, R.npre = 0;
	ulk();
	LightEvent_Signal(&R.ev);
	return g;
}

bool rd_relaid(u32 gen, int *page, int *pages)
{
	lk();
	bool done = R.lgen_done == gen;
	if (done) {
		*page = R.relaid_page;
		*pages = R.pages;
	}
	ulk();
	return done;
}

bool rd_get(const RdReq *r, u16 **px, int *w, int *h)
{
	if (!R.init)
		return false;
	bool ok = false;
	lk();
	Slot *s = find_slot(r);
	if (s) {
		*px = malloc((size_t)s->w * s->h * 2);
		if (*px) {
			memcpy(*px, s->px, (size_t)s->w * s->h * 2);
			*w = s->w, *h = s->h, s->used = ++R.tick;
			ok = true;
		}
	} else if (!R.want_layout) {
		R.want = *r, R.has_want = true;
	}
	ulk();
	if (!ok)
		LightEvent_Signal(&R.ev);
	return ok;
}

void rd_prefetch(const RdReq *r)
{
	if (!R.init)
		return;
	lk();
	bool have = find_slot(r) != NULL;
	for (int i = 0; i < R.npre && !have; i++)
		have = req_eq(&R.pre[i], r);
	if (!have) {
		if (R.npre == NPRE)
			memmove(R.pre, R.pre + 1, sizeof R.pre[0] * --R.npre);
		R.pre[R.npre++] = *r;
	}
	ulk();
	if (!have)
		LightEvent_Signal(&R.ev);
}

bool rd_cover(const char *path, int tw, int th, u16 **px, int *w, int *h)
{
	if (!start())
		return false;
	bool ok = false;
	lk();
	if (!strcmp(R.cov_path, path) && R.cov_tw == tw && R.cov_th == th) {
		if (R.cov_px && (*px = malloc((size_t)R.cov_w * R.cov_h * 2))) {
			memcpy(*px, R.cov_px, (size_t)R.cov_w * R.cov_h * 2);
			*w = R.cov_w, *h = R.cov_h;
			ok = true;
		}
		ulk();
		return ok; // (si falló, no se vuelve a intentar)
	}
	if (strcmp(R.cov_want, path)) {
		util_strlcpy(R.cov_want, path, sizeof R.cov_want);
		R.ctw = tw, R.cth = th;
	}
	ulk();
	LightEvent_Signal(&R.ev);
	return false;
}

void rd_exit(void)
{
	if (!R.thr)
		return;
	R.quit = true;
	LightEvent_Signal(&R.ev);
	threadJoin(R.thr, 5000000000ull);
	threadFree(R.thr);
	R.thr = NULL;
	lk();
	clear_slots();
	free(R.cov_px);
	R.cov_px = NULL;
	ulk();
}

/* ================================================================ biblioteca */

static struct {
	LightLock lock;
	Thread thr;
	volatile bool quit, busy, again;
	RBook *b;
	int n;
	u32 rev;
	int where;
	volatile int found;
	bool init;
} L;

// las carpetas de lectura: donde se buscan también los .txt (en el resto de la SD hay muchos que no son para leer)
static const char *const BOOK_DIRS[] = {"/Books", "/Libros", "/Manga", "/Mangas", "/Comics", "/Cómics", "/Documents", "/Documentos", "/Lector",
                                        "/Downloads", "/ebooks"};

static bool book_dir(const char *p)
{
	for (size_t i = 0; i < sizeof BOOK_DIRS / sizeof BOOK_DIRS[0]; i++) {
		size_t n = strlen(BOOK_DIRS[i]);
		if (!strncasecmp(p, BOOK_DIRS[i], n) && (p[n] == 0 || p[n] == '/'))
			return true;
	}
	return false;
}

static bool skip_dir(const char *path)
{
	static const char *const SKIP[] = {"/Nintendo 3DS", "/Nintendo DSiWare", "/luma", "/gm9", "/3ds", "/private", "/DCIM", "/cias"};
	for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
		if (!strcasecmp(path, SKIP[i]))
			return true;
	return false;
}

typedef struct {
	RBook *out;
	int n;
	const RBook *old;
	int on;
	int depth;
} Walk;

typedef struct {
	char (*names)[256];
	bool *dirs;
	u64 *sizes, *mtimes;
	int n, cap;
} Ents;

static bool ents_cb(const FsxEntry *e, void *u)
{
	Ents *l = u;
	if (e->name[0] == '.')
		return true;
	if (!e->dir && rd_kind(e->name) < 0)
		return true;
	if (l->n == l->cap) {
		int nc = l->cap ? l->cap * 2 : 64;
		void *a = realloc(l->names, nc * sizeof *l->names), *b = realloc(l->dirs, nc * sizeof *l->dirs), *c = realloc(l->sizes, nc * sizeof *l->sizes),
		     *d = realloc(l->mtimes, nc * sizeof *l->mtimes);
		if (a)
			l->names = a;
		if (b)
			l->dirs = b;
		if (c)
			l->sizes = c;
		if (d)
			l->mtimes = d;
		if (!a || !b || !c || !d)
			return false;
		l->cap = nc;
	}
	util_strlcpy(l->names[l->n], e->name, sizeof l->names[0]);
	l->dirs[l->n] = e->dir, l->sizes[l->n] = e->size, l->mtimes[l->n] = e->mtime;
	l->n++;
	return true;
}

static const RBook *find_old(const Walk *w, const char *path)
{
	for (int i = 0; i < w->on; i++)
		if (!strcmp(w->old[i].path, path))
			return &w->old[i];
	return NULL;
}

static void walk(Walk *w, const char *dir)
{
	if (L.quit || w->depth > 12 || w->n >= RLIB_MAX)
		return;
	Ents l = {0};
	fsx_list(dir, true, ents_cb, &l);
	for (int i = 0; i < l.n && !L.quit && w->n < RLIB_MAX; i++) {
		char p[256];
		snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", l.names[i]);
		if (l.dirs[i]) {
			if (!skip_dir(p)) {
				w->depth++;
				walk(w, p);
				w->depth--;
			}
			continue;
		}
		int k = rd_kind(p);
		if (k == RK_TXT && !book_dir(p))
			continue;
		RBook *b = &w->out[w->n];
		const RBook *o = find_old(w, p);
		if (o)
			*b = *o; // (con su progreso)
		else
			memset(b, 0, sizeof *b);
		util_strlcpy(b->path, p, sizeof b->path);
		b->kind = (u8)k, b->size = l.sizes[i], b->mtime = l.mtimes[i];
		w->n++;
		L.found = w->n;
	}
	free(l.names), free(l.dirs), free(l.sizes), free(l.mtimes);
}

static void save(void)
{
	cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
	cJSON_AddNumberToObject(j, "v", 1);
	cJSON_AddNumberToObject(j, "where", L.where);
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++) {
		const RBook *b = &L.b[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "p", b->path);
		if (b->title[0])
			cJSON_AddStringToObject(e, "t", b->title);
		cJSON_AddNumberToObject(e, "s", (double)b->size);
		cJSON_AddNumberToObject(e, "m", (double)b->mtime);
		if (b->last) {
			cJSON_AddNumberToObject(e, "pg", b->page);
			cJSON_AddNumberToObject(e, "n", b->pages);
			cJSON_AddNumberToObject(e, "l", (double)b->last);
		}
		cJSON_AddItemToArray(a, e);
	}
	LightLock_Unlock(&L.lock);
	cJSON_AddItemToObject(j, "books", a);
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!s)
		return;
	FILE *f = fopen(RL_FILE ".tmp", "wb");
	bool ok = f && fwrite(s, 1, strlen(s), f) == strlen(s);
	if (f)
		ok = fclose(f) == 0 && ok;
	free(s);
	if (ok) {
		remove(RL_FILE);
		rename(RL_FILE ".tmp", RL_FILE);
	}
}

static void load(void)
{
	FILE *f = fopen(RL_FILE, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = sz > 0 && sz < 4 * 1024 * 1024 ? malloc(sz + 1) : NULL;
	size_t n = buf ? fread(buf, 1, sz, f) : 0;
	fclose(f);
	if (!buf)
		return;
	buf[n] = 0;
	cJSON *j = cJSON_Parse(buf);
	free(buf);
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "books"), *w = cJSON_GetObjectItemCaseSensitive(j, "where"), *e;
	L.where = cJSON_IsNumber(w) ? w->valueint : RLS_ALL;
	int k = 0;
	cJSON_ArrayForEach(e, a)
	{
		if (k >= RLIB_MAX)
			break;
		const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "p");
		if (!cJSON_IsString(p) || rd_kind(p->valuestring) < 0)
			continue;
		RBook *b = &L.b[k++];
		memset(b, 0, sizeof *b);
		util_strlcpy(b->path, p->valuestring, sizeof b->path);
		b->kind = (u8)rd_kind(b->path);
		const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "t"), *s = cJSON_GetObjectItemCaseSensitive(e, "s"), *m = cJSON_GetObjectItemCaseSensitive(e, "m"),
		            *pg = cJSON_GetObjectItemCaseSensitive(e, "pg"), *np = cJSON_GetObjectItemCaseSensitive(e, "n"), *l = cJSON_GetObjectItemCaseSensitive(e, "l");
		if (cJSON_IsString(t))
			util_strlcpy(b->title, t->valuestring, sizeof b->title);
		b->size = cJSON_IsNumber(s) ? (u64)s->valuedouble : 0;
		b->mtime = cJSON_IsNumber(m) ? (u64)m->valuedouble : 0;
		b->page = cJSON_IsNumber(pg) ? pg->valueint : 0;
		b->pages = cJSON_IsNumber(np) ? np->valueint : 0;
		b->last = cJSON_IsNumber(l) ? (u64)l->valuedouble : 0;
	}
	L.n = k;
	cJSON_Delete(j);
}

static void scan_thread(void *arg)
{
	(void)arg;
	do {
		L.again = false;
		L.found = 0;
		RBook *out = malloc(sizeof(RBook) * RLIB_MAX), *old = malloc(sizeof(RBook) * RLIB_MAX);
		if (!out || !old) {
			free(out), free(old);
			break;
		}
		LightLock_Lock(&L.lock);
		int on = L.n;
		memcpy(old, L.b, sizeof(RBook) * on);
		LightLock_Unlock(&L.lock);
		Walk w = {out, 0, old, on, 0};
		if (L.where == RLS_BOOKS)
			for (size_t i = 0; i < sizeof BOOK_DIRS / sizeof BOOK_DIRS[0]; i++)
				walk(&w, BOOK_DIRS[i]);
		else
			walk(&w, "/");
		// lo leído que está fuera de donde se busca (abierto desde Archivos) se queda, si el archivo sigue ahí
		for (int i = 0; i < on && w.n < RLIB_MAX && !L.quit; i++) {
			if (!old[i].last)
				continue;
			bool in = false;
			for (int k = 0; k < w.n && !in; k++)
				in = !strcmp(out[k].path, old[i].path);
			bool d;
			u64 s;
			if (!in && fsx_stat(old[i].path, &d, &s) == 0 && !d)
				out[w.n++] = old[i];
		}
		if (!L.quit) {
			LightLock_Lock(&L.lock);
			// (lo que se leyó mientras se buscaba gana a la copia de antes)
			for (int k = 0; k < w.n; k++)
				for (int i = 0; i < L.n; i++)
					if (!strcmp(L.b[i].path, out[k].path) && L.b[i].last > out[k].last) {
						out[k].page = L.b[i].page, out[k].pages = L.b[i].pages, out[k].last = L.b[i].last;
						if (L.b[i].title[0])
							util_strlcpy(out[k].title, L.b[i].title, sizeof out[k].title);
					}
			memcpy(L.b, out, sizeof(RBook) * w.n);
			L.n = w.n;
			L.rev++;
			LightLock_Unlock(&L.lock);
			save();
			util_log("lector: %d documentos", w.n);
		}
		free(out), free(old);
	} while (L.again && !L.quit);
	L.busy = false;
	L.rev++;
}

static void scan_start(void)
{
	if (L.busy) {
		L.again = true;
		return;
	}
	if (L.thr) {
		threadJoin(L.thr, U64_MAX);
		threadFree(L.thr);
		L.thr = NULL;
	}
	L.busy = true;
	L.rev++;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	L.thr = threadCreate(scan_thread, NULL, 32 * 1024, prio + 2, -2, false);
	if (!L.thr)
		L.busy = false;
}

void rlib_init(void)
{
	if (L.init)
		return;
	LightLock_Init(&L.lock);
	L.b = malloc(sizeof(RBook) * RLIB_MAX);
	if (!L.b)
		return;
	L.init = true;
	load();
	scan_start();
}

void rlib_exit(void)
{
	L.quit = true;
	if (L.thr) {
		threadJoin(L.thr, 3000000000ull);
		threadFree(L.thr);
		L.thr = NULL;
	}
}

void rlib_scan(void)
{
	if (L.init)
		scan_start();
}

bool rlib_scanning(int *found)
{
	if (found)
		*found = L.found;
	return L.busy;
}

u32 rlib_rev(void)
{
	return L.rev;
}

int rlib_lock(const RBook **out)
{
	if (!L.init) {
		*out = NULL;
		return 0;
	}
	LightLock_Lock(&L.lock);
	*out = L.b;
	return L.n;
}

void rlib_unlock(void)
{
	if (L.init)
		LightLock_Unlock(&L.lock);
}

int rlib_where(void)
{
	return L.where;
}

void rlib_set_where(int w)
{
	L.where = w;
	rlib_scan();
}

void rlib_progress(const char *path, int page, int pages, const char *title)
{
	if (!L.init)
		rlib_init();
	if (!L.init || rd_kind(path) < 0)
		return;
	LightLock_Lock(&L.lock);
	int i = 0;
	while (i < L.n && strcmp(L.b[i].path, path))
		i++;
	if (i == L.n && L.n < RLIB_MAX) { // (abierto desde Archivos: entra en la biblioteca)
		RBook *b = &L.b[L.n++];
		memset(b, 0, sizeof *b);
		util_strlcpy(b->path, path, sizeof b->path);
		b->kind = (u8)rd_kind(path);
		bool d;
		fsx_stat(path, &d, &b->size);
	}
	if (i < L.n) {
		RBook *b = &L.b[i];
		b->page = page, b->pages = pages, b->last = util_unix_now();
		if (title && title[0])
			util_strlcpy(b->title, title, sizeof b->title);
	}
	L.rev++;
	LightLock_Unlock(&L.lock);
	save();
}

int rlib_last_page(const char *path, int *pages)
{
	if (!L.init)
		rlib_init();
	int p = -1;
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++)
		if (!strcmp(L.b[i].path, path) && L.b[i].last) {
			p = L.b[i].page;
			if (pages)
				*pages = L.b[i].pages;
		}
	LightLock_Unlock(&L.lock);
	return p;
}

void rlib_forget(const char *path)
{
	if (!L.init)
		return;
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++)
		if (!strcmp(L.b[i].path, path)) {
			memmove(&L.b[i], &L.b[i + 1], sizeof(RBook) * (L.n - i - 1));
			L.n--;
			break;
		}
	L.rev++;
	LightLock_Unlock(&L.lock);
	save();
}
