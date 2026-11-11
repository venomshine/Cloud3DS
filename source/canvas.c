/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "canvas.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb/stb_truetype.h"
#pragma GCC diagnostic pop
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifdef HOST_TEST
#define FONT_R getenv("CV_FONT")
#define FONT_B getenv("CV_FONT_B")
typedef int LightLock;
static void LightLock_Init(LightLock *l) { (void)l; }
static void LightLock_Lock(LightLock *l) { (void)l; }
static void LightLock_Unlock(LightLock *l) { (void)l; }
static bool room(size_t n) { (void)n; return true; }
#else
#include <3ds.h>
#include <malloc.h>
// ¿cabe sin dejar a la app sin memoria? (lo libre del montón, con 6 MB de margen para lo demás)
static bool room(size_t n)
{
	struct mallinfo mi = mallinfo();
	size_t hs = envGetHeapSize(), used = (size_t)mi.uordblks;
	return hs > used && hs - used > n + (6u << 20);
}
#define FONT_R "romfs:/fonts/LiberationSans-Regular.ttf"
#define FONT_B "romfs:/fonts/LiberationSans-Bold.ttf"
#endif

#define CV_MAX 32
#define CV_MAX_PX (1600 * 1000) /* (más grande: no se pinta; 6,4 MB por lienzo) */
#define ST_MAX 64
#define IMG_MAX 48
#define IMG_BUDGET (12u << 20) /* las imágenes guardadas para drawImage y los patrones */
#define CV_BUDGET (16u << 20)  /* los píxeles de todos los lienzos juntos (más grandes, en blanco) */

/* con qué se pinta: un color, un degradado (su tabla de 256 colores) o una imagen que se repite */
typedef struct {
	int type;          /* CV_P_* */
	uint32_t color;    /* RGBA (r en el byte bajo) */
	float g[6];        /* lineal x0 y0 x1 y1; radial x0 y0 r0 x1 y1 r1; cónico ángulo x y */
	uint32_t lut[256];
	int img, rep;      /* patrón: su imagen (cv_img_*) y repeat (bit 0: x, bit 1: y) */
} Paint;
/* una máscara de recorte (clip): de varios save a la vez, con su cuenta */
typedef struct {
	int rc;
	uint8_t a[];
} Mask;
typedef struct {
	float m[6];
	Paint pf, ps;
	float alpha, lw, miter;
	int cap, join, comp;
	float dash[8], dash_off;
	int ndash;
	bool smooth;
	uint32_t shadow;   /* color de la sombra (0: sin) */
	float sh_x, sh_y, sh_blur;
} St;
typedef struct {
	float *p; /* x0,y0, luego de 6 en 6 (cúbicas) */
	int n, cap;
	bool closed;
} Sub;
typedef struct {
	const void *key;
	int id, w, h;
	uint8_t *px;
	bool dirty;
	St st;
	Mask *clip, *cstack[ST_MAX];
	int ncs;
	Sub *sub;
	int nsub, csub;
	float ux, uy; /* el punto actual en coordenadas del usuario (para arcTo) */
	bool has_cur;
} Cv;

static Cv *CV[CV_MAX];
static int NEXT_ID = 1;
static size_t CV_BYTES; /* lo que ocupan los píxeles de los lienzos */
static LightLock LK;
static bool LK_OK;

static void lk(void)
{
	if (!LK_OK)
		LightLock_Init(&LK), LK_OK = true;
	LightLock_Lock(&LK);
}
static void ulk(void) { LightLock_Unlock(&LK); }

static Cv *get(int id)
{
	for (int i = 0; i < CV_MAX; i++)
		if (CV[i] && CV[i]->id == id)
			return CV[i];
	return NULL;
}

static void st_reset(St *s)
{
	static const float I[6] = {1, 0, 0, 1, 0, 0};
	memset(s, 0, sizeof *s);
	memcpy(s->m, I, sizeof I);
	s->pf.color = s->ps.color = 0xFF000000u, s->alpha = 1, s->lw = 1, s->miter = 10, s->smooth = true;
}

static void mask_drop(Mask *m)
{
	if (m && --m->rc <= 0)
		free(m);
}
static void clip_reset(Cv *c)
{
	mask_drop(c->clip);
	c->clip = NULL;
	while (c->ncs > 0)
		mask_drop(c->cstack[--c->ncs]);
}

int cv_of(const void *key)
{
	for (int i = 0; key && i < CV_MAX; i++)
		if (CV[i] && CV[i]->key == key)
			return CV[i]->id;
	return 0;
}

int cv_new(const void *key, int w, int h)
{
	int old = cv_of(key);
	if (old)
		return old; // (ya tenía: el mismo, tal cual)
	lk();
	int slot = -1;
	for (int i = 0; i < CV_MAX && slot < 0; i++)
		if (!CV[i])
			slot = i;
	Cv *c = slot >= 0 ? calloc(1, sizeof *c) : NULL;
	if (!c) {
		ulk();
		return 0;
	}
	c->id = NEXT_ID++, c->key = key;
	st_reset(&c->st);
	CV[slot] = c;
	ulk();
	cv_resize(c->id, w, h);
	return c->id;
}

static void path_clear(Cv *c)
{
	for (int i = 0; i < c->nsub; i++)
		free(c->sub[i].p);
	c->nsub = 0, c->has_cur = false;
}

void cv_free(int id)
{
	lk();
	for (int i = 0; i < CV_MAX; i++)
		if (CV[i] && CV[i]->id == id) {
			path_clear(CV[i]);
			clip_reset(CV[i]);
			if (CV[i]->px)
				CV_BYTES -= (size_t)CV[i]->w * CV[i]->h * 4;
			free(CV[i]->sub), free(CV[i]->px), free(CV[i]);
			CV[i] = NULL;
		}
	ulk();
}
static void img_free_all(void);
void cv_free_all(void)
{
	for (int i = 0; i < CV_MAX; i++)
		if (CV[i])
			cv_free(CV[i]->id);
	img_free_all();
}

bool cv_resize(int id, int w, int h)
{
	lk();
	Cv *c = get(id);
	if (!c) {
		ulk();
		return false;
	}
	w = w < 0 ? 0 : w, h = h < 0 ? 0 : h;
	if (c->px)
		CV_BYTES -= (size_t)c->w * c->h * 4;
	free(c->px);
	c->px = NULL;
	clip_reset(c); // (antes de cambiar el tamaño: las máscaras son del de antes)
	c->w = w, c->h = h;
	size_t sz = (size_t)w * h * 4;
	if ((long)w * h > 0 && (long)w * h <= CV_MAX_PX && CV_BYTES + sz <= CV_BUDGET && room(sz) && (c->px = calloc(sz, 1)))
		CV_BYTES += sz;
	st_reset(&c->st);
	path_clear(c);
	c->dirty = true;
	ulk();
	return c->px != NULL;
}

void cv_size(int id, int *w, int *h)
{
	Cv *c = get(id);
	*w = c ? c->w : 0, *h = c ? c->h : 0;
}

uint8_t *cv_take(int id, int *w, int *h, bool force)
{
	lk();
	Cv *c = get(id);
	uint8_t *o = NULL;
	if (c && c->px && (c->dirty || force)) {
		o = malloc((size_t)c->w * c->h * 4);
		if (o) {
			memcpy(o, c->px, (size_t)c->w * c->h * 4);
			*w = c->w, *h = c->h;
			c->dirty = false;
		}
	}
	ulk();
	return o;
}

bool cv_dirty_any(void)
{
	for (int i = 0; i < CV_MAX; i++)
		if (CV[i] && CV[i]->dirty && CV[i]->px)
			return true;
	return false;
}

/* ------------------------------------------------------------ estado (el resto lo guarda el JavaScript; aquí, el recorte) */
void cv_save(int id)
{
	Cv *c = get(id);
	if (!c)
		return;
	if (c->ncs == ST_MAX) { // (demasiados save sin restore: el más antiguo se pierde)
		mask_drop(c->cstack[0]);
		memmove(c->cstack, c->cstack + 1, sizeof c->cstack[0] * (ST_MAX - 1));
		c->ncs--;
	}
	if (c->clip)
		c->clip->rc++;
	c->cstack[c->ncs++] = c->clip;
}
void cv_restore(int id)
{
	Cv *c = get(id);
	if (!c || !c->ncs)
		return;
	mask_drop(c->clip);
	c->clip = c->cstack[--c->ncs];
}
void cv_set_transform(int id, const float m[6])
{
	Cv *c = get(id);
	if (c)
		memcpy(c->st.m, m, sizeof c->st.m);
}
void cv_get_transform(int id, float m[6])
{
	Cv *c = get(id);
	static const float I[6] = {1, 0, 0, 1, 0, 0};
	memcpy(m, c ? c->st.m : I, sizeof c->st.m);
}
void cv_style(int id, uint32_t fill, uint32_t stroke, float alpha, float line_w, int cap, int join, float miter, int comp)
{
	Cv *c = get(id);
	if (!c)
		return;
	c->st.pf.type = c->st.ps.type = CV_P_COLOR;
	c->st.pf.color = fill, c->st.ps.color = stroke, c->st.alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
	c->st.lw = line_w, c->st.cap = cap, c->st.join = join, c->st.miter = miter, c->st.comp = comp;
}
void cv_style2(int id, const float *dash, int ndash, float dash_off, bool smooth, uint32_t shadow, float sx, float sy, float blur)
{
	Cv *c = get(id);
	if (!c)
		return;
	c->st.ndash = ndash < 0 ? 0 : ndash > 8 ? 8 : ndash;
	for (int i = 0; i < c->st.ndash; i++)
		c->st.dash[i] = dash[i];
	c->st.dash_off = dash_off, c->st.smooth = smooth;
	c->st.shadow = shadow, c->st.sh_x = sx, c->st.sh_y = sy, c->st.sh_blur = blur;
}

// un degradado o un patrón para el relleno (stroke: false) o el trazo; stops: offset y color (RGBA), de dos en dos
void cv_paint(int id, bool stroke, int type, const float g[6], const float *stops, const uint32_t *cols, int nstops, int img, int rep)
{
	Cv *c = get(id);
	if (!c)
		return;
	Paint *p = stroke ? &c->st.ps : &c->st.pf;
	p->type = type;
	memcpy(p->g, g, sizeof p->g);
	p->img = img, p->rep = rep;
	if (type == CV_P_PATTERN || type == CV_P_COLOR)
		return;
	// la tabla: de 0 a 1, interpolando entre paradas (sin paradas: transparente)
	for (int i = 0; i < 256; i++) {
		float t = i / 255.0f;
		if (nstops <= 0) {
			p->lut[i] = 0;
			continue;
		}
		int k = 0;
		while (k < nstops && stops[k] <= t)
			k++;
		if (k == 0)
			p->lut[i] = cols[0];
		else if (k == nstops)
			p->lut[i] = cols[nstops - 1];
		else {
			float t0 = stops[k - 1], t1 = stops[k], f = t1 > t0 ? (t - t0) / (t1 - t0) : 1;
			uint32_t a = cols[k - 1], b = cols[k], o = 0;
			for (int s = 0; s < 32; s += 8) {
				float va = (float)((a >> s) & 0xFF), vb = (float)((b >> s) & 0xFF);
				o |= (uint32_t)(va + (vb - va) * f + 0.5f) << s;
			}
			p->lut[i] = o;
		}
	}
}

static void xf(const Cv *c, float x, float y, float *ox, float *oy)
{
	const float *m = c->st.m;
	*ox = m[0] * x + m[2] * y + m[4];
	*oy = m[1] * x + m[3] * y + m[5];
}
static float xscale(const Cv *c)
{
	const float *m = c->st.m;
	return sqrtf(fabsf(m[0] * m[3] - m[1] * m[2]));
}
// la inversa de la transformación (del lienzo al usuario)
static bool inv6(const float *m, float *o)
{
	float d = m[0] * m[3] - m[1] * m[2];
	if (fabsf(d) < 1e-12f)
		return false;
	float id = 1 / d;
	o[0] = m[3] * id, o[1] = -m[1] * id, o[2] = -m[2] * id, o[3] = m[0] * id;
	o[4] = -(o[0] * m[4] + o[2] * m[5]), o[5] = -(o[1] * m[4] + o[3] * m[5]);
	return true;
}

/* ------------------------------------------------------------ caminos (en coordenadas del lienzo, ya transformadas) */
static bool pts_add(Sub *s, int k)
{
	if (s->n * 2 + k > s->cap) {
		int nc = s->cap ? s->cap * 2 : 64;
		while (nc < s->n * 2 + k)
			nc *= 2;
		float *np = realloc(s->p, sizeof *np * (size_t)nc);
		if (!np)
			return false;
		s->p = np, s->cap = nc;
	}
	return true;
}
static Sub *cur_sub(Cv *c) { return c->nsub ? &c->sub[c->nsub - 1] : NULL; }

void cv_begin(int id)
{
	Cv *c = get(id);
	if (c)
		path_clear(c);
}

static void move_dev(Cv *c, float X, float Y)
{
	if (c->nsub == c->csub) {
		int nc = c->csub ? c->csub * 2 : 8;
		Sub *ns = realloc(c->sub, sizeof *ns * (size_t)nc);
		if (!ns)
			return;
		c->sub = ns, c->csub = nc;
	}
	Sub *s = &c->sub[c->nsub++];
	memset(s, 0, sizeof *s);
	if (pts_add(s, 2))
		s->p[0] = X, s->p[1] = Y, s->n = 1;
}
static void cubic_dev(Cv *c, float x1, float y1, float x2, float y2, float x, float y)
{
	Sub *s = cur_sub(c);
	if (!s || !pts_add(s, 6))
		return;
	float *p = s->p + s->n * 2;
	p[0] = x1, p[1] = y1, p[2] = x2, p[3] = y2, p[4] = x, p[5] = y;
	s->n += 3;
}
static void last_dev(Cv *c, float *x, float *y)
{
	Sub *s = cur_sub(c);
	*x = s ? s->p[(s->n - 1) * 2] : 0, *y = s ? s->p[(s->n - 1) * 2 + 1] : 0;
}

void cv_move(int id, float x, float y)
{
	Cv *c = get(id);
	if (!c)
		return;
	float X, Y;
	xf(c, x, y, &X, &Y);
	move_dev(c, X, Y);
	c->ux = x, c->uy = y, c->has_cur = true;
}
static void line_dev(Cv *c, float X, float Y)
{
	float x0, y0;
	last_dev(c, &x0, &y0);
	cubic_dev(c, x0 + (X - x0) / 3, y0 + (Y - y0) / 3, x0 + (X - x0) * 2 / 3, y0 + (Y - y0) * 2 / 3, X, Y);
}
void cv_line(int id, float x, float y)
{
	Cv *c = get(id);
	if (!c)
		return;
	if (!c->has_cur) {
		cv_move(id, x, y);
		return;
	}
	float X, Y;
	xf(c, x, y, &X, &Y);
	line_dev(c, X, Y);
	c->ux = x, c->uy = y;
}
void cv_bezier(int id, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	Cv *c = get(id);
	if (!c)
		return;
	if (!c->has_cur)
		cv_move(id, c1x, c1y);
	float a, b, d, e, X, Y;
	xf(c, c1x, c1y, &a, &b), xf(c, c2x, c2y, &d, &e), xf(c, x, y, &X, &Y);
	cubic_dev(c, a, b, d, e, X, Y);
	c->ux = x, c->uy = y;
}
void cv_quad(int id, float qx, float qy, float x, float y)
{
	Cv *c = get(id);
	if (!c)
		return;
	if (!c->has_cur)
		cv_move(id, qx, qy);
	float x0 = c->ux, y0 = c->uy;
	cv_bezier(id, x0 + (qx - x0) * 2 / 3, y0 + (qy - y0) * 2 / 3, x + (qx - x) * 2 / 3, y + (qy - y) * 2 / 3, x, y);
}

// un arco de elipse (como ctx.ellipse / ctx.arc): en trozos de 90° como cúbicas
void cv_ellipse(int id, float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw)
{
	Cv *c = get(id);
	if (!c || rx < 0 || ry < 0)
		return;
	const float TAU = 6.2831853f;
	float sweep = a1 - a0;
	if (!ccw) {
		if (sweep >= TAU)
			sweep = TAU;
		else {
			sweep = fmodf(sweep, TAU);
			if (sweep < 0)
				sweep += TAU;
		}
	} else {
		if (-sweep >= TAU)
			sweep = -TAU;
		else {
			sweep = fmodf(sweep, TAU);
			if (sweep > 0)
				sweep -= TAU;
		}
	}
	float cr = cosf(rot), sr = sinf(rot);
#define EP(a, ox, oy)                                                     \
	do {                                                                  \
		float ex_ = rx * cosf(a), ey_ = ry * sinf(a);                     \
		(ox) = cx + ex_ * cr - ey_ * sr, (oy) = cy + ex_ * sr + ey_ * cr; \
	} while (0)
	float sx, sy;
	EP(a0, sx, sy);
	if (c->has_cur)
		cv_line(id, sx, sy);
	else
		cv_move(id, sx, sy);
	int segs = (int)ceilf(fabsf(sweep) / (TAU / 4) - 1e-4f);
	if (segs < 1)
		segs = 1;
	float step = sweep / segs, k = 4.0f / 3 * tanf(step / 4);
	float a = a0;
	for (int i = 0; i < segs; i++) {
		float b = a + step;
		float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
		// en el espacio de la elipse sin girar, y luego girado y trasladado
		float p1x = rx * (ca - k * sa), p1y = ry * (sa + k * ca);
		float p2x = rx * (cb + k * sb), p2y = ry * (sb - k * cb);
		float ex = rx * cb, ey = ry * sb;
		float X1 = cx + p1x * cr - p1y * sr, Y1 = cy + p1x * sr + p1y * cr;
		float X2 = cx + p2x * cr - p2y * sr, Y2 = cy + p2x * sr + p2y * cr;
		float X3 = cx + ex * cr - ey * sr, Y3 = cy + ex * sr + ey * cr;
		cv_bezier(id, X1, Y1, X2, Y2, X3, Y3);
		a = b;
	}
#undef EP
}

// arcTo: la esquina redondeada entre (actual)->(x1,y1)->(x2,y2) con radio r
void cv_arc_to(int id, float x1, float y1, float x2, float y2, float r)
{
	Cv *c = get(id);
	if (!c)
		return;
	if (!c->has_cur) {
		cv_move(id, x1, y1);
		return;
	}
	float x0 = c->ux, y0 = c->uy;
	float v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
	float l1 = hypotf(v1x, v1y), l2 = hypotf(v2x, v2y);
	float cross = v1x * v2y - v1y * v2x;
	if (r <= 0 || l1 < 1e-6f || l2 < 1e-6f || fabsf(cross) < 1e-6f) {
		cv_line(id, x1, y1);
		return;
	}
	float cosA = (v1x * v2x + v1y * v2y) / (l1 * l2);
	float ang = acosf(fmaxf(-1, fminf(1, cosA)));
	float t = r / tanf(ang / 2);
	float t1x = x1 + v1x / l1 * t, t1y = y1 + v1y / l1 * t, t2x = x1 + v2x / l2 * t, t2y = y1 + v2y / l2 * t;
	// el centro: desde t1, perpendicular hacia dentro
	float nx = -v1y / l1, ny = v1x / l1;
	if (nx * v2x + ny * v2y < 0)
		nx = -nx, ny = -ny;
	float ccx = t1x + nx * r, ccy = t1y + ny * r;
	float a0 = atan2f(t1y - ccy, t1x - ccx), a1 = atan2f(t2y - ccy, t2x - ccx);
	cv_line(id, t1x, t1y);
	cv_ellipse(id, ccx, ccy, r, r, 0, a0, a1, cross > 0);
	(void)t2x, (void)t2y;
}

void cv_rect(int id, float x, float y, float w, float h)
{
	cv_move(id, x, y);
	cv_line(id, x + w, y);
	cv_line(id, x + w, y + h);
	cv_line(id, x, y + h);
	cv_close(id);
}

void cv_close(int id)
{
	Cv *c = get(id);
	Sub *s = c ? cur_sub(c) : NULL;
	if (!s)
		return;
	s->closed = true;
	// el punto actual vuelve al principio (en coordenadas del usuario: aproximado, sin la inversa)
	float X = s->p[0], Y = s->p[1];
	move_dev(c, X, Y);
}

/* ------------------------------------------------------------ imágenes (para drawImage y los patrones) */
typedef struct {
	char *url;      /* (NULL: una copia de un lienzo) */
	uint8_t *px;
	int w, h, ow, oh; /* decodificada (quizá reducida) y su tamaño de verdad */
	int handle;
	unsigned used;
} Img;
static Img IMG[IMG_MAX];
static size_t IMG_BYTES;
static unsigned IMG_T;
static int IMG_GEN;

static void img_free(Img *m)
{
	IMG_BYTES -= (size_t)m->w * m->h * 4;
	free(m->url), free(m->px);
	memset(m, 0, sizeof *m);
}
static void img_free_all(void)
{
	for (int i = 0; i < IMG_MAX; i++)
		if (IMG[i].px)
			img_free(&IMG[i]);
	IMG_BYTES = 0;
}
static Img *img_get(int handle)
{
	for (int i = 0; handle > 0 && i < IMG_MAX; i++)
		if (IMG[i].px && IMG[i].handle == handle) {
			IMG[i].used = ++IMG_T;
			return &IMG[i];
		}
	return NULL;
}
int cv_img_find(const char *url, int *ow, int *oh)
{
	for (int i = 0; url && i < IMG_MAX; i++)
		if (IMG[i].px && IMG[i].url && !strcmp(IMG[i].url, url)) {
			IMG[i].used = ++IMG_T;
			*ow = IMG[i].ow, *oh = IMG[i].oh;
			return IMG[i].handle;
		}
	return 0;
}
// se queda con px (malloc); las más antiguas se van si no cabe
int cv_img_add(const char *url, uint8_t *px, int w, int h, int ow, int oh)
{
	size_t sz = (size_t)w * h * 4;
	if (!px || w <= 0 || h <= 0 || sz > IMG_BUDGET || !room(0)) {
		free(px);
		return 0;
	}
	for (;;) {
		int lru = -1, empty = -1;
		for (int i = 0; i < IMG_MAX; i++) {
			if (!IMG[i].px) {
				if (empty < 0)
					empty = i;
			} else if (lru < 0 || IMG[i].used < IMG[lru].used)
				lru = i;
		}
		if (empty >= 0 && IMG_BYTES + sz <= IMG_BUDGET) {
			Img *m = &IMG[empty];
			m->url = url ? strdup(url) : NULL;
			m->px = px, m->w = w, m->h = h, m->ow = ow > 0 ? ow : w, m->oh = oh > 0 ? oh : h;
			m->handle = ++IMG_GEN, m->used = ++IMG_T;
			IMG_BYTES += sz;
			return m->handle;
		}
		if (lru < 0) {
			free(px);
			return 0;
		}
		img_free(&IMG[lru]);
	}
}
// una copia de un lienzo como imagen (createPattern con un canvas)
int cv_img_from_canvas(int id)
{
	int w = 0, h = 0;
	Cv *c = get(id);
	if (!c || !c->px)
		return 0;
	lk();
	w = c->w, h = c->h;
	uint8_t *p = malloc((size_t)w * h * 4);
	if (p)
		memcpy(p, c->px, (size_t)w * h * 4);
	ulk();
	return cv_img_add(NULL, p, w, h, w, h);
}

/* ------------------------------------------------------------ pintar */
// lo que se pinta en cada píxel: su color (ya resuelto el degradado o el patrón) y la transparencia global
typedef struct {
	const Paint *p;
	float inv[6];
	uint32_t col;
	unsigned ga;      /* globalAlpha * 255 */
	const Img *im;
	bool ok;
} Pen;

static void pen_make(const Cv *c, const Paint *p, Pen *e)
{
	memset(e, 0, sizeof *e);
	e->p = p, e->col = p->color, e->ga = (unsigned)(c->st.alpha * 255 + 0.5f);
	e->ok = e->ga > 0;
	if (p->type != CV_P_COLOR && !inv6(c->st.m, e->inv))
		e->ok = false;
	if (p->type == CV_P_PATTERN && !(e->im = img_get(p->img)))
		e->ok = false;
	if (p->type == CV_P_COLOR && !(p->color >> 24))
		e->ok = false;
}

static uint32_t samp(const Img *m, float x, float y, bool smooth)
{
	// x, y en píxeles de la imagen original; la decodificada puede ser más pequeña
	float kx = (float)m->w / m->ow, ky = (float)m->h / m->oh;
	x *= kx, y *= ky;
	if (!smooth) {
		int ix = (int)floorf(x), iy = (int)floorf(y);
		ix = ix < 0 ? 0 : ix >= m->w ? m->w - 1 : ix, iy = iy < 0 ? 0 : iy >= m->h ? m->h - 1 : iy;
		const uint8_t *s = m->px + ((size_t)iy * m->w + ix) * 4;
		return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
	}
	// bilineal
	x -= 0.5f, y -= 0.5f;
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - x0, fy = y - y0;
	int x1 = x0 + 1, y1 = y0 + 1;
	x0 = x0 < 0 ? 0 : x0 >= m->w ? m->w - 1 : x0, x1 = x1 < 0 ? 0 : x1 >= m->w ? m->w - 1 : x1;
	y0 = y0 < 0 ? 0 : y0 >= m->h ? m->h - 1 : y0, y1 = y1 < 0 ? 0 : y1 >= m->h ? m->h - 1 : y1;
	const uint8_t *a = m->px + ((size_t)y0 * m->w + x0) * 4, *b = m->px + ((size_t)y0 * m->w + x1) * 4;
	const uint8_t *cc = m->px + ((size_t)y1 * m->w + x0) * 4, *d = m->px + ((size_t)y1 * m->w + x1) * 4;
	float w00 = (1 - fx) * (1 - fy) * a[3], w01 = fx * (1 - fy) * b[3], w10 = (1 - fx) * fy * cc[3], w11 = fx * fy * d[3];
	float ws = w00 + w01 + w10 + w11;
	if (ws <= 0)
		return 0;
	uint32_t o = (uint32_t)(ws + 0.5f) << 24;
	for (int k = 0; k < 3; k++)
		o |= (uint32_t)((a[k] * w00 + b[k] * w01 + cc[k] * w10 + d[k] * w11) / ws + 0.5f) << (k * 8);
	return o;
}

// el color en el centro de un píxel del lienzo
static uint32_t pen_at(const Pen *e, int X, int Y)
{
	const Paint *p = e->p;
	if (p->type == CV_P_COLOR)
		return e->col;
	float px = X + 0.5f, py = Y + 0.5f;
	float ux = e->inv[0] * px + e->inv[2] * py + e->inv[4], uy = e->inv[1] * px + e->inv[3] * py + e->inv[5];
	const float *g = p->g;
	float t;
	if (p->type == CV_P_LINEAR) {
		float dx = g[2] - g[0], dy = g[3] - g[1], l2 = dx * dx + dy * dy;
		if (l2 < 1e-12f)
			return 0;
		t = ((ux - g[0]) * dx + (uy - g[1]) * dy) / l2;
	} else if (p->type == CV_P_RADIAL) { // dos círculos (como en el canvas): el mayor t con r(t) >= 0
		float cdx = g[3] - g[0], cdy = g[4] - g[1], dr = g[5] - g[2];
		float pdx = ux - g[0], pdy = uy - g[1];
		float a = cdx * cdx + cdy * cdy - dr * dr, b = pdx * cdx + pdy * cdy + g[2] * dr, c = pdx * pdx + pdy * pdy - g[2] * g[2];
		if (fabsf(a) < 1e-6f) {
			if (fabsf(b) < 1e-9f)
				return 0;
			t = c / (2 * b);
			if (g[2] + t * dr < 0)
				return 0;
		} else {
			float disc = b * b - a * c;
			if (disc < 0)
				return 0;
			float s = sqrtf(disc);
			t = (b + s) / a;
			if (g[2] + t * dr < 0) {
				t = (b - s) / a;
				if (g[2] + t * dr < 0)
					return 0;
			}
		}
	} else if (p->type == CV_P_CONIC) {
		float a = atan2f(uy - g[2], ux - g[1]) - g[0];
		t = a / 6.2831853f;
		t -= floorf(t);
	} else { // patrón
		const Img *m = e->im;
		float x = ux, y = uy;
		if (p->rep & 1)
			x -= floorf(x / m->ow) * m->ow;
		else if (x < 0 || x >= m->ow)
			return 0;
		if (p->rep & 2)
			y -= floorf(y / m->oh) * m->oh;
		else if (y < 0 || y >= m->oh)
			return 0;
		return samp(m, x, y, false);
	}
	int i = (int)(t * 255 + 0.5f);
	return p->lut[i < 0 ? 0 : i > 255 ? 255 : i];
}

// juntar un color (sin premultiplicar; a ya con cobertura) con el del lienzo según globalCompositeOperation
static void blend_px(uint8_t *d, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa, int comp)
{
	if (!comp) { // source-over (casi siempre)
		if (!sa)
			return;
		if (sa == 255) {
			d[0] = (uint8_t)sr, d[1] = (uint8_t)sg, d[2] = (uint8_t)sb, d[3] = 255;
			return;
		}
		uint32_t da = d[3], oa = sa + da * (255 - sa) / 255;
		if (!oa)
			return;
		d[0] = (uint8_t)((sr * sa + d[0] * da * (255 - sa) / 255) / oa);
		d[1] = (uint8_t)((sg * sa + d[1] * da * (255 - sa) / 255) / oa);
		d[2] = (uint8_t)((sb * sa + d[2] * da * (255 - sa) / 255) / oa);
		d[3] = (uint8_t)oa;
		return;
	}
	float Sa = sa / 255.0f, Da = d[3] / 255.0f, s[3] = {sr / 255.0f, sg / 255.0f, sb / 255.0f}, c[3] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
	float oa, o[3];
	switch (comp) {
	case 1: // destination-out
		d[3] = (uint8_t)(d[3] * (1 - Sa) + 0.5f);
		return;
	case 2: // copy (dentro de lo que se pinta)
		d[0] = (uint8_t)sr, d[1] = (uint8_t)sg, d[2] = (uint8_t)sb, d[3] = (uint8_t)sa;
		return;
	case 3: // destination-over
		oa = Da + Sa * (1 - Da);
		for (int k = 0; k < 3; k++)
			o[k] = oa > 0 ? (c[k] * Da + s[k] * Sa * (1 - Da)) / oa : 0;
		break;
	case 4: // source-atop
		oa = Da;
		for (int k = 0; k < 3; k++)
			o[k] = s[k] * Sa + c[k] * (1 - Sa);
		break;
	case 5: // lighter
		oa = fminf(1, Sa + Da);
		for (int k = 0; k < 3; k++)
			o[k] = oa > 0 ? fminf(1, (s[k] * Sa + c[k] * Da) / oa) : 0;
		break;
	case 6: // source-in
		oa = Sa * Da;
		for (int k = 0; k < 3; k++)
			o[k] = s[k];
		break;
	case 7: // destination-in
		d[3] = (uint8_t)(d[3] * Sa + 0.5f);
		return;
	case 8: // xor
		oa = Sa * (1 - Da) + Da * (1 - Sa);
		for (int k = 0; k < 3; k++)
			o[k] = oa > 0 ? (s[k] * Sa * (1 - Da) + c[k] * Da * (1 - Sa)) / oa : 0;
		break;
	case 9: // destination-atop
		oa = Sa;
		for (int k = 0; k < 3; k++)
			o[k] = c[k] * Da + s[k] * (1 - Da);
		break;
	default: { // los de mezcla (multiply, screen, darken, lighten, overlay…) y luego source-over
		float m[3];
		for (int k = 0; k < 3; k++) {
			float a = s[k], b = c[k], r;
			switch (comp) {
			case 10: r = a * b; break;                                     // multiply
			case 11: r = a + b - a * b; break;                             // screen
			case 12: r = fminf(a, b); break;                               // darken
			case 13: r = fmaxf(a, b); break;                               // lighten
			case 14: r = b <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b); break; // overlay
			case 15: r = a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b); break; // hard-light
			case 16: r = fabsf(a - b); break;                              // difference
			default: r = a;
			}
			m[k] = (1 - Da) * a + Da * r;
		}
		oa = Sa + Da * (1 - Sa);
		for (int k = 0; k < 3; k++)
			o[k] = oa > 0 ? (m[k] * Sa + c[k] * Da * (1 - Sa)) / oa : 0;
	}
	}
	for (int k = 0; k < 3; k++)
		d[k] = (uint8_t)(fminf(1, fmaxf(0, o[k])) * 255 + 0.5f);
	d[3] = (uint8_t)(fminf(1, fmaxf(0, oa)) * 255 + 0.5f);
}

// un píxel con cobertura cov (0-255): su color, la transparencia global, el recorte y la composición
static inline void plot(Cv *c, const Pen *e, int x, int y, unsigned cov)
{
	if (c->clip)
		cov = cov * c->clip->a[(size_t)y * c->w + x] / 255;
	if (!cov && c->st.comp != 6 && c->st.comp != 7 && c->st.comp != 2)
		return;
	uint32_t col = pen_at(e, x, y);
	unsigned a = (col >> 24) * cov / 255 * e->ga / 255;
	blend_px(c->px + ((size_t)y * c->w + x) * 4, col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, a, c->st.comp);
}

// una cobertura (w*h, en x0,y0) al lienzo, con su pintura
static void apply(Cv *c, const uint8_t *cov, int x0, int y0, int w, int h, const Pen *e)
{
	if (!e->ok)
		return;
	lk();
	for (int y = 0; y < h; y++) {
		int Y = y + y0;
		if (Y < 0 || Y >= c->h)
			continue;
		for (int x = 0; x < w; x++) {
			int X = x + x0;
			unsigned v = cov[(size_t)y * w + x];
			if (v && X >= 0 && X < c->w)
				plot(c, e, X, Y, v);
		}
	}
	c->dirty = true;
	ulk();
}

// desenfoque de caja (tres pasadas ~ gaussiano) de una cobertura, para las sombras
static void box_blur(uint8_t *a, int w, int h, int r)
{
	if (r < 1)
		return;
	uint8_t *t = malloc((size_t)(w > h ? w : h));
	if (!t)
		return;
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < h; y++) {
			uint8_t *row = a + (size_t)y * w;
			int s = 0;
			for (int x = -r; x <= r; x++)
				s += row[x < 0 ? 0 : x >= w ? w - 1 : x] * (x >= 0 && x < w);
			for (int x = 0; x < w; x++) {
				t[x] = (uint8_t)(s / (2 * r + 1));
				int xo = x - r, xi = x + r + 1;
				s += (xi < w ? row[xi] : 0) - (xo >= 0 ? row[xo] : 0);
			}
			memcpy(row, t, (size_t)w);
		}
		for (int x = 0; x < w; x++) {
			int s = 0;
			for (int y = 0; y <= r && y < h; y++)
				s += a[(size_t)y * w + x];
			for (int y = 0; y < h; y++) {
				t[y] = (uint8_t)(s / (2 * r + 1));
				int yo = y - r, yi = y + r + 1;
				s += (yi < h ? a[(size_t)yi * w + x] : 0) - (yo >= 0 ? a[(size_t)yo * w + x] : 0);
			}
			for (int y = 0; y < h; y++)
				a[(size_t)y * w + x] = t[y];
		}
	}
	free(t);
}

// la sombra de una cobertura (si hay): desplazada, desenfocada y de su color, debajo
static void shadow(Cv *c, const uint8_t *cov, int x0, int y0, int w, int h)
{
	St *s = &c->st;
	if (!(s->shadow >> 24) || (!s->sh_blur && !s->sh_x && !s->sh_y))
		return;
	int r = (int)(s->sh_blur / 2 + 0.5f), pad = r * 3;
	int W = w + 2 * pad, H = h + 2 * pad;
	if ((long)W * H > 4L << 20)
		return;
	uint8_t *b = calloc((size_t)W * H, 1);
	if (!b)
		return;
	for (int y = 0; y < h; y++)
		memcpy(b + (size_t)(y + pad) * W + pad, cov + (size_t)y * w, (size_t)w);
	box_blur(b, W, H, r);
	Paint p = {0};
	p.color = s->shadow;
	Pen e;
	pen_make(c, &p, &e);
	apply(c, b, x0 - pad + (int)lroundf(s->sh_x), y0 - pad + (int)lroundf(s->sh_y), W, H, &e);
	free(b);
}

// la cobertura del camino actual (relleno o trazo) con nanosvg: en *cov (malloc), su zona en x0..; false si nada
static bool cover_path(Cv *c, bool fill, bool evenodd, bool whole, uint8_t **cov, int *ox, int *oy, int *ow, int *oh)
{
	*cov = NULL;
	if (!c->px || !c->nsub)
		return false;
	float lw = fill ? 0 : c->st.lw * xscale(c);
	NSVGpath *paths = NULL, **tail = &paths;
	float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
	for (int i = 0; i < c->nsub; i++) {
		Sub *s = &c->sub[i];
		if (s->n < 4)
			continue;
		NSVGpath *p = calloc(1, sizeof *p);
		if (!p)
			break;
		p->pts = s->p, p->npts = s->n, p->closed = s->closed || fill;
		float px0 = 1e30f, py0 = 1e30f, px1 = -1e30f, py1 = -1e30f;
		for (int k = 0; k < s->n; k++) {
			float x = s->p[k * 2], y = s->p[k * 2 + 1];
			px0 = fminf(px0, x), py0 = fminf(py0, y), px1 = fmaxf(px1, x), py1 = fmaxf(py1, y);
		}
		p->bounds[0] = px0, p->bounds[1] = py0, p->bounds[2] = px1, p->bounds[3] = py1;
		bx0 = fminf(bx0, px0), by0 = fminf(by0, py0), bx1 = fmaxf(bx1, px1), by1 = fmaxf(by1, py1);
		*tail = p, tail = &p->next;
	}
	if (!paths)
		return false;
	float pad = lw / 2 + 2 + (fill ? 0 : lw * c->st.miter / 2);
	int x0 = (int)floorf(bx0 - pad), y0 = (int)floorf(by0 - pad), x1 = (int)ceilf(bx1 + pad), y1 = (int)ceilf(by1 + pad);
	if (whole)
		x0 = y0 = 0, x1 = c->w, y1 = c->h;
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	int w = x1 - x0, h = y1 - y0;
	uint8_t *tmp = w > 0 && h > 0 ? malloc((size_t)w * h * 4) : NULL;
	NSVGrasterizer *r = tmp ? nsvgCreateRasterizer() : NULL;
	bool ok = false;
	if (r) {
		NSVGshape sh;
		memset(&sh, 0, sizeof sh);
		sh.opacity = 1;
		sh.flags = NSVG_FLAGS_VISIBLE;
		sh.paths = paths;
		sh.fillRule = evenodd ? NSVG_FILLRULE_EVENODD : NSVG_FILLRULE_NONZERO;
		sh.paintOrder = (unsigned char)(NSVG_PAINT_FILL | NSVG_PAINT_STROKE << 2 | NSVG_PAINT_MARKERS << 4);
		sh.fill.type = fill ? NSVG_PAINT_COLOR : NSVG_PAINT_NONE;
		sh.stroke.type = fill ? NSVG_PAINT_NONE : NSVG_PAINT_COLOR;
		sh.fill.color = sh.stroke.color = 0xFFFFFFFFu;
		sh.strokeWidth = lw;
		sh.strokeLineCap = (char)c->st.cap, sh.strokeLineJoin = (char)c->st.join, sh.miterLimit = c->st.miter;
		if (!fill && c->st.ndash) {
			float k = xscale(c), sum = 0;
			for (int i = 0; i < c->st.ndash; i++)
				sh.strokeDashArray[i] = c->st.dash[i] * k, sum += c->st.dash[i];
			if (sum > 0)
				sh.strokeDashCount = (char)c->st.ndash, sh.strokeDashOffset = c->st.dash_off * k;
		}
		sh.bounds[0] = bx0, sh.bounds[1] = by0, sh.bounds[2] = bx1, sh.bounds[3] = by1;
		NSVGimage im = {(float)c->w, (float)c->h, &sh};
		nsvgRasterize(r, &im, (float)-x0, (float)-y0, 1, tmp, w, h, w * 4);
		nsvgDeleteRasterizer(r);
		// solo la cobertura (la transparencia), en su sitio
		for (size_t i = 0, n = (size_t)w * h; i < n; i++)
			tmp[i] = tmp[i * 4 + 3];
		uint8_t *sm = realloc(tmp, (size_t)w * h);
		*cov = sm ? sm : tmp, tmp = NULL;
		*ox = x0, *oy = y0, *ow = w, *oh = h;
		ok = true;
	}
	free(tmp);
	while (paths) {
		NSVGpath *n = paths->next;
		free(paths); // (sus puntos son los del camino del lienzo)
		paths = n;
	}
	return ok;
}

static void raster(Cv *c, bool fill, bool evenodd)
{
	Pen e;
	pen_make(c, fill ? &c->st.pf : &c->st.ps, &e);
	if (!e.ok && c->st.comp != 1)
		return;
	uint8_t *cov;
	int x0, y0, w, h;
	if (!cover_path(c, fill, evenodd, false, &cov, &x0, &y0, &w, &h))
		return;
	shadow(c, cov, x0, y0, w, h);
	if (c->st.comp == 1) // (destination-out: borra aunque el color sea transparente)
		e.ok = true, e.ga = e.ga ? e.ga : 255;
	apply(c, cov, x0, y0, w, h, &e);
	free(cov);
}

void cv_fill(int id, bool evenodd)
{
	Cv *c = get(id);
	if (c)
		raster(c, true, evenodd);
}
void cv_stroke(int id)
{
	Cv *c = get(id);
	if (c)
		raster(c, false, false);
}

// clip(): lo de dentro del camino actual, junto con el recorte que ya hubiera
void cv_clip(int id, bool evenodd)
{
	Cv *c = get(id);
	if (!c || !c->px)
		return;
	uint8_t *cov = NULL;
	int x0, y0, w, h;
	size_t n = (size_t)c->w * c->h;
	Mask *m = room(n) ? malloc(sizeof *m + n) : NULL;
	if (!m)
		return;
	m->rc = 1;
	memset(m->a, 0, n);
	if (cover_path(c, true, evenodd, true, &cov, &x0, &y0, &w, &h)) {
		for (int y = 0; y < h; y++)
			memcpy(m->a + (size_t)(y + y0) * c->w + x0, cov + (size_t)y * w, (size_t)w);
		free(cov);
	}
	if (c->clip)
		for (size_t i = 0; i < n; i++)
			m->a[i] = (uint8_t)(m->a[i] * c->clip->a[i] / 255);
	mask_drop(c->clip);
	c->clip = m;
}

// lo de los rectángulos: si no hay giro ni sombra, directo (sin nanosvg)
static bool axis_aligned(const Cv *c) { return fabsf(c->st.m[1]) < 1e-6f && fabsf(c->st.m[2]) < 1e-6f; }

void cv_fill_rect(int id, float x, float y, float w, float h)
{
	Cv *c = get(id);
	if (!c || !c->px)
		return;
	if (!axis_aligned(c) || ((c->st.shadow >> 24) && (c->st.sh_blur || c->st.sh_x || c->st.sh_y))) { // como camino
		Sub *keep = c->sub;
		int kn = c->nsub, kc = c->csub;
		bool khc = c->has_cur;
		float kux = c->ux, kuy = c->uy;
		c->sub = NULL, c->nsub = c->csub = 0;
		cv_rect(id, x, y, w, h);
		raster(c, true, false);
		path_clear(c), free(c->sub);
		c->sub = keep, c->nsub = kn, c->csub = kc, c->has_cur = khc, c->ux = kux, c->uy = kuy;
		return;
	}
	Pen e;
	pen_make(c, &c->st.pf, &e);
	if (!e.ok && c->st.comp != 1)
		return;
	if (c->st.comp == 1)
		e.ok = true, e.ga = e.ga ? e.ga : 255, e.col |= 0xFF000000u;
	float X0, Y0, X1, Y1;
	xf(c, x, y, &X0, &Y0), xf(c, x + w, y + h, &X1, &Y1);
	float fx0 = fminf(X0, X1), fx1 = fmaxf(X0, X1), fy0 = fminf(Y0, Y1), fy1 = fmaxf(Y0, Y1);
	int x0 = (int)floorf(fx0), x1 = (int)ceilf(fx1), y0 = (int)floorf(fy0), y1 = (int)ceilf(fy1);
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	lk();
	for (int yy = y0; yy < y1; yy++) {
		float cy = fminf(yy + 1, fy1) - fmaxf(yy, fy0); // (bordes a medio píxel: en parte)
		for (int xx = x0; xx < x1; xx++) {
			float cx = fminf(xx + 1, fx1) - fmaxf(xx, fx0);
			unsigned cov = (unsigned)(fmaxf(0, fminf(1, cx)) * fmaxf(0, fminf(1, cy)) * 255 + 0.5f);
			if (cov)
				plot(c, &e, xx, yy, cov);
		}
	}
	c->dirty = true;
	ulk();
}

void cv_clear_rect(int id, float x, float y, float w, float h)
{
	Cv *c = get(id);
	if (!c || !c->px)
		return;
	if (!axis_aligned(c)) { // girado: borrar con su camino
		int comp = c->st.comp;
		float a = c->st.alpha;
		Paint keep = c->st.pf;
		uint32_t ks = c->st.shadow;
		c->st.comp = 1, c->st.alpha = 1, c->st.pf.type = CV_P_COLOR, c->st.pf.color = 0xFF000000u, c->st.shadow = 0;
		cv_fill_rect(id, x, y, w, h);
		c->st.comp = comp, c->st.alpha = a, c->st.pf = keep, c->st.shadow = ks;
		return;
	}
	float X0, Y0, X1, Y1;
	xf(c, x, y, &X0, &Y0), xf(c, x + w, y + h, &X1, &Y1);
	int x0 = (int)floorf(fminf(X0, X1)), x1 = (int)ceilf(fmaxf(X0, X1)), y0 = (int)floorf(fminf(Y0, Y1)), y1 = (int)ceilf(fmaxf(Y0, Y1));
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	lk();
	for (int yy = y0; yy < y1; yy++)
		if (x1 > x0) {
			if (!c->clip)
				memset(c->px + ((size_t)yy * c->w + x0) * 4, 0, (size_t)(x1 - x0) * 4);
			else
				for (int xx = x0; xx < x1; xx++) {
					uint8_t *d = c->px + ((size_t)yy * c->w + xx) * 4;
					d[3] = (uint8_t)(d[3] * (255 - c->clip->a[(size_t)yy * c->w + xx]) / 255);
				}
		}
	c->dirty = true;
	ulk();
}

// drawImage: de una imagen (RGBA, de tamaño iw x ih, que «mide» ow x oh) a un rectángulo, con la transformación entera
static void draw_px(Cv *c, const Img *m, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	if (!c->px || !m || sw == 0 || sh == 0 || dw == 0 || dh == 0)
		return;
	float inv[6];
	if (!inv6(c->st.m, inv))
		return;
	// su caja en el lienzo
	float X[4], Y[4];
	xf(c, dx, dy, &X[0], &Y[0]), xf(c, dx + dw, dy, &X[1], &Y[1]), xf(c, dx, dy + dh, &X[2], &Y[2]), xf(c, dx + dw, dy + dh, &X[3], &Y[3]);
	float bx0 = fminf(fminf(X[0], X[1]), fminf(X[2], X[3])), bx1 = fmaxf(fmaxf(X[0], X[1]), fmaxf(X[2], X[3]));
	float by0 = fminf(fminf(Y[0], Y[1]), fminf(Y[2], Y[3])), by1 = fmaxf(fmaxf(Y[0], Y[1]), fmaxf(Y[2], Y[3]));
	int x0 = (int)floorf(bx0), x1 = (int)ceilf(bx1), y0 = (int)floorf(by0), y1 = (int)ceilf(by1);
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	Paint p = {0};
	p.color = 0xFFFFFFFFu;
	Pen e;
	pen_make(c, &p, &e);
	if (!e.ga)
		return;
	float kx = sw / dw, ky = sh / dh;
	bool smooth = c->st.smooth && (fabsf(kx) < 0.99f || fabsf(kx) > 1.01f || fabsf(ky) < 0.99f || fabsf(ky) > 1.01f || !axis_aligned(c));
	float sx0 = fminf(sx, sx + sw), sx1 = fmaxf(sx, sx + sw), sy0 = fminf(sy, sy + sh), sy1 = fmaxf(sy, sy + sh);
	lk();
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			float px = x + 0.5f, py = y + 0.5f;
			float ux = inv[0] * px + inv[2] * py + inv[4], uy = inv[1] * px + inv[3] * py + inv[5];
			float fx = (ux - dx) / dw, fy = (uy - dy) / dh;
			if (fx < 0 || fx >= 1 || fy < 0 || fy >= 1)
				continue;
			float ix = sx + fx * sw, iy = sy + fy * sh;
			if (ix < sx0 || ix >= sx1 || iy < sy0 || iy >= sy1 || ix < 0 || iy < 0 || ix >= m->ow || iy >= m->oh)
				continue;
			uint32_t col = samp(m, ix, iy, smooth);
			unsigned cov = 255;
			if (c->clip)
				cov = c->clip->a[(size_t)y * c->w + x];
			unsigned a = (col >> 24) * cov / 255 * e.ga / 255;
			blend_px(c->px + ((size_t)y * c->w + x) * 4, col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, a, c->st.comp);
		}
	c->dirty = true;
	ulk();
}

void cv_draw_img(int id, int img, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	Cv *c = get(id);
	Img *m = img_get(img);
	if (c && m)
		draw_px(c, m, sx, sy, sw, sh, dx, dy, dw, dh);
}
// de otro lienzo (o del mismo: se copia antes)
void cv_draw_canvas(int id, int src, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	Cv *c = get(id), *s = get(src);
	if (!c || !s || !s->px)
		return;
	Img m = {0};
	m.w = m.ow = s->w, m.h = m.oh = s->h, m.px = s->px;
	uint8_t *copy = NULL;
	if (s == c) {
		copy = malloc((size_t)s->w * s->h * 4);
		if (!copy)
			return;
		memcpy(copy, s->px, (size_t)s->w * s->h * 4);
		m.px = copy;
	}
	draw_px(c, &m, sx, sy, sw, sh, dx, dy, dw, dh);
	free(copy);
}

/* ------------------------------------------------------------ texto (Liberation Sans, como Arial) */
static stbtt_fontinfo FONT[2];
static unsigned char *FONT_DATA[2];
static bool FONT_TRIED[2];

static stbtt_fontinfo *font(bool bold)
{
	int k = bold ? 1 : 0;
	if (!FONT_TRIED[k]) {
		FONT_TRIED[k] = true;
		const char *path = k ? FONT_B : FONT_R;
		FILE *f = path ? fopen(path, "rb") : NULL;
		if (f) {
			fseek(f, 0, SEEK_END);
			long l = ftell(f);
			fseek(f, 0, SEEK_SET);
			FONT_DATA[k] = l > 0 ? malloc((size_t)l) : NULL;
			if (FONT_DATA[k] && fread(FONT_DATA[k], 1, (size_t)l, f) == (size_t)l &&
			    stbtt_InitFont(&FONT[k], FONT_DATA[k], stbtt_GetFontOffsetForIndex(FONT_DATA[k], 0)))
				;
			else
				free(FONT_DATA[k]), FONT_DATA[k] = NULL;
			fclose(f);
		}
		if (!FONT_DATA[k] && k) // (sin negrita: la normal)
			return font(false);
	}
	return FONT_DATA[k] ? &FONT[k] : (k ? font(false) : NULL);
}

static uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	if (c >= 0xF0 && p[0] && p[1] && p[2])
		c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u), p += 3;
	else if (c >= 0xE0 && p[0] && p[1])
		c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u), p += 2;
	else if (c >= 0xC0 && p[0])
		c = ((c & 31) << 6) | (p[0] & 63u), p += 1;
	*s = (const char *)p;
	return c;
}

void cv_text_metrics(float px, bool bold, float *ascent, float *descent)
{
	stbtt_fontinfo *f = font(bold);
	*ascent = px * 0.905f, *descent = px * 0.212f;
	if (!f)
		return;
	int a, d, g;
	stbtt_GetFontVMetrics(f, &a, &d, &g);
	float k = stbtt_ScaleForMappingEmToPixels(f, px);
	*ascent = a * k, *descent = -d * k;
}

// el texto con su línea base en (x, y) (en coordenadas del usuario); devuelve su ancho (sin transformar)
// el texto con su línea base en (x, y) (en coordenadas del usuario); devuelve su ancho (sin transformar)
float cv_text(int id, const char *s, float x, float y, float px, bool bold, bool draw)
{
	stbtt_fontinfo *f = font(bold);
	Cv *c = draw ? get(id) : NULL;
	if (!f) // (sin fuente: un ancho aproximado, y no se dibuja)
		return (float)strlen(s) * px * 0.52f;
	float k = stbtt_ScaleForMappingEmToPixels(f, px), w = 0;
	float sc = c ? xscale(c) : 1;
	float kd = k * sc; // (al dibujar, del tamaño en el lienzo)
	Pen e;
	if (c)
		pen_make(c, &c->st.pf, &e);
	if (c && (!e.ok || !c->px))
		c = NULL;
	int prev = 0;
	for (const char *p = s; *p;) {
		uint32_t cp = utf8_next(&p);
		int adv, lsb;
		stbtt_GetCodepointHMetrics(f, (int)cp, &adv, &lsb);
		if (prev)
			w += stbtt_GetCodepointKernAdvance(f, prev, (int)cp) * k;
		if (c && cp > ' ' && kd > 0.5f && kd < 400) {
			float X, Y;
			xf(c, x + w, y, &X, &Y);
			int gw, gh, gx, gy;
			unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(f, kd, kd, X - floorf(X), 0, (int)cp, &gw, &gh, &gx, &gy);
			if (bm) {
				int ox = (int)floorf(X) + gx, oy = (int)floorf(Y) + gy;
				lk();
				for (int yy = 0; yy < gh; yy++) {
					int ty = oy + yy;
					if (ty < 0 || ty >= c->h)
						continue;
					for (int xx = 0; xx < gw; xx++) {
						int tx = ox + xx;
						unsigned a = bm[yy * gw + xx];
						if (tx >= 0 && tx < c->w && a)
							plot(c, &e, tx, ty, a);
					}
				}
				c->dirty = true;
				ulk();
				stbtt_FreeBitmap(bm, NULL);
			}
		}
		w += adv * k;
		prev = (int)cp;
	}
	return w;
}

/* ------------------------------------------------------------ píxeles */
bool cv_get(int id, int x, int y, int w, int h, uint8_t *out)
{
	Cv *c = get(id);
	if (!c || w <= 0 || h <= 0)
		return false;
	memset(out, 0, (size_t)w * h * 4);
	if (!c->px)
		return true;
	lk();
	for (int yy = 0; yy < h; yy++) {
		int sy = y + yy;
		if (sy < 0 || sy >= c->h)
			continue;
		for (int xx = 0; xx < w; xx++) {
			int sx = x + xx;
			if (sx >= 0 && sx < c->w)
				memcpy(out + ((size_t)yy * w + xx) * 4, c->px + ((size_t)sy * c->w + sx) * 4, 4);
		}
	}
	ulk();
	return true;
}

void cv_put(int id, const uint8_t *in, int w, int h, int dx, int dy)
{
	Cv *c = get(id);
	if (!c || !c->px || !in)
		return;
	lk();
	for (int yy = 0; yy < h; yy++) {
		int ty = dy + yy;
		if (ty < 0 || ty >= c->h)
			continue;
		for (int xx = 0; xx < w; xx++) {
			int tx = dx + xx;
			if (tx >= 0 && tx < c->w)
				memcpy(c->px + ((size_t)ty * c->w + tx) * 4, in + ((size_t)yy * w + xx) * 4, 4);
		}
	}
	c->dirty = true;
	ulk();
}

// toDataURL: PNG (sin filtros, comprimido con zlib) en base64
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
char *cv_png_data_url(int id)
{
	Cv *c = get(id);
	int w = c ? c->w : 0, h = c ? c->h : 0;
	if (!c || !c->px || w <= 0 || h <= 0) {
		// (vacío: un PNG de 1x1 transparente, como da un navegador con un lienzo de tamaño 0)
		return strdup("data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=");
	}
	size_t raw_n = (size_t)(w * 4 + 1) * h;
	uint8_t *raw = malloc(raw_n);
	uLongf zn = compressBound((uLong)raw_n);
	uint8_t *z = malloc(zn);
	char *out = NULL;
	if (raw && z) {
		lk();
		for (int y = 0; y < h; y++) {
			raw[(size_t)y * (w * 4 + 1)] = 0;
			memcpy(raw + (size_t)y * (w * 4 + 1) + 1, c->px + (size_t)y * w * 4, (size_t)w * 4);
		}
		ulk();
		if (compress2(z, &zn, raw, (uLong)raw_n, 6) == Z_OK) {
			size_t pn = 8 + 25 + 12 + zn + 12;
			uint8_t *png = malloc(pn);
			if (png) {
				static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
				uint8_t *p = png;
				memcpy(p, SIG, 8), p += 8;
				// IHDR
				be32(p, 13), memcpy(p + 4, "IHDR", 4);
				be32(p + 8, (uint32_t)w), be32(p + 12, (uint32_t)h);
				p[16] = 8, p[17] = 6, p[18] = 0, p[19] = 0, p[20] = 0;
				be32(p + 21, (uint32_t)crc32(0, p + 4, 17)), p += 25;
				// IDAT
				be32(p, (uint32_t)zn), memcpy(p + 4, "IDAT", 4), memcpy(p + 8, z, zn);
				be32(p + 8 + zn, (uint32_t)crc32(0, p + 4, (uInt)(4 + zn))), p += 12 + zn;
				// IEND
				be32(p, 0), memcpy(p + 4, "IEND", 4), be32(p + 8, (uint32_t)crc32(0, p + 4, 4)), p += 12;
				size_t n = (size_t)(p - png);
				static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
				out = malloc(22 + (n + 2) / 3 * 4 + 1);
				if (out) {
					char *o = out + sprintf(out, "data:image/png;base64,");
					for (size_t i = 0; i < n; i += 3) {
						uint32_t v = (uint32_t)png[i] << 16 | (i + 1 < n ? (uint32_t)png[i + 1] << 8 : 0) | (i + 2 < n ? png[i + 2] : 0);
						*o++ = B[v >> 18 & 63], *o++ = B[v >> 12 & 63];
						*o++ = i + 1 < n ? B[v >> 6 & 63] : '=';
						*o++ = i + 2 < n ? B[v & 63] : '=';
					}
					*o = 0;
				}
				free(png);
			}
		}
	}
	free(raw), free(z);
	return out;
}
