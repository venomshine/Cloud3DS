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
#else
#include <3ds.h>
#define FONT_R "romfs:/fonts/LiberationSans-Regular.ttf"
#define FONT_B "romfs:/fonts/LiberationSans-Bold.ttf"
#endif

#define CV_MAX 32
#define CV_MAX_PX (1600 * 1000) /* (más grande: no se pinta; 6,4 MB por lienzo) */
#define ST_MAX 32

typedef struct {
	float m[6];
	uint32_t fill, stroke; /* RGBA (r en el byte bajo, como nanosvg) */
	float alpha, lw, miter;
	int cap, join, comp;
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
	St st, stack[ST_MAX];
	int nst;
	Sub *sub;
	int nsub, csub;
	float ux, uy; /* el punto actual en coordenadas del usuario (para arcTo) */
	bool has_cur;
} Cv;

static Cv *CV[CV_MAX];
static int NEXT_ID = 1;
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
	memcpy(s->m, I, sizeof I);
	s->fill = 0xFF000000u, s->stroke = 0xFF000000u, s->alpha = 1, s->lw = 1, s->miter = 10, s->cap = 0, s->join = 0, s->comp = 0;
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
			free(CV[i]->sub), free(CV[i]->px), free(CV[i]);
			CV[i] = NULL;
		}
	ulk();
}
void cv_free_all(void)
{
	for (int i = 0; i < CV_MAX; i++)
		if (CV[i])
			cv_free(CV[i]->id);
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
	free(c->px);
	c->px = NULL;
	c->w = w, c->h = h;
	if ((long)w * h > 0 && (long)w * h <= CV_MAX_PX)
		c->px = calloc((size_t)w * h, 4);
	st_reset(&c->st);
	c->nst = 0;
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

/* ------------------------------------------------------------ estado */
void cv_save(int id)
{
	Cv *c = get(id);
	if (c && c->nst < ST_MAX)
		c->stack[c->nst++] = c->st;
}
void cv_restore(int id)
{
	Cv *c = get(id);
	if (c && c->nst > 0)
		c->st = c->stack[--c->nst];
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
	c->st.fill = fill, c->st.stroke = stroke, c->st.alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
	c->st.lw = line_w, c->st.cap = cap, c->st.join = join, c->st.miter = miter, c->st.comp = comp;
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

/* ------------------------------------------------------------ pintar: nanosvg en un búfer de su zona y encima */
static void blend_px(uint8_t *d, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa, int comp)
{
	if (!sa)
		return;
	if (comp == 1) { // destination-out: borra donde se pinta
		d[3] = (uint8_t)(d[3] * (255 - sa) / 255);
		return;
	}
	if (comp == 2) { // copy
		d[0] = (uint8_t)sr, d[1] = (uint8_t)sg, d[2] = (uint8_t)sb, d[3] = (uint8_t)sa;
		return;
	}
	uint32_t da = d[3];
	uint32_t oa = sa + da * (255 - sa) / 255;
	if (!oa)
		return;
	d[0] = (uint8_t)((sr * sa + d[0] * da * (255 - sa) / 255) / oa);
	d[1] = (uint8_t)((sg * sa + d[1] * da * (255 - sa) / 255) / oa);
	d[2] = (uint8_t)((sb * sa + d[2] * da * (255 - sa) / 255) / oa);
	d[3] = (uint8_t)oa;
}

static void raster(Cv *c, bool fill, bool evenodd)
{
	if (!c->px || !c->nsub)
		return;
	uint32_t col = fill ? c->st.fill : c->st.stroke;
	if (!(col >> 24) || c->st.alpha <= 0)
		return;
	float lw = fill ? 0 : c->st.lw * xscale(c);
	// sus caminos para nanosvg, y la zona que tocan
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
		for (int k = 0; k < s->n; k++) {
			float x = s->p[k * 2], y = s->p[k * 2 + 1];
			bx0 = fminf(bx0, x), by0 = fminf(by0, y), bx1 = fmaxf(bx1, x), by1 = fmaxf(by1, y);
		}
		p->bounds[0] = bx0, p->bounds[1] = by0, p->bounds[2] = bx1, p->bounds[3] = by1;
		*tail = p, tail = &p->next;
	}
	if (!paths)
		return;
	float pad = lw / 2 + 2 + (fill ? 0 : lw * c->st.miter / 2);
	int x0 = (int)floorf(bx0 - pad), y0 = (int)floorf(by0 - pad), x1 = (int)ceilf(bx1 + pad), y1 = (int)ceilf(by1 + pad);
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	int w = x1 - x0, h = y1 - y0;
	uint8_t *tmp = w > 0 && h > 0 ? malloc((size_t)w * h * 4) : NULL;
	NSVGrasterizer *r = tmp ? nsvgCreateRasterizer() : NULL;
	if (r) {
		NSVGshape sh;
		memset(&sh, 0, sizeof sh);
		sh.opacity = 1;
		sh.flags = NSVG_FLAGS_VISIBLE;
		sh.paths = paths;
		sh.fillRule = evenodd ? NSVG_FILLRULE_EVENODD : NSVG_FILLRULE_NONZERO;
		sh.paintOrder = (unsigned char)(fill ? (NSVG_PAINT_FILL | NSVG_PAINT_STROKE << 2 | NSVG_PAINT_MARKERS << 4) : (NSVG_PAINT_STROKE | NSVG_PAINT_FILL << 2 | NSVG_PAINT_MARKERS << 4));
		sh.fill.type = fill ? NSVG_PAINT_COLOR : NSVG_PAINT_NONE;
		sh.stroke.type = fill ? NSVG_PAINT_NONE : NSVG_PAINT_COLOR;
		sh.fill.color = sh.stroke.color = col | 0xFF000000u; // (la transparencia, al juntar)
		sh.strokeWidth = lw;
		sh.strokeLineCap = (char)c->st.cap, sh.strokeLineJoin = (char)c->st.join, sh.miterLimit = c->st.miter;
		sh.bounds[0] = bx0, sh.bounds[1] = by0, sh.bounds[2] = bx1, sh.bounds[3] = by1;
		NSVGimage im = {(float)c->w, (float)c->h, &sh};
		nsvgRasterize(r, &im, (float)-x0, (float)-y0, 1, tmp, w, h, w * 4);
		nsvgDeleteRasterizer(r);
		uint32_t ca = (uint32_t)((col >> 24) * c->st.alpha);
		uint32_t cr_ = col & 0xFF, cg = (col >> 8) & 0xFF, cb = (col >> 16) & 0xFF;
		lk();
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const uint8_t *s = tmp + ((size_t)y * w + x) * 4;
				if (s[3])
					blend_px(c->px + ((size_t)(y + y0) * c->w + (x + x0)) * 4, cr_, cg, cb, s[3] * ca / 255, c->st.comp);
			}
		c->dirty = true;
		ulk();
	}
	free(tmp);
	while (paths) {
		NSVGpath *n = paths->next;
		free(paths); // (sus puntos son los del camino del lienzo)
		paths = n;
	}
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

static bool axis_aligned(const Cv *c) { return fabsf(c->st.m[1]) < 1e-6f && fabsf(c->st.m[2]) < 1e-6f; }

void cv_fill_rect(int id, float x, float y, float w, float h)
{
	Cv *c = get(id);
	if (!c || !c->px)
		return;
	if (!axis_aligned(c)) { // girado: como camino
		Sub *keep = c->sub;
		int kn = c->nsub, kc = c->csub;
		c->sub = NULL, c->nsub = c->csub = 0;
		cv_rect(id, x, y, w, h);
		raster(c, true, false);
		path_clear(c), free(c->sub);
		c->sub = keep, c->nsub = kn, c->csub = kc;
		return;
	}
	float X0, Y0, X1, Y1;
	xf(c, x, y, &X0, &Y0), xf(c, x + w, y + h, &X1, &Y1);
	int x0 = (int)lroundf(fminf(X0, X1)), x1 = (int)lroundf(fmaxf(X0, X1)), y0 = (int)lroundf(fminf(Y0, Y1)), y1 = (int)lroundf(fmaxf(Y0, Y1));
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	uint32_t col = c->st.fill, ca = (uint32_t)((col >> 24) * c->st.alpha);
	lk();
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++)
			blend_px(c->px + ((size_t)yy * c->w + xx) * 4, col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, ca, c->st.comp);
	c->dirty = true;
	ulk();
}

void cv_clear_rect(int id, float x, float y, float w, float h)
{
	Cv *c = get(id);
	if (!c || !c->px)
		return;
	float X0, Y0, X1, Y1;
	xf(c, x, y, &X0, &Y0), xf(c, x + w, y + h, &X1, &Y1);
	int x0 = (int)floorf(fminf(X0, X1)), x1 = (int)ceilf(fmaxf(X0, X1)), y0 = (int)floorf(fminf(Y0, Y1)), y1 = (int)ceilf(fmaxf(Y0, Y1));
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > c->w ? c->w : x1, y1 = y1 > c->h ? c->h : y1;
	lk();
	for (int yy = y0; yy < y1; yy++)
		if (x1 > x0)
			memset(c->px + ((size_t)yy * c->w + x0) * 4, 0, (size_t)(x1 - x0) * 4);
	c->dirty = true;
	ulk();
}

// drawImage: de otra imagen (RGBA) a su sitio, con la transformación (sin giro: su caja) y la transparencia
void cv_draw_image(int id, const uint8_t *src, int iw, int ih, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
	Cv *c = get(id);
	if (!c || !c->px || !src || iw <= 0 || ih <= 0 || sw == 0 || sh == 0)
		return;
	float X0, Y0, X1, Y1;
	xf(c, dx, dy, &X0, &Y0), xf(c, dx + dw, dy + dh, &X1, &Y1);
	if (X1 < X0) {
		float t = X0;
		X0 = X1, X1 = t;
	}
	if (Y1 < Y0) {
		float t = Y0;
		Y0 = Y1, Y1 = t;
	}
	int x0 = (int)floorf(X0), y0 = (int)floorf(Y0), x1 = (int)ceilf(X1), y1 = (int)ceilf(Y1);
	float kx = sw / (X1 - X0 > 0 ? X1 - X0 : 1), ky = sh / (Y1 - Y0 > 0 ? Y1 - Y0 : 1);
	uint32_t ga = (uint32_t)(255 * c->st.alpha);
	lk();
	for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; y++) {
		int syy = (int)(sy + (y + 0.5f - Y0) * ky);
		if (syy < 0 || syy >= ih)
			continue;
		for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; x++) {
			int sxx = (int)(sx + (x + 0.5f - X0) * kx);
			if (sxx < 0 || sxx >= iw)
				continue;
			const uint8_t *s = src + ((size_t)syy * iw + sxx) * 4;
			blend_px(c->px + ((size_t)y * c->w + x) * 4, s[0], s[1], s[2], s[3] * ga / 255, c->st.comp);
		}
	}
	c->dirty = true;
	ulk();
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
float cv_text(int id, const char *s, float x, float y, float px, bool bold, bool draw)
{
	stbtt_fontinfo *f = font(bold);
	Cv *c = draw ? get(id) : NULL;
	if (!f) // (sin fuente: un ancho aproximado, y no se dibuja)
		return (float)strlen(s) * px * 0.52f;
	float k = stbtt_ScaleForMappingEmToPixels(f, px), w = 0;
	float sc = c ? xscale(c) : 1;
	float kd = k * sc; // (al dibujar, del tamaño en el lienzo)
	uint32_t col = c ? c->st.fill : 0, ca = c ? (uint32_t)((col >> 24) * c->st.alpha) : 0;
	int prev = 0;
	for (const char *p = s; *p;) {
		uint32_t cp = utf8_next(&p);
		int adv, lsb;
		stbtt_GetCodepointHMetrics(f, (int)cp, &adv, &lsb);
		if (prev)
			w += stbtt_GetCodepointKernAdvance(f, prev, (int)cp) * k;
		if (c && c->px && cp > ' ') {
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
						if (tx < 0 || tx >= c->w || !a)
							continue;
						blend_px(c->px + ((size_t)ty * c->w + tx) * 4, col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, a * ca / 255, c->st.comp);
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
