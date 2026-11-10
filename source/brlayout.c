/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Maquetación por cajas (ver brlayout.h). Las medidas van en px de la página; cada caja guarda su
 * posición relativa a la caja de la que cuelga al colocarla («lp»), y al pintar se suman. */
#include "brlayout.h"
#include "core/node.h" /* libdom por dentro: hijos y hermanos sin referencias */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NI(n) ((dom_node_internal *)(n))
#define INF   1e30f

/* ------------------------------------------------------------ tipos */
enum { K_BLOCK, K_INLINE, K_TEXT, K_ATOM, K_BR };            /* clase de caja */
enum { I_FLOW, I_FLEX, I_GRID, I_TABLE, I_ROWG, I_ROW, I_CELL, I_CAPTION }; /* cómo coloca lo de dentro */
enum { O_FLOW, O_FLOAT, O_ABS };                              /* en el flujo, flotante o absoluta */
/* longitudes de cada elemento: px, % (bit en pct) o auto/none (bit en aut) */
enum { LW, LH, LMINW, LMAXW, LMINH, LMAXH, LBASIS, LMT, LMR, LMB, LML, LPT, LPR, LPB, LPL, LOT, LOR, LOB, LOL, LTIND, LCGAP, LRGAP, LN };
enum { F_TEXT, F_BOX, F_BG, F_OOF };                          /* trozos de una línea */

typedef struct Box Box;
typedef struct Sty Sty;

typedef struct {
	float x, y, w, h;
	Box *b;
	const char *s;
	uint16_t n;
	uint8_t t;
} LFrag;

typedef struct {
	uint32_t off;
	uint16_t n;
	uint8_t sp;   /* espacios delante */
	uint8_t tsp;  /* espacio detrás */
	uint8_t brk;  /* salto de línea forzado (pre) */
	uint8_t bk;   /* se puede partir delante aunque no haya espacio (chino, japonés…) */
	float w;
} Word;

struct Sty {
	Sty *all;          /* todas, para soltarlas */
	dom_node *node;
	float lv[LN];
	uint32_t pct, aut;
	float bw[4];
	uint32_t bc[4], bg, col;
	float fpx, lh, grow, shrink, bsh, bsv, vash, spw, iw, ih, wcalc; /* wcalc: los px de width: calc(% + px) */
	int32_t order;
	int16_t link, clk, img, wdg, colspan, lnum, scope; /* scope: el tema por ámbitos (-1 ninguno) */
	const char *env;             /* las variables de los style="" de él y sus antepasados («--a:x;») */
	int16_t gcs, gsp, gr, grs;   /* rejilla: columna (1…, 0 auto) y cuántas; fila y cuántas */
	int16_t tw[16];              /* rejilla: ancho de las pistas (px; -1 auto; -10·fr) */
	uint8_t outer, inner, li, pos, flt, clr, ws, ta, va, ovx, ovy, bbox, vis, fdir, fwrap, jc, ai, as, lst, lstin;
	uint8_t bold, under, gall, ncols, ifc, item, bcol, tag, upper, placed, bgsz, bgrep, bgpxp, bgpyp;
	const char *bgurl; /* background-image */
	float bgsw, bgpx, bgpy;
	float ar, tx, ty, rad; /* aspect-ratio, translate (px o %: txp/typ), border-radius (px; <0: %) */
	uint8_t txp, typ, fit, fixbg, bcen; /* bcen: centra (1) o arrima a la derecha (2) los bloques hijos (<center>) */ /* fixbg: fija y detrás de todo, más estrecha que la página */
	/* resultado */
	float um[4], up[4], cmt, cmb, minc, maxc, cy, sx, sy, layw, fbase, lbase, ax, ay;
	bool icv, laid;
	LFrag *fr;
	int nfr, cfr;
};
enum { T_OTHER, T_HTML, T_BODY, T_HR };

struct Box {
	Box *parent, *first, *last, *next;
	Sty *s;
	float x, y, w, h;
	uint8_t kind, oof, tsp;
	const char *t;
	uint32_t tn;
	Word *wd;
	int nwd;
};

typedef struct {
	float x0, y0, x1, y1;
	uint8_t right;
} FlR;
typedef struct {
	FlR *f;
	int n, c;
} BFC;

typedef struct Chunk {
	struct Chunk *nx;
	size_t used, cap;
	char d[];
} Chunk;

typedef struct {
	BrLay *L;
	Chunk *ar;
	Sty *all;
	float vw, vh, bottom, doch; /* doch: el alto de la página (al pintar) */
	bool stop;
	int nel;
	uint32_t page_bg;
	bool bg_html;
} Ctx;

typedef struct {
	float x0, y0, x1, y1;
} Clip;

/* ------------------------------------------------------------ memoria: todo de una vez */
static void *arena(Ctx *C, size_t n)
{
	n = (n + 7) & ~(size_t)7;
	if (!C->ar || C->ar->used + n > C->ar->cap) {
		size_t cap = n > 128 * 1024 ? n : 128 * 1024;
		Chunk *k = malloc(sizeof *k + cap);
		if (!k) {
			C->stop = true;
			return NULL;
		}
		k->nx = C->ar, k->used = 0, k->cap = cap;
		C->ar = k;
	}
	void *p = C->ar->d + C->ar->used;
	C->ar->used += n;
	memset(p, 0, n);
	return p;
}

static void arena_free(Ctx *C)
{
	for (Sty *s = C->all; s; s = s->all)
		free(s->fr);
	while (C->ar) {
		Chunk *k = C->ar->nx;
		free(C->ar);
		C->ar = k;
	}
}

/* ------------------------------------------------------------ medidas */
static float tw_(Ctx *C, const char *s, int n, const Sty *st)
{
	return n > 0 ? C->L->text_w(C->L->u, s, n, st->fpx, st->bold) : 0;
}
static float glyph_h(Ctx *C, const Sty *s)
{
	return C->L->line_h(C->L->u, s->fpx);
}
static float spw(Ctx *C, Sty *s)
{
	if (s->spw < 0)
		s->spw = tw_(C, " ", 1, s);
	return s->spw;
}

static bool AUT(const Sty *s, int i) { return (s->aut >> i) & 1; }
static bool PCT(const Sty *s, int i) { return (s->pct >> i) & 1; }
// una longitud en px (los % sobre base; auto, 0)
static float LV(const Sty *s, int i, float base)
{
	if (AUT(s, i))
		return 0;
	if (PCT(s, i))
		return (base > 0 && base < INF ? base * s->lv[i] / 100 : 0) + (i == LW ? s->wcalc : 0);
	return s->lv[i];
}
static float hx(const Sty *s) { return s->up[1] + s->up[3] + s->bw[1] + s->bw[3]; }
static float vy(const Sty *s) { return s->up[0] + s->up[2] + s->bw[0] + s->bw[2]; }

static void resolve(Sty *s, float cbw)
{
	for (int k = 0; k < 4; k++) {
		s->um[k] = LV(s, LMT + k, cbw);
		s->up[k] = fmaxf(0, LV(s, LPT + k, cbw));
	}
}

// el ancho pedido, como caja de borde (*has: si lo hay)
static float spec_w(const Sty *s, float cbw, bool *has)
{
	*has = !AUT(s, LW) && (!PCT(s, LW) || (cbw > 0 && cbw < INF));
	if (!*has)
		return 0;
	float w = LV(s, LW, cbw);
	return s->bbox ? fmaxf(w, hx(s)) : w + hx(s);
}
static float spec_h(const Sty *s, float cbh, bool *has)
{
	*has = !AUT(s, LH) && (!PCT(s, LH) || cbh >= 0);
	if (!*has)
		return 0;
	float h = LV(s, LH, cbh);
	return s->bbox ? fmaxf(h, vy(s)) : h + vy(s);
}
static float clamp_w(const Sty *s, float w, float cbw)
{
	bool okp = cbw > 0 && cbw < INF;
	if (!AUT(s, LMAXW) && (!PCT(s, LMAXW) || okp)) {
		float m = LV(s, LMAXW, cbw) + (s->bbox ? 0 : hx(s));
		if (w > m)
			w = m;
	}
	if (!AUT(s, LMINW) && (!PCT(s, LMINW) || okp)) {
		float m = LV(s, LMINW, cbw) + (s->bbox ? 0 : hx(s));
		if (w < m)
			w = m;
	}
	return fmaxf(w, hx(s));
}
static float clamp_h(const Sty *s, float h, float cbh)
{
	if (!AUT(s, LMAXH) && (!PCT(s, LMAXH) || cbh >= 0)) {
		float m = LV(s, LMAXH, cbh) + (s->bbox ? 0 : vy(s));
		if (h > m)
			h = m;
	}
	if (!AUT(s, LMINH) && (!PCT(s, LMINH) || cbh >= 0)) {
		float m = LV(s, LMINH, cbh) + (s->bbox ? 0 : vy(s));
		if (h < m)
			h = m;
	}
	return fmaxf(h, vy(s));
}

/* ------------------------------------------------------------ montar el árbol */
static void tag_of(dom_node *n, char *tag, size_t sz)
{
	tag[0] = 0;
	dom_string *nm = NULL;
	if (dom_node_get_node_name(n, &nm) != DOM_NO_ERR || !nm)
		return;
	size_t l = dom_string_byte_length(nm), i = 0;
	for (; i < l && i < sz - 1; i++)
		tag[i] = (char)tolower((unsigned char)dom_string_data(nm)[i]);
	tag[i] = 0;
	dom_string_unref(nm);
}

static bool attr(dom_node *el, const char *name, char *out, size_t n)
{
	dom_string *k = NULL, *v = NULL;
	out[0] = 0;
	if (dom_string_create((const uint8_t *)name, strlen(name), &k) != DOM_NO_ERR)
		return false;
	bool ok = dom_element_get_attribute((dom_element *)el, k, &v) == DOM_NO_ERR && v;
	if (ok) {
		size_t l = dom_string_byte_length(v);
		snprintf(out, n, "%.*s", (int)(l < n - 1 ? l : n - 1), dom_string_data(v));
		dom_string_unref(v);
	}
	dom_string_unref(k);
	return ok;
}

static uint32_t css_col(css_color c)
{
	return (c & 0xFF000000) | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static void setlen(Ctx *C, Sty *s, int i, const css_computed_style *cs, bool set, css_fixed v, css_unit u)
{
	if (!set) {
		s->aut |= 1u << i;
		return;
	}
	if (u == CSS_UNIT_PCT) {
		s->pct |= 1u << i;
		s->lv[i] = FIXTOFLT(v);
		return;
	}
	s->lv[i] = brcss_px(cs, v, u, C->vw);
}

// escondido «para lectores de pantalla» o de otras formas que un navegador no enseña
static bool invisible(Ctx *C, const css_computed_style *cs, bool replaced)
{
	css_fixed v;
	css_unit u;
	if (css_computed_position(cs) == CSS_POSITION_ABSOLUTE) {
		css_computed_clip_rect r;
		if (css_computed_clip(cs, &r) == CSS_CLIP_RECT)
			return true;
		if ((css_computed_left(cs, &v, &u) == CSS_LEFT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) < -400) ||
		    (css_computed_top(cs, &v, &u) == CSS_TOP_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) < -400) ||
		    (css_computed_right(cs, &v, &u) == CSS_RIGHT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) < -400))
			return true;
		if (css_computed_width(cs, &v, &u) == CSS_WIDTH_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) <= 2)
			return true;
		if (css_computed_height(cs, &v, &u) == CSS_HEIGHT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) <= 2)
			return true;
	}
	(void)replaced; // (font-size: 0 no esconde: sus hijos suelen volver a poner su tamaño; su texto no se pinta)
	css_fixed op;
	if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET && FIXTOFLT(op) < 0.05f)
		return true;
	if (css_computed_text_indent(cs, &v, &u) == CSS_TEXT_INDENT_SET && u != CSS_UNIT_PCT && brcss_px(cs, v, u, C->vw) < -500)
		return true;
	return false;
}

static Sty *sty_new(Ctx *C, const Sty *parent)
{
	Sty *s = arena(C, sizeof *s);
	if (!s)
		return NULL;
	s->all = C->all, C->all = s;
	s->link = parent ? parent->link : -1;
	s->clk = parent ? parent->clk : -1;
	s->scope = parent ? parent->scope : -1;
	s->env = parent ? parent->env : NULL;
	s->img = s->wdg = -1;
	s->colspan = 1;
	s->spw = -1;
	s->fbase = s->lbase = -1;
	return s;
}

static void sty_fill(Ctx *C, Sty *s, const css_computed_style *cs, uint8_t d)
{
	css_fixed v = 0, v2 = 0;
	css_unit u = CSS_UNIT_PX, u2 = CSS_UNIT_PX;
#define SL(i, call, setv) do { uint8_t t_ = call; setlen(C, s, i, cs, t_ == setv, v, u); } while (0)
	css_color c;
	SL(LW, css_computed_width(cs, &v, &u), CSS_WIDTH_SET);
	float cpx, cpc;
	if (brcss_width_calc(cs, C->vw, &cpx, &cpc)) {
		s->aut &= ~(1u << LW);
		s->pct |= 1u << LW;
		s->lv[LW] = cpc, s->wcalc = cpx;
	}
	SL(LH, css_computed_height(cs, &v, &u), CSS_HEIGHT_SET);
	SL(LMINW, css_computed_min_width(cs, &v, &u), CSS_MIN_WIDTH_SET);
	SL(LMAXW, css_computed_max_width(cs, &v, &u), CSS_MAX_WIDTH_SET);
	SL(LMINH, css_computed_min_height(cs, &v, &u), CSS_MIN_HEIGHT_SET);
	SL(LMAXH, css_computed_max_height(cs, &v, &u), CSS_MAX_HEIGHT_SET);
	SL(LBASIS, css_computed_flex_basis(cs, &v, &u), CSS_FLEX_BASIS_SET);
	SL(LMT, css_computed_margin_top(cs, &v, &u), CSS_MARGIN_SET);
	SL(LMR, css_computed_margin_right(cs, &v, &u), CSS_MARGIN_SET);
	SL(LMB, css_computed_margin_bottom(cs, &v, &u), CSS_MARGIN_SET);
	SL(LML, css_computed_margin_left(cs, &v, &u), CSS_MARGIN_SET);
	SL(LPT, css_computed_padding_top(cs, &v, &u), CSS_PADDING_SET);
	SL(LPR, css_computed_padding_right(cs, &v, &u), CSS_PADDING_SET);
	SL(LPB, css_computed_padding_bottom(cs, &v, &u), CSS_PADDING_SET);
	SL(LPL, css_computed_padding_left(cs, &v, &u), CSS_PADDING_SET);
	SL(LOT, css_computed_top(cs, &v, &u), CSS_TOP_SET);
	SL(LOR, css_computed_right(cs, &v, &u), CSS_RIGHT_SET);
	SL(LOB, css_computed_bottom(cs, &v, &u), CSS_BOTTOM_SET);
	SL(LOL, css_computed_left(cs, &v, &u), CSS_LEFT_SET);
	SL(LTIND, css_computed_text_indent(cs, &v, &u), CSS_TEXT_INDENT_SET);
	SL(LCGAP, css_computed_column_gap(cs, &v, &u), CSS_COLUMN_GAP_SET);
	SL(LRGAP, css_computed_row_gap(cs, &v, &u), CSS_ROW_GAP_SET);
	for (int k = LPT; k <= LPL; k++) // auto no vale en el relleno
		if (AUT(s, k))
			s->aut &= ~(1u << k), s->lv[k] = 0;
	// letra
	css_computed_font_size(cs, &v, &u);
	s->fpx = fmaxf(1, brcss_px(cs, v, u, C->vw));
	uint8_t lhk = css_computed_line_height(cs, &v, &u);
	if (lhk == CSS_LINE_HEIGHT_NUMBER)
		s->lh = FIXTOFLT(v) * s->fpx;
	else if (lhk == CSS_LINE_HEIGHT_DIMENSION)
		s->lh = u == CSS_UNIT_PCT ? FIXTOFLT(v) * s->fpx / 100 : brcss_px(cs, v, u, C->vw);
	else
		s->lh = s->fpx * 1.2f;
	if (s->lh < s->fpx * 0.5f)
		s->lh = s->fpx * 0.5f;
	uint8_t fw = css_computed_font_weight(cs);
	s->bold = fw == CSS_FONT_WEIGHT_BOLD || fw == CSS_FONT_WEIGHT_BOLDER || fw >= CSS_FONT_WEIGHT_600;
	s->under = (css_computed_text_decoration(cs) & CSS_TEXT_DECORATION_UNDERLINE) != 0;
	int scope = s->scope; // dentro de un tema («[data-color-mode=dark]»…): sus colores
	s->col = css_computed_color(cs, &c) == CSS_COLOR_COLOR ? css_col(brcss_scope_col(C->L->css, scope, cs, c, 2)) | 0xFF000000 : 0xFF222120;
	s->ws = css_computed_white_space(cs);
	uint8_t ta = css_computed_text_align(cs);
	s->ta = (ta == CSS_TEXT_ALIGN_CENTER || ta == CSS_TEXT_ALIGN_LIBCSS_CENTER) ? 1 : (ta == CSS_TEXT_ALIGN_RIGHT || ta == CSS_TEXT_ALIGN_LIBCSS_RIGHT) ? 2 : 0;
	s->bcen = ta == CSS_TEXT_ALIGN_LIBCSS_CENTER ? 1 : ta == CSS_TEXT_ALIGN_LIBCSS_RIGHT ? 2 : 0;
	s->va = css_computed_vertical_align(cs, &v, &u);
	if (s->va == CSS_VERTICAL_ALIGN_SET)
		s->vash = u == CSS_UNIT_PCT ? FIXTOFLT(v) * s->lh / 100 : brcss_px(cs, v, u, C->vw);
	s->vis = css_computed_visibility(cs) == CSS_VISIBILITY_VISIBLE;
	// caja
	s->pos = css_computed_position(cs);
	s->flt = css_computed_float(cs);
	s->clr = css_computed_clear(cs);
	if (s->clr == CSS_CLEAR_NONE)
		s->clr = 0;
	s->ovx = css_computed_overflow_x(cs);
	s->ovy = css_computed_overflow_y(cs);
	s->bbox = css_computed_box_sizing(cs) == CSS_BOX_SIZING_BORDER_BOX;
	if (css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR && (c = brcss_scope_col(C->L->css, scope, cs, c, 1)) >> 24 > 0x10)
		s->bg = css_col(c);
	{ // imagen de fondo, con su tamaño (de cssprep, en outline), posición y repetición
		lwc_string *bi = NULL;
		if (css_computed_background_image(cs, &bi) == CSS_BACKGROUND_IMAGE_IMAGE && bi && lwc_string_length(bi) > 4 && !C->L->light) {
			size_t l = lwc_string_length(bi);
			char *uc = arena(C, l + 1);
			if (uc) {
				memcpy(uc, lwc_string_data(bi), l);
				s->bgurl = uc;
			}
			uint8_t os = css_computed_outline_style(cs);
			s->bgsz = os == CSS_OUTLINE_STYLE_DASHED ? BRL_BG_COVER : os == CSS_OUTLINE_STYLE_DOTTED ? BRL_BG_CONTAIN : os == CSS_OUTLINE_STYLE_GROOVE ? BRL_BG_FILL
			          : os == CSS_OUTLINE_STYLE_DOUBLE ? BRL_BG_WIDTH : BRL_BG_AUTO;
			if (s->bgsz == BRL_BG_WIDTH && css_computed_outline_width(cs, &v, &u) == CSS_OUTLINE_WIDTH_WIDTH)
				s->bgsw = brcss_px(cs, v, u, C->vw);
			uint8_t r = css_computed_background_repeat(cs);
			s->bgrep = r == CSS_BACKGROUND_REPEAT_REPEAT ? 3 : r == CSS_BACKGROUND_REPEAT_REPEAT_X ? 1 : r == CSS_BACKGROUND_REPEAT_REPEAT_Y ? 2 : 0;
			if (css_computed_background_position(cs, &v, &u, &v2, &u2) == CSS_BACKGROUND_POSITION_SET) {
				s->bgpxp = u == CSS_UNIT_PCT, s->bgpyp = u2 == CSS_UNIT_PCT;
				s->bgpx = u == CSS_UNIT_PCT ? FIXTOFLT(v) : brcss_px(cs, v, u, C->vw);
				s->bgpy = u2 == CSS_UNIT_PCT ? FIXTOFLT(v2) : brcss_px(cs, v2, u2, C->vw);
			}
		}
	}
	uint8_t (*wf[4])(const css_computed_style *, css_fixed *, css_unit *) = {css_computed_border_top_width, css_computed_border_right_width,
	                                                                            css_computed_border_bottom_width, css_computed_border_left_width};
	uint8_t (*sf[4])(const css_computed_style *) = {css_computed_border_top_style, css_computed_border_right_style, css_computed_border_bottom_style,
	                                                css_computed_border_left_style};
	uint8_t (*cf[4])(const css_computed_style *, css_color *) = {css_computed_border_top_color, css_computed_border_right_color,
	                                                               css_computed_border_bottom_color, css_computed_border_left_color};
	for (int k = 0; k < 4; k++) {
		uint8_t bs = sf[k](cs);
		if (bs == CSS_BORDER_STYLE_NONE || bs == CSS_BORDER_STYLE_HIDDEN || bs == 0)
			continue;
		uint8_t wt = wf[k](cs, &v, &u);
		s->bw[k] = wt == CSS_BORDER_WIDTH_THIN ? 1 : wt == CSS_BORDER_WIDTH_THICK ? 5 : wt == CSS_BORDER_WIDTH_WIDTH ? fminf(fmaxf(0, brcss_px(cs, v, u, C->vw)), 40) : 3;
		css_color bcol;
		s->bc[k] = cf[k](cs, &bcol) == CSS_BORDER_COLOR_COLOR ? css_col(brcss_scope_col(C->L->css, scope, cs, bcol, 3)) : s->col;
	}
	// flex
	s->fdir = css_computed_flex_direction(cs);
	s->fwrap = css_computed_flex_wrap(cs) != CSS_FLEX_WRAP_NOWRAP;
	s->jc = css_computed_justify_content(cs);
	s->ai = css_computed_align_items(cs);
	s->as = css_computed_align_self(cs);
	css_fixed g = 0;
	s->grow = css_computed_flex_grow(cs, &g) == CSS_FLEX_GROW_SET ? FIXTOFLT(g) : 0;
	s->shrink = css_computed_flex_shrink(cs, &g) == CSS_FLEX_SHRINK_SET ? FIXTOFLT(g) : 1;
	css_computed_order(cs, &s->order);
	// tabla
	s->bcol = css_computed_border_collapse(cs) == CSS_BORDER_COLLAPSE_COLLAPSE;
	if (css_computed_border_spacing(cs, &v, &u, &v2, &u2) == CSS_BORDER_SPACING_SET)
		s->bsh = brcss_px(cs, v, u, C->vw), s->bsv = brcss_px(cs, v2, u2, C->vw);
	// lista
	uint8_t lt = css_computed_list_style_type(cs);
	s->lst = lt == CSS_LIST_STYLE_TYPE_NONE ? 0 : (lt == CSS_LIST_STYLE_TYPE_DISC || lt == CSS_LIST_STYLE_TYPE_CIRCLE || lt == CSS_LIST_STYLE_TYPE_SQUARE) ? 1
	         : (lt == CSS_LIST_STYLE_TYPE_LOWER_ALPHA || lt == CSS_LIST_STYLE_TYPE_LOWER_LATIN) ? 3
	         : (lt == CSS_LIST_STYLE_TYPE_UPPER_ALPHA || lt == CSS_LIST_STYLE_TYPE_UPPER_LATIN) ? 4
	         : (lt == CSS_LIST_STYLE_TYPE_LOWER_ROMAN) ? 5 : (lt == CSS_LIST_STYLE_TYPE_UPPER_ROMAN) ? 6 : 2;
	s->lstin = css_computed_list_style_position(cs) == CSS_LIST_STYLE_POSITION_INSIDE;
	// display
	switch (d) {
	case CSS_DISPLAY_INLINE: s->outer = 1, s->inner = I_FLOW; break;
	case CSS_DISPLAY_INLINE_BLOCK: s->outer = 1, s->inner = I_FLOW; break;
	case CSS_DISPLAY_LIST_ITEM: s->inner = I_FLOW, s->li = 1; break;
	case CSS_DISPLAY_TABLE: s->inner = I_TABLE; break;
	case CSS_DISPLAY_INLINE_TABLE: s->outer = 1, s->inner = I_TABLE; break;
	case CSS_DISPLAY_TABLE_ROW_GROUP:
	case CSS_DISPLAY_TABLE_HEADER_GROUP:
	case CSS_DISPLAY_TABLE_FOOTER_GROUP: s->inner = I_ROWG; break;
	case CSS_DISPLAY_TABLE_ROW: s->inner = I_ROW; break;
	case CSS_DISPLAY_TABLE_CELL: s->inner = I_CELL; break;
	case CSS_DISPLAY_TABLE_CAPTION: s->inner = I_CAPTION; break;
	case CSS_DISPLAY_FLEX: s->inner = I_FLEX; break;
	case CSS_DISPLAY_INLINE_FLEX: s->outer = 1, s->inner = I_FLEX; break;
	case CSS_DISPLAY_GRID: s->inner = I_GRID; break;
	case CSS_DISPLAY_INLINE_GRID: s->outer = 1, s->inner = I_GRID; break;
	default: s->inner = I_FLOW; break;
	}
	// rejilla (de cssprep: column-count = pistas; counter-reset g1 W1…; z-index = posición)
	if (s->inner == I_GRID) {
		int32_t k = 0;
		if (css_computed_column_count(cs, &k) == CSS_COLUMN_COUNT_SET && k > 0) {
			k = FIXTOINT(k);
			s->ncols = (uint8_t)(k < 1 ? 1 : k > 16 ? 16 : k);
		}
		for (int i = 0; i < 16; i++)
			s->tw[i] = -10;
		const css_computed_counter *ctr = NULL;
		if (css_computed_counter_reset(cs, &ctr) == CSS_COUNTER_RESET_NAMED && ctr)
			for (; ctr->name; ctr++) {
				const char *nm = lwc_string_data(ctr->name);
				if (nm[0] == 'g' && isdigit((unsigned char)nm[1])) {
					int i = atoi(nm + 1) - 1;
					if (i >= 0 && i < 16)
						s->tw[i] = (int16_t)FIXTOINT(ctr->value);
				}
			}
	}
	int32_t z = 0;
	if (css_computed_z_index(cs, &z) == CSS_Z_INDEX_SET) {
		if (z >= 100000 * 1024)
			z = FIXTOINT(z);
		if (z >= 100000 && z < 200000)
			s->gcs = (int16_t)((z - 100000) / 100), s->gsp = (int16_t)((z - 100000) % 100);
	}
	const css_computed_counter *ci = NULL;
	if (css_computed_counter_increment(cs, &ci) == CSS_COUNTER_INCREMENT_NAMED && ci)
		for (; ci->name; ci++) {
			const char *nm = lwc_string_data(ci->name);
			int val = FIXTOINT(ci->value);
			if (!strcmp(nm, "gr"))
				s->gr = (int16_t)val;
			else if (!strcmp(nm, "gq"))
				s->grs = (int16_t)val;
			else if (!strcmp(nm, "gc"))
				s->gcs = (int16_t)val;
			else if (!strcmp(nm, "gp"))
				s->gsp = (int16_t)val;
			else if (!strcmp(nm, "c3x") || !strcmp(nm, "c3xp"))
				s->tx = val / 10.0f, s->txp = nm[3] == 'p';
			else if (!strcmp(nm, "c3y") || !strcmp(nm, "c3yp"))
				s->ty = val / 10.0f, s->typ = nm[3] == 'p';
		}
	s->gall = css_computed_column_span(cs) == CSS_COLUMN_SPAN_ALL;
	// lo que cssprep pasa en propiedades que libcss sí guarda: aspect-ratio, object-fit, border-radius
	if (css_computed_column_width(cs, &v, &u) == CSS_COLUMN_WIDTH_SET) {
		float cw = brcss_px(cs, v, u, C->vw);
		if (cw >= 1000000)
			s->ar = (cw - 1000000) / 1000;
	}
	uint8_t crs = css_computed_column_rule_style(cs);
	s->fit = crs == CSS_COLUMN_RULE_STYLE_DOTTED ? 1 : crs == CSS_COLUMN_RULE_STYLE_SOLID ? 2 : 0;
	if (css_computed_column_rule_width(cs, &v, &u) == CSS_COLUMN_RULE_WIDTH_WIDTH) { // border-radius
		float r = brcss_px(cs, v, u, C->vw);
		s->rad = r >= 100000 ? -(r - 100000) : r;
	}
}

static Box *box_new(Ctx *C, Box *parent, uint8_t kind, Sty *s)
{
	Box *b = arena(C, sizeof *b);
	if (!b)
		return NULL;
	b->kind = kind, b->s = s, b->parent = parent;
	if (parent) {
		if (parent->last)
			parent->last->next = b;
		else
			parent->first = b;
		parent->last = b;
	}
	return b;
}

static bool blank(const char *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r' && s[i] != '\f')
			return false;
	return true;
}

static bool inline_level(const Box *b)
{
	if (b->oof)
		return false;
	return b->kind == K_INLINE || b->kind == K_TEXT || b->kind == K_BR || b->kind == K_ATOM || b->s->outer;
}

static void fixup(Ctx *C, Box *b);

// un trozo de cajas en línea, dentro de una caja de bloque anónima
static Box *anon_wrap(Ctx *C, Box *b, Box *first, Box *last)
{
	Sty *s = sty_new(C, b->s);
	if (!s)
		return NULL;
	Sty *p = b->s;
	s->node = p->node;
	s->upper = p->upper, s->fpx = p->fpx, s->lh = p->lh, s->col = p->col, s->ws = p->ws, s->ta = p->ta, s->bcen = p->bcen, s->bold = p->bold, s->under = p->under, s->vis = p->vis;
	s->link = p->link, s->clk = p->clk;
	s->aut = (1u << LW) | (1u << LH) | (1u << LMINW) | (1u << LMAXW) | (1u << LMINH) | (1u << LMAXH) | (1u << LBASIS) | (1u << LOT) | (1u << LOR) |
	         (1u << LOB) | (1u << LOL) | (1u << LTIND) | (1u << LCGAP) | (1u << LRGAP);
	s->inner = I_FLOW;
	s->shrink = 1;
	s->ifc = 1;
	s->as = CSS_ALIGN_SELF_AUTO;
	Box *a = arena(C, sizeof *a);
	if (!a)
		return NULL;
	a->kind = K_BLOCK, a->s = s, a->parent = b;
	a->first = first, a->last = last;
	for (Box *c = first;; c = c->next) {
		c->parent = a;
		if (c == last)
			break;
	}
	return a;
}

/* lo de dentro de una caja de bloque: o todo bloques o todo en línea (lo que no, en cajas anónimas);
 * en flex y rejilla, cada hijo es un elemento */
static void fixup(Ctx *C, Box *b)
{
	Sty *s = b->s;
	bool items = s->inner == I_FLEX || s->inner == I_GRID;
	bool tab = s->inner == I_TABLE || s->inner == I_ROWG || s->inner == I_ROW;
	bool any_inl = false, any_blk = false;
	for (Box *c = b->first; c; c = c->next) {
		if (c->oof)
			continue;
		if (c->kind == K_TEXT && blank(c->t, c->tn) && s->ws != CSS_WHITE_SPACE_PRE && s->ws != CSS_WHITE_SPACE_PRE_WRAP)
			continue;
		if (inline_level(c))
			any_inl = true;
		else
			any_blk = true;
	}
	if (items || tab) {
		// cada hijo, un elemento: el texto suelto en una caja anónima; los espacios fuera
		Box *prev = NULL, *c = b->first, *head = NULL, *tail = NULL;
		b->first = b->last = NULL;
		while (c) {
			Box *nx = c->next;
			c->next = NULL;
			if (c->kind == K_TEXT || c->kind == K_INLINE || c->kind == K_BR || (c->kind == K_ATOM && !items && !c->oof)) {
				// junta los seguidos en línea
				Box *run0 = c, *run1 = c;
				while (nx && !nx->oof && (nx->kind == K_TEXT || nx->kind == K_INLINE || nx->kind == K_BR || (nx->kind == K_ATOM && !items))) {
					run1->next = nx;
					run1 = nx;
					nx = nx->next;
					run1->next = NULL;
				}
				bool all_blank = true;
				for (Box *q = run0; q; q = q->next)
					if (!(q->kind == K_TEXT && blank(q->t, q->tn)))
						all_blank = false;
				if (all_blank) {
					c = nx;
					continue;
				}
				Box *a = anon_wrap(C, b, run0, run1);
				if (!a)
					break;
				c = a;
			}
			if (!head)
				head = c;
			else
				tail->next = c;
			tail = c;
			c->parent = b;
			if (c->kind == K_BLOCK && c->s->outer) { // en flex y rejilla, todo es bloque
				if (items)
					c->s->outer = 0;
			}
			if (items && c->kind == K_INLINE) {
				c->kind = K_BLOCK, c->s->outer = 0, c->s->inner = I_FLOW;
				fixup(C, c);
			}
			if (items)
				c->s->item = 1;
			prev = c;
			c = nx;
		}
		(void)prev;
		b->first = head, b->last = tail;
		s->ifc = 0;
		return;
	}
	if (!any_blk && any_inl) { // todo en línea
		s->ifc = 1;
		return;
	}
	if (!any_inl) { // todo bloques: fuera los espacios
		Box **pp = &b->first;
		b->last = NULL;
		while (*pp) {
			Box *c = *pp;
			if (c->kind == K_TEXT && !c->oof) {
				*pp = c->next;
				continue;
			}
			b->last = c;
			pp = &c->next;
		}
		s->ifc = 0;
		return;
	}
	// mezclado: los trozos en línea (con los flotantes y absolutas de en medio), en cajas anónimas
	Box *c = b->first, *head = NULL, *tail = NULL;
	while (c) {
		Box *nx = c->next;
		if (!c->oof && !inline_level(c)) {
			c->next = NULL;
			if (!head)
				head = c;
			else
				tail->next = c;
			tail = c;
			c = nx;
			continue;
		}
		if (c->oof && (!nx || (!nx->oof && !inline_level(nx)))) { // flotante entre bloques: va suelto
			c->next = NULL;
			if (!head)
				head = c;
			else
				tail->next = c;
			tail = c;
			c = nx;
			continue;
		}
		Box *r0 = c, *r1 = c;
		while (r1->next && (r1->next->oof || inline_level(r1->next)))
			r1 = r1->next;
		nx = r1->next;
		r1->next = NULL;
		bool all_blank = true;
		for (Box *q = r0; q; q = q->next)
			if (!(q->kind == K_TEXT && blank(q->t, q->tn)) && !q->oof)
				all_blank = false;
		if (all_blank) { // solo espacios: fuera (los flotantes y absolutas, sueltos)
			for (Box *q = r0; q;) {
				Box *qn = q->next;
				if (q->oof) {
					q->next = NULL;
					if (!head)
						head = q;
					else
						tail->next = q;
					tail = q;
				}
				q = qn;
			}
			c = nx;
			continue;
		}
		Box *a = anon_wrap(C, b, r0, r1);
		if (!a)
			break;
		if (!head)
			head = a;
		else
			tail->next = a;
		tail = a;
		c = nx;
	}
	b->first = head, b->last = tail;
	s->ifc = 0;
}

static const char *const NEVER[] = {"script", "style", "noscript", "template", "iframe", "object", "embed", "canvas", "video", "audio", "head",
                                    "title", "meta", "link", "base", NULL};

static bool in_list(const char *t, const char *const *l)
{
	for (; *l; l++)
		if (!strcmp(t, *l))
			return true;
	return false;
}

static void build_kids(Ctx *C, Box *b, dom_node *n, const css_computed_style *cs, int *olc);

#define AUTO_ALL                                                                                                                                     \
	((1u << LW) | (1u << LH) | (1u << LMINW) | (1u << LMAXW) | (1u << LMINH) | (1u << LMAXH) | (1u << LBASIS) | (1u << LOT) | (1u << LOR) | (1u << LOB) | \
	 (1u << LOL) | (1u << LTIND) | (1u << LCGAP) | (1u << LRGAP))

// el texto de ::before o ::after: como un trozo de texto más
static void pseudo_text(Ctx *C, Box *b, const char *txt)
{
	size_t l = strlen(txt);
	Box *t = box_new(C, b, K_TEXT, b->s);
	char *p = t ? arena(C, l + 1) : NULL;
	if (p) {
		memcpy(p, txt, l);
		if (b->s->upper)
			for (size_t i = 0; i < l; i++)
				p[i] = (char)toupper((unsigned char)p[i]);
		t->t = p, t->tn = (uint32_t)l;
	}
}

static void ps_free(BrcssPseudo *ps)
{
	for (int k = 0; k < 2; k++)
		if (ps->st[k])
			css_computed_style_destroy(ps->st[k]), ps->st[k] = NULL;
}

/* ::before / ::after: una caja con su estilo (fondo, borde, tamaño, posición…) y su texto */
static void pseudo_box(Ctx *C, Box *b, const char *txt, const css_computed_style *st, bool icon)
{
	if (!st) {
		if (txt[0])
			pseudo_text(C, b, txt);
		return;
	}
	uint8_t d = css_computed_display(st, false);
	if (d == CSS_DISPLAY_NONE || css_computed_position(st) == CSS_POSITION_FIXED || invisible(C, st, false))
		return;
	Sty *s = sty_new(C, b->s);
	if (!s)
		return;
	s->node = b->s->node;
	sty_fill(C, s, st, d);
	bool shows = txt[0] || s->bg || s->bgurl || icon;
	for (int k = 0; k < 4; k++)
		shows = shows || (s->bw[k] > 0 && (s->bc[k] >> 24) > 0x10);
	if (!shows) // nada que pintar: ni caja (no empuja nada, como casi siempre en Chrome)
		return;
	if (css_computed_text_transform(st) == CSS_TEXT_TRANSFORM_UPPERCASE)
		s->upper = 1;
	if (s->flt == CSS_FLOAT_LEFT || s->flt == CSS_FLOAT_RIGHT) {
		if (s->pos != CSS_POSITION_ABSOLUTE)
			s->outer = 0;
		else
			s->flt = 0;
	} else
		s->flt = 0;
	uint8_t oof = s->pos == CSS_POSITION_ABSOLUTE ? O_ABS : s->flt ? O_FLOAT : O_FLOW;
	if (oof)
		s->outer = 0;
	bool inl = s->outer && s->inner == I_FLOW && d == CSS_DISPLAY_INLINE && !oof;
	Box *p = box_new(C, b, inl ? K_INLINE : K_BLOCK, s);
	if (!p)
		return;
	p->oof = oof;
	if (icon && !txt[0]) { // el icono (sin su fuente): su sitio, un cuadro de 1em
		Sty *is = sty_new(C, s);
		if (is) {
			is->node = s->node;
			is->col = s->col, is->fpx = s->fpx, is->lh = s->fpx, is->vis = s->vis, is->scope = s->scope;
			is->aut = AUTO_ALL & ~(1u << LW | 1u << LH);
			is->lv[LW] = is->lv[LH] = s->fpx;
			is->outer = 1, is->inner = I_FLOW, is->shrink = 1;
			box_new(C, p, K_BLOCK, is);
		}
	}
	if (txt[0])
		pseudo_text(C, p, txt);
	if (p->kind == K_BLOCK)
		fixup(C, p);
}

static void build_text(Ctx *C, Box *parent, dom_node_internal *c)
{
	dom_string *d = NULL;
	if (dom_characterdata_get_data((dom_characterdata *)c, &d) != DOM_NO_ERR || !d)
		return;
	size_t n = dom_string_byte_length(d);
	if (n) {
		Box *t = box_new(C, parent, K_TEXT, parent->s);
		char *p = t ? arena(C, n + 1) : NULL;
		if (p) {
			memcpy(p, dom_string_data(d), n);
			if (parent->s->upper) // text-transform: uppercase
				for (size_t i = 0; i < n; i++)
					p[i] = (char)toupper((unsigned char)p[i]);
			if (parent->s->ws == CSS_WHITE_SPACE_PRE || parent->s->ws == CSS_WHITE_SPACE_PRE_WRAP)
				for (size_t i = 0; i < n; i++)
					if (p[i] == '\t')
						p[i] = ' ';
			t->t = p, t->tn = (uint32_t)n;
		}
	}
	dom_string_unref(d);
}

// la dirección de una imagen como la elegiría un navegador (y las «perezosas», que la guardan en data-*):
// src si es de verdad; si no, data-src y compañía, o la primera de srcset; y si solo hay un data:, ese
static bool src_first(const char *set, char *out, size_t n)
{
	while (*set == ' ')
		set++;
	size_t l = strcspn(set, " ,");
	if (!l || l >= n)
		return false;
	memcpy(out, set, l);
	out[l] = 0;
	return true;
}
static bool img_src(dom_node *n, char *a, size_t an)
{
	static const char *const LAZY[] = {"data-src", "data-lazy-src", "data-original", "data-url", "data-hi-res-src", NULL};
	char v[1024];
	if (attr(n, "src", a, an) && a[0] && strncmp(a, "data:", 5))
		return true;
	for (int i = 0; LAZY[i]; i++)
		if (attr(n, LAZY[i], v, sizeof v) && v[0] && strncmp(v, "data:", 5)) {
			snprintf(a, an, "%s", v);
			return true;
		}
	if ((attr(n, "srcset", v, sizeof v) || attr(n, "data-srcset", v, sizeof v)) && src_first(v, a, an))
		return true;
	// en un <picture>: el primer <source> con srcset
	dom_node_internal *p = NI(n)->parent;
	if (p)
		for (dom_node_internal *c = p->first_child; c; c = c->next) {
			if (c->type != DOM_ELEMENT_NODE)
				continue;
			char t[12];
			tag_of((dom_node *)c, t, sizeof t);
			if (!strcmp(t, "source") && (attr((dom_node *)c, "srcset", v, sizeof v) || attr((dom_node *)c, "data-srcset", v, sizeof v)) && src_first(v, a, an))
				return true;
		}
	return attr(n, "src", a, an) && !strncmp(a, "data:image/", 11) && strstr(a, ";base64,") && strlen(a) > 100; // (las de 1 px no)
}

/* una capa fija detrás de todo («position: fixed; inset: 0; z-index: -1», el fondo de Linktree): no se
 * desplaza con la página, así que es su fondo. Su color y los de sus capas absolutas, mezclados en él */
static void backdrop(Ctx *C, dom_node *n, const css_computed_style *cs)
{
	css_color c;
	if (css_computed_background_color(cs, &c) == CSS_BACKGROUND_COLOR_COLOR && (c >> 24)) {
		uint32_t s = css_col(c), d = C->page_bg ? C->page_bg : 0xFFFFFFFF;
		float a = (float)(s >> 24) / 255;
		uint32_t o = 0xFF000000;
		for (int k = 0; k < 24; k += 8)
			o |= (uint32_t)(((s >> k) & 255) * a + ((d >> k) & 255) * (1 - a) + 0.5f) << k;
		C->page_bg = o;
	}
#ifdef BRL_DEBUG
	if (getenv("BRL_BD"))
		fprintf(stderr, "fondo fijo %08x -> página %08x hijos %p\n", (unsigned)c, C->page_bg, (void *)NI(n)->first_child);
#endif
	for (dom_node_internal *ch = NI(n)->first_child; ch; ch = ch->next) {
		if (ch->type != DOM_ELEMENT_NODE)
			continue;
		css_computed_style *ccs = brcss_style(C->L->css, (dom_node *)ch, cs, C->vw);
		if (!ccs)
			continue;
		uint8_t p = css_computed_position(ccs);
#ifdef BRL_DEBUG
		if (getenv("BRL_BD"))
			fprintf(stderr, "  hijo pos=%d d=%d\n", p, css_computed_display(ccs, false));
#endif
		if (css_computed_display(ccs, false) != CSS_DISPLAY_NONE && (p == CSS_POSITION_ABSOLUTE || p == CSS_POSITION_FIXED))
			backdrop(C, (dom_node *)ch, ccs);
		css_computed_style_destroy(ccs);
	}
}

static void build_el(Ctx *C, Box *parent, dom_node *n, const css_computed_style *pcs, int *olc)
{
	if (C->stop)
		return;
	if ((++C->nel & 127) == 0 && C->L->stop && C->L->stop(C->L->u)) {
		C->stop = true;
		return;
	}
	char tag[16];
	tag_of(n, tag, sizeof tag);
	if (in_list(tag, NEVER))
		return;
	char *env = NULL;
	css_computed_style *cs = brcss_style_env(C->L->css, n, pcs, C->vw, parent ? parent->s->env : NULL, &env);
	if (env) { // (al montón de la maqueta)
		size_t el = strlen(env);
		char *ea = arena(C, el + 1);
		if (ea)
			memcpy(ea, env, el + 1);
		free(env);
		env = ea;
	}
	BrcssPseudo ps __attribute__((cleanup(ps_free))) = brcss_ps; /* (lo pisan los hijos; se suelta al salir) */
	brcss_ps.st[0] = brcss_ps.st[1] = NULL;
	if (!cs)
		return;
	uint8_t d = css_computed_display(cs, pcs == NULL);
	// fija y detrás de todo (z-index < 0): un fondo. Si ocupa todo el ancho, el de la página; si no (una columna
	// centrada), una caja de arriba abajo de la página, en su sitio
	bool fixbg = false;
	if (d != CSS_DISPLAY_NONE && css_computed_position(cs) == CSS_POSITION_FIXED) {
		int32_t z = 0;
		css_fixed mv;
		css_unit mu;
		if (css_computed_z_index(cs, &z) == CSS_Z_INDEX_SET && z < 0) {
			uint8_t mk = css_computed_max_width(cs, &mv, &mu);
			float mx = mk == CSS_MAX_WIDTH_SET ? (mu == CSS_UNIT_PCT ? FIXTOFLT(mv) * C->vw / 100 : brcss_px(cs, mv, mu, C->vw)) : 1e9f;
			if (mx >= C->vw - 1)
				backdrop(C, n, cs);
			else
				fixbg = true;
		}
	}
	if (d == CSS_DISPLAY_NONE || d == CSS_DISPLAY_TABLE_COLUMN || d == CSS_DISPLAY_TABLE_COLUMN_GROUP || (css_computed_position(cs) == CSS_POSITION_FIXED && !fixbg) ||
	    invisible(C, cs, !strcmp(tag, "img") || !strcmp(tag, "svg") || !strcmp(tag, "input") || !strcmp(tag, "select") || !strcmp(tag, "textarea"))) {
#ifdef BRL_DEBUG
		if (getenv("BRL_SKIP") && !strcmp(tag, getenv("BRL_SKIP")))
			fprintf(stderr, "fuera <%s> d=%d pos=%d\n", tag, d, css_computed_position(cs));
#endif
		css_computed_style_destroy(cs);
		return;
	}
	if (d == CSS_DISPLAY_BLOCK && css_computed_column_fill(cs) == CSS_COLUMN_FILL_AUTO && parent) {
		// display: contents (de cssprep): no hay caja; sus hijos van en la del padre
		build_kids(C, parent, n, cs, olc);
		css_computed_style_destroy(cs);
		return;
	}
	Sty *s = sty_new(C, parent ? parent->s : NULL);
	if (!s) {
		css_computed_style_destroy(cs);
		return;
	}
	s->node = n;
	s->scope = (int16_t)brcss_scope_of(C->L->css, n, s->scope);
	if (env)
		s->env = env;
	sty_fill(C, s, cs, d);
	if (fixbg)
		s->fixbg = 1, s->pos = CSS_POSITION_ABSOLUTE;
	if (css_computed_text_transform(cs) == CSS_TEXT_TRANSFORM_UPPERCASE)
		s->upper = 1;
	char a[512];
	if (!strcmp(tag, "html"))
		s->tag = T_HTML;
	else if (!strcmp(tag, "body"))
		s->tag = T_BODY;
	if (s->tag == T_HTML || s->tag == T_BODY) // su overflow es el de la ventana (la que se desplaza): no recortan
		s->ovx = s->ovy = CSS_OVERFLOW_VISIBLE;
	if ((s->tag == T_HTML || (s->tag == T_BODY && !C->bg_html)) && s->bg) {
		C->page_bg = s->bg | 0xFF000000;
		C->bg_html |= s->tag == T_HTML;
	}
	if (!strcmp(tag, "a") && attr(n, "href", a, sizeof a) && a[0] && strncasecmp(a, "javascript:", 11) && C->L->link)
		s->link = (int16_t)C->L->link(C->L->u, n, a);
	if (s->link < 0 && C->L->clk &&
	    (attr(n, "onclick", a, sizeof a) || (attr(n, "role", a, sizeof a) && (!strcmp(a, "button") || !strcmp(a, "tab") || !strcmp(a, "link")))))
		s->clk = (int16_t)C->L->clk(C->L->u, n);
	if (s->inner == I_CELL && attr(n, "colspan", a, sizeof a) && atoi(a) > 1)
		s->colspan = (int16_t)(atoi(a) > 50 ? 50 : atoi(a));
	// una imagen de fondo (no se pinta) con texto claro encima: un fondo oscuro, para que se lea
	if (!s->bg && s->tag != T_HTML && s->tag != T_BODY) {
		lwc_string *bi = NULL;
		if (css_computed_background_image(cs, &bi) == CSS_BACKGROUND_IMAGE_IMAGE && bi) {
			uint32_t tc = s->col;
			float tl = (0.2126f * (tc & 0xFF) + 0.7152f * ((tc >> 8) & 0xFF) + 0.0722f * ((tc >> 16) & 0xFF)) / 255;
			if (tl > 0.6f)
				s->bg = 0xFF38302B;
		}
	}
	if (s->flt == CSS_FLOAT_LEFT || s->flt == CSS_FLOAT_RIGHT) {
		if (s->pos != CSS_POSITION_ABSOLUTE)
			s->outer = 0;
		else
			s->flt = 0;
	} else
		s->flt = 0;
	uint8_t oof = s->pos == CSS_POSITION_ABSOLUTE ? O_ABS : s->flt ? O_FLOAT : O_FLOW;
	if (oof)
		s->outer = 0;
	if (parent && parent->s->li == 0 && s->li && olc) { // número de la lista
		if (attr(n, "value", a, sizeof a) && a[0])
			*olc = atoi(a) - 1;
		s->lnum = (int16_t)++*olc;
	} else if (s->li && olc)
		s->lnum = (int16_t)++*olc;
	Box *b = NULL;
	// lo que se sustituye por algo: imágenes, dibujos y controles
	bool replaced = false;
	float iw = 0, ih = 0;
	if (!strcmp(tag, "img")) {
		replaced = true;
		bool ok = img_src(n, a, sizeof a);
		char v[32];
		if (attr(n, "width", v, sizeof v) && atof(v) > 0)
			iw = (float)atof(v);
		if (attr(n, "height", v, sizeof v) && atof(v) > 0)
			ih = (float)atof(v);
		if (ok && !C->L->light && C->L->img) {
			float kw = 0, kh = 0;
			s->img = (int16_t)C->L->img(C->L->u, n, a, &kw, &kh);
			if (kw > 0 && kh > 0) { // ya se sabe: lo que pida la página manda, la proporción de la imagen
				if (iw > 0 && ih <= 0)
					ih = iw * kh / kw;
				else if (ih > 0 && iw <= 0)
					iw = ih * kw / kh;
				else if (iw <= 0)
					iw = kw, ih = kh;
			}
		}
		if (s->img < 0) { // sin imagen: su texto (si lo tiene y es un enlace, o en modo ligero)
			char alt[160];
			attr(n, "alt", alt, sizeof alt);
			css_computed_style_destroy(cs);
			if (alt[0] && (s->link >= 0 || !C->L->light) && parent) {
				s->outer = 1, s->inner = I_FLOW;
				b = box_new(C, parent, K_INLINE, s);
				if (b) {
					Box *t = box_new(C, b, K_TEXT, s);
					size_t l = strlen(alt);
					char *p = t ? arena(C, l + 3) : NULL;
					if (p) {
						snprintf(p, l + 3, C->L->light ? "%s " : "[%s]", alt);
						t->t = p, t->tn = (uint32_t)strlen(p);
					}
				}
			}
			return;
		}
		if (iw <= 0 && ih <= 0)
			iw = 120, ih = 90;
		else if (iw <= 0)
			iw = ih * 1.33f;
		else if (ih <= 0)
			ih = iw * 0.75f;
	} else if (!strcmp(tag, "svg")) {
		replaced = true;
		if (C->L->light || !C->L->svg || (s->img = (int16_t)C->L->svg(C->L->u, n, cs, &iw, &ih)) < 0) {
			css_computed_style_destroy(cs);
			return;
		}
	} else if (!strcmp(tag, "input") || !strcmp(tag, "button") || !strcmp(tag, "select") || !strcmp(tag, "textarea")) {
		replaced = true;
		if (!C->L->wdg || (s->wdg = (int16_t)C->L->wdg(C->L->u, n, tag, cs, s->fpx, &iw, &ih)) < 0) {
			css_computed_style_destroy(cs);
			return;
		}
	}
	if (replaced) {
		if (s->inner != I_FLOW)
			s->inner = I_FLOW;
		if (!oof && d == CSS_DISPLAY_INLINE)
			s->outer = 1;
		s->iw = iw, s->ih = ih;
		b = box_new(C, parent, K_ATOM, s);
		if (b)
			b->oof = oof;
		css_computed_style_destroy(cs);
		return;
	}
	if (!strcmp(tag, "br")) {
		box_new(C, parent, K_BR, s);
		css_computed_style_destroy(cs);
		return;
	}
	if (!strcmp(tag, "hr")) {
		s->tag = T_HR;
		if (!s->bw[0] && !s->bw[2] && AUT(s, LH)) { // sin borde: la raya de siempre
			s->bw[0] = 1;
			s->bc[0] = 0xFFD1CCC8;
		}
	}
	bool inl = s->outer && s->inner == I_FLOW && d == CSS_DISPLAY_INLINE && !oof;
	b = box_new(C, parent, inl ? K_INLINE : K_BLOCK, s);
	if (!b) {
		css_computed_style_destroy(cs);
		return;
	}
	b->oof = oof;
	int ol = 0;
	bool list = !strcmp(tag, "ol") || !strcmp(tag, "ul") || !strcmp(tag, "menu");
	if (!strcmp(tag, "ol") && attr(n, "start", a, sizeof a) && a[0])
		ol = atoi(a) - 1;
	pseudo_box(C, b, ps.before, ps.st[0], ps.icon[0]);
	build_kids(C, b, n, cs, list ? &ol : olc);
	pseudo_box(C, b, ps.after, ps.st[1], ps.icon[1]);
	if (ps.after_clear && b->kind == K_BLOCK) { // el «clearfix»: debajo de los flotantes de dentro
		Sty *cl = sty_new(C, s);
		if (cl) {
			cl->aut = AUTO_ALL;
			cl->clr = CSS_CLEAR_BOTH, cl->vis = 1, cl->fpx = s->fpx, cl->lh = 0, cl->shrink = 1;
			box_new(C, b, K_BLOCK, cl);
		}
	}
	css_computed_style_destroy(cs);
	if (b->kind == K_INLINE) { // un elemento en línea con bloques dentro: se hace bloque (tarjetas hechas con <a>)
		bool blk = false;
		for (Box *c = b->first; c && !blk; c = c->next)
			blk = !c->oof && !inline_level(c);
		if (blk) {
			b->kind = K_BLOCK;
			s->outer = 0;
		}
	}
	if (b->kind == K_BLOCK)
		fixup(C, b);
}

static void build_kids(Ctx *C, Box *b, dom_node *n, const css_computed_style *cs, int *olc)
{
	for (dom_node_internal *c = NI(n)->first_child; c && !C->stop; c = c->next) {
		if (c->type == DOM_TEXT_NODE || c->type == DOM_CDATA_SECTION_NODE)
			build_text(C, b, c);
		else if (c->type == DOM_ELEMENT_NODE)
			build_el(C, b, (dom_node *)c, cs, olc);
	}
}

/* ------------------------------------------------------------ palabras */
static bool is_cjk(const unsigned char *p)
{
	// U+2E80… (CJK, kana, hangul): se pueden partir entre letras
	return p[0] >= 0xE2 && !(p[0] == 0xE2 && p[1] < 0xBA) && p[0] < 0xF0 && (p[0] != 0xEF || p[1] < 0xBC);
}

static void words(Ctx *C, Box *b)
{
	if (b->wd || !b->tn)
		return;
	Sty *s = b->s;
	uint8_t ws = s->ws;
	bool pre = ws == CSS_WHITE_SPACE_PRE, prew = ws == CSS_WHITE_SPACE_PRE_WRAP, keepnl = pre || prew || ws == CSS_WHITE_SPACE_PRE_LINE;
	const char *t = b->t;
	uint32_t n = b->tn;
	int cap = 8;
	for (uint32_t i = 0; i < n; i++)
		cap += t[i] == ' ' || t[i] == '\n' || ((unsigned char)t[i] >= 0xE2 && (unsigned char)t[i] < 0xF0);
	Word *w = arena(C, sizeof *w * (size_t)cap);
	if (!w)
		return;
	int nw = 0;
	uint32_t i = 0;
	int sp = 0;
	while (i < n && nw < cap - 1) {
		char ch = t[i];
		if (ch == '\n' && keepnl) {
			w[nw++] = (Word){.off = i, .n = 0, .brk = 1};
			sp = 0;
			i++;
			continue;
		}
		if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\f') {
			if (pre) { // pre: los espacios son parte de la línea
			} else {
				sp = prew ? sp + 1 : 1;
				if (nw)
					w[nw - 1].tsp = 1;
				i++;
				continue;
			}
		}
		uint32_t j = i;
		if (pre) {
			while (j < n && t[j] != '\n')
				j++;
		} else if (is_cjk((const unsigned char *)t + i)) {
			j = i + 3;
			if (j > n)
				j = n;
		} else
			while (j < n && t[j] != ' ' && t[j] != '\n' && t[j] != '\t' && t[j] != '\r' && t[j] != '\f' && !is_cjk((const unsigned char *)t + j))
				j++;
		if (j - i > 0xFFFF)
			j = i + 0xFFFF;
		Word *k = &w[nw++];
		k->off = i, k->n = (uint16_t)(j - i), k->sp = (uint8_t)(sp > 255 ? 255 : sp);
		k->bk = is_cjk((const unsigned char *)t + i) || (nw > 1 && is_cjk((const unsigned char *)t + w[nw - 2].off) && w[nw - 2].n);
		k->w = tw_(C, t + i, (int)(j - i), s);
		sp = 0;
		i = j;
	}
	if (prew && sp && nw < cap) // pre-wrap: los espacios del final también ocupan (en una palabra vacía)
		w[nw++] = (Word){.off = n, .n = 0, .sp = (uint8_t)(sp > 255 ? 255 : sp)};
	else if (sp && nw)
		w[nw - 1].tsp = 1;
	b->tsp = sp > 0 || (n && (t[n - 1] == ' ' || t[n - 1] == '\n' || t[n - 1] == '\t'));
	b->wd = w, b->nwd = nw;
	if (!nw)
		b->wd = w; // (sin palabras: no se vuelve a mirar)
}

static bool lead_space(const Box *b)
{
	return b->tn && (b->t[0] == ' ' || b->t[0] == '\n' || b->t[0] == '\t' || b->t[0] == '\r');
}

/* ------------------------------------------------------------ anchos mínimo y máximo (sin partir) */
static void intr(Ctx *C, Box *b, float *mn, float *mx);

static float mpx(const Sty *s, int i) // un margen fijo (los % y auto, 0)
{
	return AUT(s, i) || PCT(s, i) ? 0 : s->lv[i];
}

typedef struct {
	float line, mn, mx, run;
	bool sp, nowrap;
} IS;

static void intr_inl(Ctx *C, Box *b, IS *st)
{
	for (Box *c = b->first; c; c = c->next) {
		if (c->oof == O_ABS)
			continue;
		if (c->oof == O_FLOAT || c->kind == K_ATOM || (c->kind == K_BLOCK)) {
			float a, z;
			intr(C, c, &a, &z);
			float m = mpx(c->s, LML) + mpx(c->s, LMR);
			st->mn = fmaxf(st->mn, a + m);
			st->line += z + m;
			st->run = 0;
			continue;
		}
		if (c->kind == K_BR) {
			st->mx = fmaxf(st->mx, st->line);
			st->line = st->run = 0;
			continue;
		}
		if (c->kind == K_INLINE) {
			Sty *s = c->s;
			float l = mpx(s, LML) + mpx(s, LPL) + s->bw[3], r = mpx(s, LMR) + mpx(s, LPR) + s->bw[1];
			st->line += l;
			intr_inl(C, c, st);
			st->line += r;
			continue;
		}
		if (c->kind != K_TEXT)
			continue;
		words(C, c);
		Sty *s = c->s;
		bool nowrap = s->ws == CSS_WHITE_SPACE_NOWRAP || s->ws == CSS_WHITE_SPACE_PRE;
		if (lead_space(c))
			st->sp = true;
		for (int i = 0; i < c->nwd; i++) {
			Word *w = &c->wd[i];
			if (w->brk) {
				st->mx = fmaxf(st->mx, st->line);
				st->line = st->run = 0;
				st->sp = false;
				continue;
			}
			bool sp = st->sp || w->sp;
			float spx = sp && st->line > 0 ? spw(C, s) * (w->sp > 1 ? w->sp : 1) : 0;
			st->line += spx + w->w;
			if (nowrap || (!sp && !w->bk))
				st->run += spx + w->w;
			else
				st->run = w->w;
			st->mn = fmaxf(st->mn, st->run);
			st->sp = w->tsp;
		}
		if (c->tsp)
			st->sp = true;
	}
}

static bool bfc_root(const Box *b)
{
	const Sty *s = b->s;
	return b->oof || s->outer || s->item || s->inner == I_CELL || s->inner == I_CAPTION || s->inner == I_FLEX || s->inner == I_GRID || s->inner == I_TABLE ||
	       (s->ovx != CSS_OVERFLOW_VISIBLE && s->ovx != 0) || s->tag == T_HTML || !b->parent;
}

static int table_cols(Ctx *C, Box *t, float *cmn, float *cmx, int maxc);

static void intr(Ctx *C, Box *b, float *mn, float *mx)
{
	Sty *s = b->s;
	if (s->icv) {
		*mn = s->minc, *mx = s->maxc;
		return;
	}
	float sum[4], sup[4];
	memcpy(sum, s->um, sizeof sum), memcpy(sup, s->up, sizeof sup);
	resolve(s, 0);
	float ex = hx(s), a = 0, z = 0;
	bool has;
	float sw = spec_w(s, 0, &has);
	if (b->kind == K_ATOM) {
		float w = s->iw;
		if (has)
			w = sw - ex;
		else if (!AUT(s, LH) && !PCT(s, LH) && s->ih > 0) // alto pedido: el ancho por su proporción
			w = s->iw * LV(s, LH, 0) / s->ih;
		a = z = w + ex;
	} else if (has)
		a = z = sw;
	else {
		if (s->ifc) {
			IS st = {0};
			intr_inl(C, b, &st);
			a = st.mn, z = fmaxf(st.mx, st.line);
		} else if (s->inner == I_FLEX && (s->fdir == CSS_FLEX_DIRECTION_ROW || s->fdir == CSS_FLEX_DIRECTION_ROW_REVERSE || s->fdir == 0)) {
			float gap = LV(s, LCGAP, 0);
			int k = 0;
			for (Box *c = b->first; c; c = c->next) {
				if (c->oof == O_ABS)
					continue;
				float ca, cz;
				intr(C, c, &ca, &cz);
				float m = mpx(c->s, LML) + mpx(c->s, LMR);
				if (s->fwrap)
					a = fmaxf(a, ca + m);
				else
					a += ca + m;
				z += cz + m + (k++ ? gap : 0);
			}
			if (!s->fwrap && k > 1)
				a += gap * (k - 1);
		} else if (s->inner == I_GRID) {
			int nt = s->ncols ? s->ncols : 1;
			float cmn[16] = {0}, cmx[16] = {0}, gap = LV(s, LCGAP, 0);
			int col = 0;
			for (Box *c = b->first; c; c = c->next) {
				if (c->oof == O_ABS)
					continue;
				float ca, cz;
				intr(C, c, &ca, &cz);
				float m = mpx(c->s, LML) + mpx(c->s, LMR);
				int span = c->s->gall ? nt : c->s->gsp > 0 ? c->s->gsp : 1;
				if (span > nt)
					span = nt;
				if (c->s->gcs > 0)
					col = c->s->gcs - 1 < nt ? c->s->gcs - 1 : 0;
				if (col + span > nt)
					col = 0;
				for (int i = col; i < col + span; i++) {
					cmn[i] = fmaxf(cmn[i], (ca + m) / span);
					cmx[i] = fmaxf(cmx[i], (cz + m) / span);
				}
				col = (col + span) % nt;
			}
			for (int i = 0; i < nt; i++) {
				if (s->tw[i] > 0)
					cmn[i] = cmx[i] = s->tw[i];
				a += cmn[i], z += cmx[i];
			}
			a += gap * (nt - 1), z += gap * (nt - 1);
		} else if (s->inner == I_TABLE) {
			float *cm = malloc(sizeof(float) * 128);
			int nc = cm ? table_cols(C, b, cm, cm + 64, 64) : 0;
			float sp = s->bcol ? 0 : s->bsh;
			for (int i = 0; i < nc; i++)
				a += cm[i], z += cm[64 + i];
			a += sp * (nc + 1), z += sp * (nc + 1);
			free(cm);
		} else {
			float run = 0; // flotantes seguidos: van en fila (su ancho máximo se suma)
			for (Box *c = b->first; c; c = c->next) {
				if (c->oof == O_ABS)
					continue;
				float ca, cz;
				intr(C, c, &ca, &cz);
				float m = mpx(c->s, LML) + mpx(c->s, LMR);
				a = fmaxf(a, ca + m);
				if (c->oof == O_FLOAT && !c->s->clr) {
					run += cz + m;
					z = fmaxf(z, run);
				} else {
					run = c->oof == O_FLOAT ? cz + m : 0;
					z = fmaxf(z, cz + m);
				}
			}
		}
		a += ex, z += ex;
	}
	if (!AUT(s, LMAXW) && !PCT(s, LMAXW)) {
		float m = s->lv[LMAXW] + (s->bbox ? 0 : ex);
		a = fminf(a, m), z = fminf(z, m);
	}
	if (!AUT(s, LMINW) && !PCT(s, LMINW)) {
		float m = s->lv[LMINW] + (s->bbox ? 0 : ex);
		a = fmaxf(a, m), z = fmaxf(z, m);
	}
	if (z < a)
		z = a;
	memcpy(s->um, sum, sizeof sum), memcpy(s->up, sup, sizeof sup);
	s->minc = a, s->maxc = z, s->icv = true;
	*mn = a, *mx = z;
}

// ajustar al contenido: entre su mínimo y su máximo, lo que quepa
static float fit_w(Ctx *C, Box *b, float avail)
{
	float a, z;
	intr(C, b, &a, &z);
	return fminf(fmaxf(a, avail), z);
}

/* ------------------------------------------------------------ flotantes */
static void fl_bounds(const BFC *bf, float y, float h, float L0, float R0, float *l, float *r)
{
	*l = L0, *r = R0;
	if (!bf)
		return;
	for (int i = 0; i < bf->n; i++) {
		const FlR *f = &bf->f[i];
		if (y >= f->y1 || y + h <= f->y0 || f->x1 <= L0 || f->x0 >= R0)
			continue;
		if (f->right)
			*r = fminf(*r, f->x0);
		else
			*l = fmaxf(*l, f->x1);
	}
}
// la siguiente y (> y) en la que acaba algún flotante que estrecha [L0, R0]
static float fl_next(const BFC *bf, float y, float L0, float R0)
{
	float best = INF;
	for (int i = 0; bf && i < bf->n; i++)
		if (bf->f[i].y1 > y + 0.01f && bf->f[i].y1 < best && bf->f[i].x1 > L0 && bf->f[i].x0 < R0)
			best = bf->f[i].y1;
	return best;
}
static float fl_clear(const BFC *bf, uint8_t clr)
{
	float y = -INF;
	for (int i = 0; bf && i < bf->n; i++)
		if (clr == CSS_CLEAR_BOTH || (clr == CSS_CLEAR_LEFT && !bf->f[i].right) || (clr == CSS_CLEAR_RIGHT && bf->f[i].right))
			y = fmaxf(y, bf->f[i].y1);
	return y;
}
static float fl_bottom(const BFC *bf)
{
	return fl_clear(bf, CSS_CLEAR_BOTH);
}
static float fl_top(const BFC *bf) // un flotante no sube por encima de los de antes
{
	return bf && bf->n ? bf->f[bf->n - 1].y0 : -INF;
}
// el sitio (en coordenadas de la BFC) de un flotante de w × h desde y, entre L0 y R0
static void fl_place(BFC *bf, float y, float w, float h, bool right, float L0, float R0, float *ox, float *oy)
{
	y = fmaxf(y, fl_top(bf));
	for (int guard = 0; guard < 200; guard++) {
		float l, r;
		fl_bounds(bf, y, fmaxf(h, 0.01f), L0, R0, &l, &r);
		if (r - l >= w - 0.5f || (l <= L0 && r >= R0)) {
			*ox = right ? r - w : l;
			*oy = y;
			break;
		}
		float ny = fl_next(bf, y, L0, R0);
		if (ny >= INF) {
			*ox = right ? R0 - w : L0;
			*oy = y;
			break;
		}
		y = ny;
	}
	if (bf->n == bf->c) {
		int nc = bf->c ? bf->c * 2 : 8;
		FlR *nf = realloc(bf->f, sizeof *nf * nc);
		if (!nf)
			return;
		bf->f = nf, bf->c = nc;
	}
	bf->f[bf->n++] = (FlR){*ox, *oy, *ox + w, *oy + h, right};
}

/* ------------------------------------------------------------ colocar */
static void layout_box(Ctx *C, Box *b, float defh, BFC *bf, float ox, float oy);

static void frag_add(Sty *s, LFrag f)
{
	if (s->nfr == s->cfr) {
		int nc = s->cfr ? s->cfr * 2 : 16;
		LFrag *nf = realloc(s->fr, sizeof *nf * nc);
		if (!nf)
			return;
		s->fr = nf, s->cfr = nc;
	}
	s->fr[s->nfr++] = f;
}

// el tamaño de algo que se sustituye (imagen, dibujo, control)
static void atom_size_h(Ctx *C, Box *b, float cbw, float cbh)
{
	Sty *s = b->s;
	resolve(s, cbw);
	float w = s->iw, h = s->ih, ar = s->ih > 0 && s->wdg < 0 ? s->iw / s->ih : 0; // (un control no tiene proporción)
	bool hw = !AUT(s, LW) && (!PCT(s, LW) || cbw > 0), hh = !AUT(s, LH) && (!PCT(s, LH) || cbh >= 0);
	if (hw)
		w = LV(s, LW, cbw) - (s->bbox ? hx(s) : 0);
	if (hh)
		h = LV(s, LH, cbh) - (s->bbox ? vy(s) : 0);
	if (s->ar > 0) // aspect-ratio manda sobre la proporción de la imagen
		ar = s->ar;
	if (hw && !hh && ar > 0)
		h = w / ar;
	else if (hh && !hw && ar > 0)
		w = h * ar;
	if (!AUT(s, LMAXW) && (!PCT(s, LMAXW) || cbw > 0)) {
		float m = LV(s, LMAXW, cbw) - (s->bbox ? hx(s) : 0);
		if (w > m && m >= 0) {
			if (!hh && ar > 0)
				h = m / ar;
			w = m;
		}
	}
	if (!AUT(s, LMAXH) && !PCT(s, LMAXH)) {
		float m = LV(s, LMAXH, 0) - (s->bbox ? vy(s) : 0);
		if (h > m && m >= 0) {
			if (!hw && ar > 0)
				w = m * ar;
			h = m;
		}
	}
	if (!AUT(s, LMINW) && !PCT(s, LMINW))
		w = fmaxf(w, LV(s, LMINW, 0) - (s->bbox ? hx(s) : 0));
	if (!AUT(s, LMINH) && !PCT(s, LMINH))
		h = fmaxf(h, LV(s, LMINH, 0) - (s->bbox ? vy(s) : 0));
	b->w = fmaxf(w, 0) + hx(s);
	b->h = fmaxf(h, 0) + vy(s);
	s->laid = true, s->layw = b->w;
}
static void atom_size(Ctx *C, Box *b, float cbw)
{
	atom_size_h(C, b, cbw, -1);
}

// el ancho de algo que se ajusta a su contenido (flotantes, inline-block, absolutas): caja de borde
static float shrink_w(Ctx *C, Box *b, float cbw, float avail)
{
	Sty *s = b->s;
	resolve(s, cbw);
	bool has;
	float w = spec_w(s, cbw, &has);
	if (!has)
		w = fit_w(C, b, avail - s->um[1] - s->um[3]);
	return clamp_w(s, w, cbw);
}

/* --- texto en líneas */
enum { IT_WORD, IT_OPEN, IT_CLOSE, IT_ATOM, IT_BR, IT_FLOAT, IT_ABS };
typedef struct {
	uint8_t t;
	Box *b;
	Word *w;
} It;
typedef struct {
	It *v;
	int n, c;
} Its;

static void it_push(Its *v, uint8_t t, Box *b, Word *w)
{
	if (v->n == v->c) {
		int nc = v->c ? v->c * 2 : 64;
		It *nv = realloc(v->v, sizeof *nv * nc);
		if (!nv)
			return;
		v->v = nv, v->c = nc;
	}
	v->v[v->n++] = (It){t, b, w};
}

static void flatten(Ctx *C, Box *b, Its *v)
{
	for (Box *c = b->first; c; c = c->next) {
		if (c->oof == O_FLOAT)
			it_push(v, IT_FLOAT, c, NULL);
		else if (c->oof == O_ABS)
			it_push(v, IT_ABS, c, NULL);
		else if (c->kind == K_TEXT) {
			words(C, c);
			if (lead_space(c))
				it_push(v, IT_WORD, c, NULL); // un espacio (sin palabra)
			for (int i = 0; i < c->nwd; i++)
				it_push(v, IT_WORD, c, &c->wd[i]);
			if (c->tsp && c->nwd == 0)
				it_push(v, IT_WORD, c, NULL);
		} else if (c->kind == K_INLINE) {
			it_push(v, IT_OPEN, c, NULL);
			flatten(C, c, v);
			it_push(v, IT_CLOSE, c, NULL);
		} else if (c->kind == K_BR)
			it_push(v, IT_BR, c, NULL);
		else if (c->kind == K_ATOM || c->kind == K_BLOCK) {
			if (c->kind == K_BLOCK && !c->s->outer) { // un bloque suelto entre texto: en su propia línea
				it_push(v, IT_BR, NULL, NULL);
				it_push(v, IT_ATOM, c, NULL);
				it_push(v, IT_BR, NULL, NULL);
			} else
				it_push(v, IT_ATOM, c, NULL);
		}
	}
}

static bool can_wrap(const Sty *s)
{
	return s->ws != CSS_WHITE_SPACE_NOWRAP && s->ws != CSS_WHITE_SPACE_PRE;
}

typedef struct {
	int it;        /* elemento */
	float x, w;    /* posición desde el borde izquierdo de la línea, ancho */
	uint32_t off;  /* palabra partida: desde dónde y cuánto */
	uint16_t n;
} Pl;

static float ifc(Ctx *C, Box *b, float cw, BFC *bf, float ox, float oy, float cx0, float cy0)
{
	Sty *s = b->s;
	Its v = {0};
	flatten(C, b, &v);
	if (!v.n)
		return 0;
	Pl *pl = malloc(sizeof *pl * (v.n + 2));
	if (!pl) {
		free(v.v);
		return 0;
	}
	float gh0 = glyph_h(C, s), A0 = gh0 * 0.8f + (s->lh - gh0) / 2, D0 = s->lh - A0;
	float y = 0, tind = LV(s, LTIND, cw);
	int i = 0;
	uint32_t woff = 0; /* palabra partida: lo ya puesto */
	bool first = true, sp = false, soft = false; /* soft: la línea de antes se partió por falta de sitio */
	// los en línea abiertos (para pintar sus fondos por trozos de línea)
	Box *open[32];
	int nopen = 0;
	int guard = 0;
	while (i < v.n && guard++ < 200000) {
		float l, r;
		fl_bounds(bf, oy + y, s->lh, ox, ox + cw, &l, &r);
		l -= ox, r -= ox;
		float avail = r - l, x = first ? tind : 0;
		int m = 0, i0 = i;
		bool any = false, brk = false, moved = false, sp_open = false;
		float last_end = x;
		Box *open0[32];
		int nopen0 = nopen;
		memcpy(open0, open, sizeof(Box *) * nopen);
		while (i < v.n) {
			It *t = &v.v[i];
			if (t->t == IT_WORD) {
				Sty *ts = t->b->s;
				Word *w = t->w;
				if (!w) { // un espacio suelto
					sp = sp || any;
					i++;
					continue;
				}
				if (w->brk) {
					i++;
					brk = true;
					any = true;
					sp = false;
					soft = false;
					break;
				}
				bool prew = ts->ws == CSS_WHITE_SPACE_PRE_WRAP;
				bool space = (sp || w->sp) && (any || (prew && w->sp && !soft)); /* pre-wrap: también al empezar la línea (no tras partirla) */
				float spx = space ? spw(C, ts) * (w->sp > 1 ? w->sp : 1) : 0;
				const char *ws0 = t->b->t + w->off + woff;
				int wn = w->n - (int)woff;
				float ww = woff ? tw_(C, ws0, wn, ts) : w->w;
				bool bok = any && (space || sp_open || w->bk || (m && v.v[pl[m - 1].it].t == IT_ATOM)) && can_wrap(ts);
				if (bok && x + spx + ww > avail + 0.01f) {
					soft = true;
					break;
				}
				if (!any && ww > avail + 0.01f && (l > 0.01f || r < cw - 0.01f) && fl_next(bf, oy + y, ox, ox + cw) < INF) {
					// no cabe al lado de los flotantes: más abajo
					y = fl_next(bf, oy + y, ox, ox + cw) - oy;
					moved = true;
					break;
				}
				if (!any && ww > fmaxf(avail, 200) && wn > 1 && can_wrap(ts)) { // enorme (direcciones largas…): a trozos
					int k = wn;
					while (k > 1 && tw_(C, ws0, k, ts) > avail - x)
						k = k * 3 / 4 < k - 1 ? k * 3 / 4 : k - 1;
					while (k > 1 && (ws0[k] & 0xC0) == 0x80)
						k--;
					pl[m++] = (Pl){i, x, tw_(C, ws0, k, ts), w->off + woff, (uint16_t)k};
					woff += (uint32_t)k;
					any = true;
					x += pl[m - 1].w;
					last_end = x;
					break;
				}
				x += spx;
				sp_open = false;
				pl[m++] = (Pl){i, x, ww, w->off + woff, (uint16_t)wn};
				x += ww;
				last_end = x;
				woff = 0;
				any = true;
				soft = false;
				sp = w->tsp;
				i++;
				continue;
			}
			if (t->t == IT_OPEN || t->t == IT_CLOSE) {
				Sty *is = t->b->s;
				resolve(is, cw);
				float e = t->t == IT_OPEN ? is->um[3] + is->up[3] + is->bw[3] : is->um[1] + is->up[1] + is->bw[1];
				if (t->t == IT_OPEN && sp && any) { // el espacio va antes de abrir (y ahí se puede partir)
					x += spw(C, is);
					sp = false;
					sp_open = true;
				}
				pl[m++] = (Pl){i, x, e, 0, 0};
				x += e;
				if (t->t == IT_OPEN && nopen < 32)
					open[nopen++] = t->b;
				else if (t->t == IT_CLOSE && nopen)
					nopen--;
				i++;
				continue;
			}
			if (t->t == IT_BR) {
				i++;
				brk = true;
				any = true;
				sp = false;
				if (t->b)
					pl[m++] = (Pl){i - 1, x, 0, 0, 0};
				break;
			}
			if (t->t == IT_ABS) { // su sitio «estático»: donde iría; se coloca al pintar
				Sty *as = t->b->s;
				if (!as->placed) {
					as->placed = 1;
					as->sx = cx0 + l + x, as->sy = cy0 + y;
					frag_add(s, (LFrag){0, 0, 0, 0, t->b, NULL, 0, F_OOF});
				}
				i++;
				continue;
			}
			if (t->t == IT_FLOAT) {
				Box *fb = t->b;
				Sty *fs = fb->s;
				if (fs->placed) {
					i++;
					continue;
				}
				fs->placed = 1;
				if (fb->kind == K_ATOM)
					atom_size(C, fb, cw);
				else {
					fb->w = shrink_w(C, fb, cw, cw);
					layout_box(C, fb, -1, NULL, 0, 0);
				}
				float W = fb->w + fs->um[1] + fs->um[3], H = fb->h + fs->um[0] + fs->um[2];
				float fx, fy, sy0 = oy + y;
				if (any && x + W > avail) // no cabe en esta línea: debajo
					sy0 = oy + y + s->lh;
				if (fs->clr)
					sy0 = fmaxf(sy0, fl_clear(bf, fs->clr));
				if (bf)
					fl_place(bf, sy0, W, H, fs->flt == CSS_FLOAT_RIGHT, ox, ox + cw, &fx, &fy);
				else
					fx = fs->flt == CSS_FLOAT_RIGHT ? ox + cw - W : ox, fy = sy0;
				fb->x = cx0 + (fx - ox) + fs->um[3];
				fb->y = cy0 + (fy - oy) + fs->um[0];
				frag_add(s, (LFrag){fb->x, fb->y, fb->w, fb->h, fb, NULL, 0, F_OOF});
				if (fy <= oy + y + 0.01f) { // en esta línea: se estrecha
					float l2, r2;
					fl_bounds(bf, oy + y, s->lh, ox, ox + cw, &l2, &r2);
					l2 -= ox, r2 -= ox;
					if (l2 > l) { // lo ya puesto se corre
						float dx = l2 - l;
						(void)dx;
					}
					l = l2, r = r2, avail = r - l;
				}
				i++;
				continue;
			}
			if (t->t == IT_ATOM) {
				Box *ab = t->b;
				Sty *as = ab->s;
				if (ab->kind == K_ATOM)
					atom_size(C, ab, cw);
				else if (!as->laid || as->layw <= 0) {
					resolve(as, cw);
					ab->w = as->outer ? shrink_w(C, ab, cw, cw) : clamp_w(as, cw - as->um[1] - as->um[3], cw);
					if (!as->outer) {
						bool has;
						float sw = spec_w(as, cw, &has);
						if (has)
							ab->w = clamp_w(as, sw, cw);
					}
					layout_box(C, ab, -1, NULL, 0, 0);
				}
				resolve(as, cw);
				float W = ab->w + as->um[1] + as->um[3];
				if (sp && any)
					x += spw(C, s), sp = false;
				if (any && x + W > avail + 0.5f && can_wrap(s))
					break;
				pl[m++] = (Pl){i, x, W, 0, 0};
				x += W;
				last_end = x;
				any = true;
				sp = false;
				i++;
				continue;
			}
			i++;
		}
		if (moved) {
			i = i0;
			nopen = nopen0;
			memcpy(open, open0, sizeof(Box *) * nopen);
			continue;
		}
		if (i == i0 && !m && !brk) { // nada cupo: se fuerza
			i++;
			continue;
		}
		(void)last_end;
		// alto de la línea: lo más alto sobre la base y lo más bajo debajo (con el «puntal» de la caja)
		float A = A0, D = D0;
		bool content = false;
		for (int k = 0; k < m; k++) {
			It *t = &v.v[pl[k].it];
			if (t->t == IT_WORD) {
				Sty *ts = t->b->s;
				float gh = glyph_h(C, ts), a = gh * 0.8f + (ts->lh - gh) / 2, d = ts->lh - a;
				float sh = ts->va == CSS_VERTICAL_ALIGN_SUPER ? -ts->fpx * 0.35f : ts->va == CSS_VERTICAL_ALIGN_SUB ? ts->fpx * 0.2f : ts->va == CSS_VERTICAL_ALIGN_SET ? -ts->vash : 0;
				A = fmaxf(A, a - sh), D = fmaxf(D, d + sh);
				content = true;
			} else if (t->t == IT_ATOM) {
				Box *ab = t->b;
				Sty *as = ab->s;
				float H = ab->h + as->um[0] + as->um[2], a = H, d = 0;
				if (as->va == CSS_VERTICAL_ALIGN_MIDDLE) {
					float xh = s->fpx * 0.25f;
					a = H / 2 + xh, d = H / 2 - xh;
				} else if (as->va == CSS_VERTICAL_ALIGN_TOP || as->va == CSS_VERTICAL_ALIGN_TEXT_TOP) {
					a = A0, d = H - A0;
				} else if (ab->kind == K_BLOCK && as->lbase >= 0 && as->ovx == CSS_OVERFLOW_VISIBLE) {
					a = as->um[0] + as->lbase, d = H - a;
				}
				A = fmaxf(A, a), D = fmaxf(D, d);
				content = true;
			} else if (t->t == IT_BR && t->b)
				content = true;
			else if (t->t == IT_OPEN || t->t == IT_CLOSE) {
				Sty *is = t->b->s;
				if (is->bg || is->bw[0] || is->bw[1] || is->bw[2] || is->bw[3] || is->up[1] > 0 || is->up[3] > 0)
					content = true;
			}
		}
		if (brk && !content && m == 0)
			content = true; // <br> en una línea vacía: ocupa su alto
		if (!content) {
			first = first && m == 0;
			continue;
		}
		float lineh = A + D;
		float used = 0;
		for (int k = 0; k < m; k++)
			used = fmaxf(used, pl[k].x + pl[k].w);
		float shift = 0;
		if (s->ta == 1)
			shift = fmaxf(0, (avail - used) / 2);
		else if (s->ta == 2)
			shift = fmaxf(0, avail - used);
		float bx = cx0 + l + shift, base = cy0 + y + A;
		// fondos de los en línea: de donde se abren (o del principio de la línea) a donde se cierran
		float segx[32];
		int nseg = nopen0;
		Box *seg[32];
		memcpy(seg, open0, sizeof(Box *) * nopen0);
		for (int k = 0; k < nseg; k++)
			segx[k] = bx;
		for (int k = 0; k < m; k++) {
			It *t = &v.v[pl[k].it];
			if (t->t == IT_OPEN && nseg < 32) {
				seg[nseg] = t->b, segx[nseg] = bx + pl[k].x;
				nseg++;
			} else if (t->t == IT_CLOSE && nseg) {
				Box *ib = seg[nseg - 1];
				Sty *is = ib->s;
				if (is->bg || is->bw[0] || is->bw[1] || is->bw[2] || is->bw[3]) {
					float gh = glyph_h(C, is), x0 = segx[nseg - 1] + is->um[3], x1 = bx + pl[k].x + pl[k].w - is->um[1];
					frag_add(s, (LFrag){x0, base - gh * 0.8f - is->up[0] - is->bw[0], x1 - x0, gh + is->up[0] + is->up[2] + is->bw[0] + is->bw[2], ib, NULL, 0, F_BG});
				}
				nseg--;
			}
		}
		for (int k = 0; k < nseg; k++) { // siguen abiertos: hasta el final de la línea
			Sty *is = seg[k]->s;
			if (is->bg || is->bw[0] || is->bw[2]) {
				float gh = glyph_h(C, is), x0 = segx[k] + (k >= nopen0 ? is->um[3] : 0), x1 = bx + used;
				frag_add(s, (LFrag){x0, base - gh * 0.8f - is->up[0] - is->bw[0], x1 - x0, gh + is->up[0] + is->up[2] + is->bw[0] + is->bw[2], seg[k], NULL, 0, F_BG});
			}
		}
		for (int k = 0; k < m; k++) {
			It *t = &v.v[pl[k].it];
			if (t->t == IT_WORD) {
				Sty *ts = t->b->s;
				float gh = glyph_h(C, ts);
				float sh = ts->va == CSS_VERTICAL_ALIGN_SUPER ? -ts->fpx * 0.35f : ts->va == CSS_VERTICAL_ALIGN_SUB ? ts->fpx * 0.2f : ts->va == CSS_VERTICAL_ALIGN_SET ? -ts->vash : 0;
				frag_add(s, (LFrag){bx + pl[k].x, base + sh - gh * 0.8f, pl[k].w, gh, t->b, t->b->t + pl[k].off, pl[k].n, F_TEXT});
			} else if (t->t == IT_ATOM) {
				Box *ab = t->b;
				Sty *as = ab->s;
				float H = ab->h + as->um[0] + as->um[2], top;
				if (as->va == CSS_VERTICAL_ALIGN_MIDDLE)
					top = base - s->fpx * 0.25f - H / 2;
				else if (as->va == CSS_VERTICAL_ALIGN_TOP || as->va == CSS_VERTICAL_ALIGN_TEXT_TOP)
					top = cy0 + y;
				else if (ab->kind == K_BLOCK && as->lbase >= 0 && as->ovx == CSS_OVERFLOW_VISIBLE)
					top = base - as->um[0] - as->lbase;
				else
					top = base - H;
				ab->x = bx + pl[k].x + as->um[3];
				ab->y = top + as->um[0];
				frag_add(s, (LFrag){ab->x, ab->y, ab->w, ab->h, ab, NULL, 0, F_BOX});
			}
		}
		if (s->fbase < 0)
			s->fbase = base;
		s->lbase = base;
		y += lineh;
		first = false;
	}
	free(pl);
	free(v.v);
	return y;
}

/* --- bloques uno debajo de otro (con los márgenes que se juntan) */
typedef struct {
	float pos, neg;
} Mc;
static void mc_add(Mc *m, float v)
{
	if (v > m->pos)
		m->pos = v;
	if (v < m->neg)
		m->neg = v;
}
static float mc_val(const Mc *m) { return m->pos + m->neg; }

// el ancho y los márgenes de un bloque en el flujo; x relativa al contenido
static void size_block(Ctx *C, Box *c, float cbw, float avail)
{
	Sty *s = c->s;
	resolve(s, cbw);
	if (c->kind == K_ATOM) {
		atom_size(C, c, cbw);
	} else {
		bool has;
		float w = spec_w(s, cbw, &has);
		if (!has) {
			if (s->inner == I_TABLE)
				w = fit_w(C, c, avail - s->um[1] - s->um[3]);
			else
				w = avail - s->um[1] - s->um[3];
		}
		c->w = clamp_w(s, w, cbw);
	}
	float rest = avail - c->w - s->um[1] - s->um[3];
	bool aml = AUT(s, LML), amr = AUT(s, LMR);
	uint8_t pc = c->parent ? c->parent->s->bcen : 0; // dentro de <center> (o align="center")
	if (rest > 0) {
		if (aml && amr)
			s->um[3] += rest / 2, s->um[1] += rest / 2;
		else if (aml)
			s->um[3] += rest;
		else if (amr)
			s->um[1] += rest;
		else if (pc == 1)
			s->um[3] += rest / 2, s->um[1] += rest / 2;
		else if (pc == 2)
			s->um[3] += rest;
	}
}

static float blocks(Ctx *C, Box *b, float cw, float dh, BFC *bf, float ox, float oy, float cx0, float cy0, bool own_bfc)
{
	Sty *s = b->s;
	float y = 0;
	Mc pend = {0, 0}, top = {0, 0};
	bool through_top = !own_bfc && s->bw[0] == 0 && s->up[0] == 0;
	bool at_top = true;
	for (Box *c = b->first; c && !C->stop; c = c->next) {
		Sty *cs = c->s;
		if (!c->oof && (c->kind == K_TEXT || c->kind == K_INLINE || c->kind == K_BR)) // (no deberían estar aquí)
			continue;
		if (c->oof == O_ABS) {
			resolve(cs, cw);
			cs->sx = cx0, cs->sy = cy0 + y + (at_top && through_top ? 0 : mc_val(&pend));
			continue;
		}
		if (c->oof == O_FLOAT) {
			if (c->kind == K_ATOM)
				atom_size(C, c, cw);
			else {
				c->w = shrink_w(C, c, cw, cw);
				layout_box(C, c, -1, NULL, 0, 0);
			}
			float W = c->w + cs->um[1] + cs->um[3], H = c->h + cs->um[0] + cs->um[2];
			float sy0 = oy + y + (at_top && through_top ? 0 : mc_val(&pend)), fx, fy;
			if (cs->clr)
				sy0 = fmaxf(sy0, fl_clear(bf, cs->clr));
			if (bf)
				fl_place(bf, sy0, W, H, cs->flt == CSS_FLOAT_RIGHT, ox, ox + cw, &fx, &fy);
			else
				fx = cs->flt == CSS_FLOAT_RIGHT ? ox + cw - W : ox, fy = sy0;
			c->x = cx0 + (fx - ox) + cs->um[3];
			c->y = cy0 + (fy - oy) + cs->um[0];
#ifdef BRL_DEBUG
			if (getenv("BRL_FL"))
				fprintf(stderr, "float w=%.3f x=%.3f h=%.0f sy0=%.0f oy=%.0f fy=%.0f nfl=%d top=%.0f\n", W, fx, H, sy0, oy, fy, bf ? bf->n : -1, fl_top(bf));
#endif
			continue;
		}
		resolve(cs, cw);
		// clear: debajo de los flotantes
		if (cs->clr && bf) {
			float fy = fl_clear(bf, cs->clr) - oy;
			float here = y + mc_val(&pend) + cs->um[0];
			if (fy > -INF / 2 && fy > here) {
				y = fy - cs->um[0];
				pend = (Mc){0, 0};
				at_top = false;
			}
		}
		bool own = bfc_root(c) || c->kind == K_ATOM;
		float L0 = 0, R0 = cw;
		Mc m = pend;
		mc_add(&m, cs->um[0]);
		float yc = (at_top && through_top) ? y : y + mc_val(&m);
		if (own && bf && bf->n) { // junto a los flotantes, sin meterse debajo
			size_block(C, c, cw, cw);
			float need = c->w + cs->um[1] + cs->um[3];
			for (int guard = 0; guard < 100; guard++) {
				float l, r;
				fl_bounds(bf, oy + yc, 1, ox, ox + cw, &l, &r);
				L0 = l - ox, R0 = r - ox;
				if (R0 - L0 >= need - 0.01f || (L0 <= 0 && R0 >= cw))
					break;
				bool has;
				spec_w(cs, cw, &has);
				if (!has && cs->inner != I_TABLE && c->kind != K_ATOM && R0 - L0 > 40)
					break; // sin ancho pedido: el que quede
				float ny = fl_next(bf, oy + yc, ox, ox + cw);
				if (ny >= INF)
					break;
				yc = ny - oy;
			}
		}
		size_block(C, c, cw, R0 - L0);
		c->x = cx0 + L0 + cs->um[3];
		c->y = cy0 + yc;
		float cdh = dh;
		if (cdh < 0 && cs->tag == T_HTML) // <html>: sus % de alto son de la ventana
			cdh = C->vh;
		if (c->kind != K_ATOM)
			layout_box(C, c, cdh, own ? NULL : bf, ox + (c->x - cx0) + cs->up[3] + cs->bw[3], oy + yc + cs->up[0] + cs->bw[0]);
		// el margen de arriba de su primer hijo se junta con el suyo
		if (cs->cmt != 0 && !own) {
			mc_add(&m, cs->cmt);
			float yc2 = (at_top && through_top) ? y : y + mc_val(&m);
			c->y += yc2 - yc;
			yc = yc2;
		}
		bool empty = c->h <= 0.01f && c->kind != K_ATOM && cs->nfr == 0 && !own;
		if (empty) { // vacío: sus márgenes se juntan por él
			mc_add(&pend, cs->um[0]);
			mc_add(&pend, cs->cmt);
			mc_add(&pend, cs->um[2]);
			mc_add(&pend, cs->cmb);
			if (at_top && through_top)
				top = pend;
			continue;
		}
		if (at_top && through_top) {
			top = m;
		}
		if (s->fbase < 0 && cs->fbase >= 0)
			s->fbase = c->y + cs->fbase;
		if (cs->lbase >= 0)
			s->lbase = c->y + cs->lbase;
		y = yc + c->h;
		pend = (Mc){0, 0};
		mc_add(&pend, cs->um[2]);
		if (!own)
			mc_add(&pend, cs->cmb);
		at_top = false;
	}
	s->cmt = through_top ? mc_val(&top) : 0;
	bool through_bot = !own_bfc && s->bw[2] == 0 && s->up[2] == 0 && AUT(s, LH) && AUT(s, LMINH);
	if (at_top && through_top) { // todo vacío
		s->cmt = mc_val(&pend);
		s->cmb = 0;
	} else if (through_bot)
		s->cmb = mc_val(&pend);
	else {
		y += mc_val(&pend);
		s->cmb = 0;
	}
	return y;
}

/* --- flex */
typedef struct {
	Box *b;
	float base, hyp, mn, mx, main, cross, ext, cext;
	bool frozen;
	int line;
} FI;

static int ord_cmp(const void *a, const void *b)
{
	const FI *x = a, *y = b;
	if (x->b->s->order != y->b->s->order)
		return x->b->s->order < y->b->s->order ? -1 : 1;
	return x->line - y->line; /* (aquí, el orden del documento) */
}

static float flex(Ctx *C, Box *b, float cw, float dh, float cx0, float cy0)
{
	Sty *s = b->s;
	bool row = s->fdir == CSS_FLEX_DIRECTION_ROW || s->fdir == CSS_FLEX_DIRECTION_ROW_REVERSE || s->fdir == 0;
	bool rev = s->fdir == CSS_FLEX_DIRECTION_ROW_REVERSE || s->fdir == CSS_FLEX_DIRECTION_COLUMN_REVERSE;
	int n = 0;
	for (Box *c = b->first; c; c = c->next)
		n += c->oof != O_ABS;
	for (Box *c = b->first; c; c = c->next)
		if (c->oof == O_ABS)
			resolve(c->s, cw), c->s->sx = cx0, c->s->sy = cy0;
	if (!n)
		return dh >= 0 ? dh : 0;
	FI *it = calloc((size_t)n, sizeof *it);
	if (!it)
		return 0;
	int k = 0;
	for (Box *c = b->first; c; c = c->next)
		if (c->oof != O_ABS)
			it[k].b = c, it[k].line = k, k++;
	qsort(it, (size_t)n, sizeof *it, ord_cmp);
	float gmain = row ? LV(s, LCGAP, cw) : LV(s, LRGAP, dh), gcross = row ? LV(s, LRGAP, dh) : LV(s, LCGAP, cw);
	float avail = row ? cw : (dh >= 0 ? dh : INF);
	for (int i = 0; i < n; i++) {
		FI *f = &it[i];
		Box *c = f->b;
		Sty *is = c->s;
		resolve(is, cw);
		f->ext = row ? is->um[1] + is->um[3] : is->um[0] + is->um[2];
		f->cext = row ? is->um[0] + is->um[2] : is->um[1] + is->um[3];
		bool has;
		float a = 0, z = 0;
		if (row || c->kind == K_ATOM)
			intr(C, c, &a, &z);
		if (c->kind == K_ATOM)
			atom_size(C, c, cw);
		// tamaño base
		if (!AUT(is, LBASIS) && (!PCT(is, LBASIS) || avail < INF)) {
			float bv = LV(is, LBASIS, avail);
			f->base = is->bbox ? fmaxf(bv, row ? hx(is) : vy(is)) : bv + (row ? hx(is) : vy(is));
		} else if (row && (f->base = spec_w(is, cw, &has), has)) {
		} else if (!row && (f->base = spec_h(is, dh, &has), has)) {
		} else if (c->kind == K_ATOM)
			f->base = row ? c->w : c->h;
		else if (row)
			f->base = z;
		else { // en columna: el alto del contenido con su ancho
			float wv = spec_w(is, cw, &has);
			uint8_t al = is->as != CSS_ALIGN_SELF_AUTO && is->as ? is->as : s->ai;
			if (!has)
				wv = (al == CSS_ALIGN_ITEMS_STRETCH || al == 0) ? cw - f->cext : fit_w(C, c, cw - f->cext);
			c->w = clamp_w(is, wv, cw);
			layout_box(C, c, -1, NULL, 0, 0);
			f->base = c->h;
		}
		// lo mínimo: su contenido (si no dice otro y no recorta)
		if (row) {
			if (!AUT(is, LMINW) && (!PCT(is, LMINW) || cw > 0))
				f->mn = clamp_w(is, 0, cw);
			else if (is->ovx == CSS_OVERFLOW_VISIBLE || is->ovx == 0) {
				float sw = spec_w(is, cw, &has);
				f->mn = has ? fminf(a, sw) : a;
				if (!AUT(is, LMAXW) && !PCT(is, LMAXW))
					f->mn = fminf(f->mn, is->lv[LMAXW] + (is->bbox ? 0 : hx(is)));
			} else
				f->mn = hx(is);
			f->mx = INF;
			if (!AUT(is, LMAXW) && (!PCT(is, LMAXW) || cw > 0))
				f->mx = LV(is, LMAXW, cw) + (is->bbox ? 0 : hx(is));
		} else {
			f->mn = !AUT(is, LMINH) && (!PCT(is, LMINH) || dh >= 0) ? LV(is, LMINH, dh) + (is->bbox ? 0 : vy(is)) : (is->ovy == CSS_OVERFLOW_VISIBLE || is->ovy == 0) && AUT(is, LBASIS) ? f->base : vy(is);
			f->mx = !AUT(is, LMAXH) && (!PCT(is, LMAXH) || dh >= 0) ? LV(is, LMAXH, dh) + (is->bbox ? 0 : vy(is)) : INF;
		}
		if (f->mx < f->mn)
			f->mx = f->mn;
		f->hyp = fminf(fmaxf(f->base, f->mn), f->mx);
	}
	// líneas
	int nl = 0;
	int *ls = malloc(sizeof(int) * (size_t)(n + 1)); /* dónde empieza cada línea */
	float *lc = malloc(sizeof(float) * (size_t)(n + 1));
	if (!ls || !lc) {
		free(it), free(ls), free(lc);
		return 0;
	}
	{
		float sum = 0;
		ls[nl++] = 0;
		int cnt = 0;
		for (int i = 0; i < n; i++) {
			float o = it[i].hyp + it[i].ext;
			if (s->fwrap && cnt && avail < INF && sum + gmain + o > avail + 0.5f) {
				ls[nl++] = i;
				sum = 0, cnt = 0;
			}
			sum += (cnt ? gmain : 0) + o;
			cnt++;
			it[i].line = nl - 1;
		}
		ls[nl] = n;
	}
	float total_main = 0;
	for (int L = 0; L < nl; L++) {
		int a = ls[L], e = ls[L + 1];
		float used = 0;
		for (int i = a; i < e; i++)
			used += it[i].hyp + it[i].ext + (i > a ? gmain : 0), it[i].main = it[i].hyp, it[i].frozen = false;
		float fr = avail < INF ? avail - used : 0;
		bool grow = fr > 0;
		for (int iter = 0; iter < 5 && fabsf(fr) > 0.01f; iter++) {
			float sumf = 0;
			for (int i = a; i < e; i++)
				if (!it[i].frozen)
					sumf += grow ? it[i].b->s->grow : it[i].b->s->shrink * it[i].hyp;
			if (sumf <= 0)
				break;
			if (grow && sumf < 1) // la suma de flex-grow por debajo de 1: solo esa parte
				fr *= sumf;
			bool clamped = false;
			float nf = fr;
			for (int i = a; i < e; i++) {
				FI *f = &it[i];
				if (f->frozen)
					continue;
				float share = grow ? fr * f->b->s->grow / sumf : fr * f->b->s->shrink * f->hyp / sumf;
				float t = f->main + share;
				if (t < f->mn)
					t = f->mn, f->frozen = true, clamped = true;
				else if (t > f->mx)
					t = f->mx, f->frozen = true, clamped = true;
				nf -= t - f->main;
				f->main = t;
			}
			fr = nf;
			if (!clamped)
				break;
			if (grow && sumf < 1)
				break;
		}
		// colocar cada elemento con su tamaño y medir el otro eje
		float lcross = 0;
		for (int i = a; i < e; i++) {
			FI *f = &it[i];
			Box *c = f->b;
			Sty *is = c->s;
			uint8_t al = is->as != CSS_ALIGN_SELF_AUTO && is->as ? is->as : s->ai;
			bool has;
			if (row) {
				if (c->kind == K_ATOM) {
					float ar = c->h > 0 ? c->w / c->h : 0;
					if (fabsf(c->w - f->main) > 0.5f && ar > 0 && AUT(is, LH))
						c->h = vy(is) + (f->main - hx(is)) / ar;
					c->w = f->main;
				} else {
					c->w = f->main;
					is->laid = false;
					layout_box(C, c, dh >= 0 && !s->fwrap ? dh - f->cext : -1, NULL, 0, 0);
				}
				f->cross = c->h;
			} else {
				if (c->kind != K_ATOM) {
					float wv = spec_w(is, cw, &has);
					if (!has)
						wv = (al == CSS_ALIGN_ITEMS_STRETCH || al == 0) ? cw - f->cext : fit_w(C, c, cw - f->cext);
					c->w = clamp_w(is, wv, cw);
					if (!is->laid || fabsf(is->layw - c->w) > 0.01f)
						layout_box(C, c, f->main - vy(is), NULL, 0, 0);
				}
				if (fabsf(c->h - f->main) > 0.01f)
					c->h = f->main;
				f->cross = c->w;
			}
			lcross = fmaxf(lcross, f->cross + f->cext);
		}
		if (nl == 1) { // una sola línea con alto fijo: ocupa ese alto
			if (row && dh >= 0)
				lcross = dh;
			else if (!row)
				lcross = cw;
		}
		lc[L] = lcross;
		float u2 = 0;
		for (int i = a; i < e; i++)
			u2 += it[i].main + it[i].ext + (i > a ? gmain : 0);
		total_main = fmaxf(total_main, u2);
	}
	// alto (o ancho) del contenedor
	float cross_total = 0;
	for (int L = 0; L < nl; L++)
		cross_total += lc[L] + (L ? gcross : 0);
	float main_size = row ? cw : (dh >= 0 ? dh : total_main);
	// colocar
	float cpos = 0;
	for (int L = 0; L < nl; L++) {
		int a = ls[L], e = ls[L + 1], cnt = e - a, nauto = 0;
		float used = 0;
		for (int i = a; i < e; i++) {
			Sty *is = it[i].b->s;
			used += it[i].main + it[i].ext + (i > a ? gmain : 0);
			nauto += row ? AUT(is, LML) + AUT(is, LMR) : AUT(is, LMT) + AUT(is, LMB);
		}
		float free_m = main_size - used, start = 0, between = 0;
		if (free_m > 0 && nauto) {
			float each = free_m / nauto;
			for (int i = a; i < e; i++) {
				Sty *is = it[i].b->s;
				if (row) {
					if (AUT(is, LML))
						is->um[3] += each;
					if (AUT(is, LMR))
						is->um[1] += each;
				} else {
					if (AUT(is, LMT))
						is->um[0] += each;
					if (AUT(is, LMB))
						is->um[2] += each;
				}
			}
			free_m = 0;
		} else if (free_m > 0) {
			switch (s->jc) {
			case CSS_JUSTIFY_CONTENT_FLEX_END: start = free_m; break;
			case CSS_JUSTIFY_CONTENT_CENTER: start = free_m / 2; break;
			case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: between = cnt > 1 ? free_m / (cnt - 1) : 0; break;
			case CSS_JUSTIFY_CONTENT_SPACE_AROUND: between = free_m / cnt, start = between / 2; break;
			case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: between = free_m / (cnt + 1), start = between; break;
			default: break;
			}
		}
		float pos = start;
		for (int i = a; i < e; i++) {
			FI *f = &it[i];
			Box *c = f->b;
			Sty *is = c->s;
			uint8_t al = is->as != CSS_ALIGN_SELF_AUTO && is->as ? is->as : s->ai;
			float mstart = row ? is->um[3] : is->um[0], mend = row ? is->um[1] : is->um[2];
			float cm0 = row ? is->um[0] : is->um[3];
			float off = 0, lcr = lc[L];
			bool autocross = row ? (AUT(is, LMT) && AUT(is, LMB)) : (AUT(is, LML) && AUT(is, LMR));
			if (al == CSS_ALIGN_ITEMS_STRETCH || al == 0) {
				if (row && AUT(is, LH) && c->kind != K_ATOM && !autocross) {
					float nh = clamp_h(is, lcr - f->cext, -1);
					if (nh > c->h)
						c->h = nh;
				}
			}
			float csz = row ? c->h : c->w;
			if (autocross)
				off = fmaxf(0, (lcr - csz - f->cext) / 2);
			else if (al == CSS_ALIGN_ITEMS_CENTER)
				off = (lcr - csz - f->cext) / 2;
			else if (al == CSS_ALIGN_ITEMS_FLEX_END)
				off = lcr - csz - f->cext;
			float mp = pos + mstart, sz = row ? c->w : c->h;
			if (rev)
				mp = main_size - mp - sz;
			if (row)
				c->x = cx0 + mp, c->y = cy0 + cpos + off + cm0;
			else
				c->y = cy0 + mp, c->x = cx0 + cpos + off + cm0;
			if (row && s->fbase < 0 && is->fbase >= 0)
				s->fbase = c->y - cy0 + is->fbase;
			pos += sz + mstart + mend + gmain + between;
		}
		cpos += lc[L] + gcross;
	}
	free(ls), free(lc), free(it);
	if (s->fbase >= 0)
		s->fbase += cy0, s->lbase = s->fbase;
	return row ? cross_total : main_size;
}

/* --- rejilla (las pistas y posiciones vienen de cssprep) */
static float grid(Ctx *C, Box *b, float cw, float dh, float cx0, float cy0)
{
	Sty *s = b->s;
	int nt = s->ncols ? s->ncols : 1;
	float cg = LV(s, LCGAP, cw), rg = LV(s, LRGAP, dh);
	int n = 0;
	for (Box *c = b->first; c; c = c->next) {
		if (c->oof == O_ABS)
			resolve(c->s, cw), c->s->sx = cx0, c->s->sy = cy0;
		else
			n++;
	}
	if (!n)
		return dh >= 0 ? dh : 0;
	FI *it = calloc((size_t)n, sizeof *it);
	int *col = malloc(sizeof(int) * (size_t)n), *row = malloc(sizeof(int) * (size_t)n), *span = malloc(sizeof(int) * (size_t)n);
	if (!it || !col || !row || !span) {
		free(it), free(col), free(row), free(span);
		return 0;
	}
	int k = 0;
	for (Box *c = b->first; c; c = c->next)
		if (c->oof != O_ABS)
			it[k].b = c, it[k].line = k, k++;
	qsort(it, (size_t)n, sizeof *it, ord_cmp);
	// colocación automática (por filas)
	int r = 0, cc = 0, nrows = 0;
	for (int i = 0; i < n; i++) {
		Sty *is = it[i].b->s;
		int sp = is->gall ? nt : is->gsp > 0 ? is->gsp : 1;
		if (sp > nt)
			sp = nt;
		int st = is->gall ? 0 : is->gcs > 0 ? is->gcs - 1 : -1;
		if (st >= nt)
			st = nt - 1;
		if (st >= 0 && st + sp > nt)
			sp = nt - st;
		if (is->gr > 0) { // fila pedida
			r = is->gr - 1;
			cc = st >= 0 ? st : 0;
		} else if (st >= 0) {
			if (st < cc)
				r++;
			cc = st;
		} else if (cc + sp > nt)
			r++, cc = 0;
		col[i] = cc, row[i] = r, span[i] = sp;
		cc += sp;
		if (cc >= nt)
			cc = 0, r++;
		if (row[i] + 1 > nrows)
			nrows = row[i] + 1;
	}
	// anchos de las pistas
	float tw[16], room = cw - cg * (nt - 1), fixed = 0, frs = 0, autos = 0;
	float amx[16] = {0}, amn[16] = {0};
	for (int i = 0; i < n; i++)
		if (span[i] == 1) {
			float a, z;
			Box *c = it[i].b;
			intr(C, c, &a, &z);
			float m = mpx(c->s, LML) + mpx(c->s, LMR);
			amn[col[i]] = fmaxf(amn[col[i]], a + m);
			amx[col[i]] = fmaxf(amx[col[i]], z + m);
		}
	for (int i = 0; i < nt; i++) {
		int t = s->tw[i];
		if (t > 0)
			fixed += t;
		else if (t == -1)
			autos += amx[i];
		else
			frs += -t / 10.0f;
	}
	float kf = fixed > room && fixed > 0 ? room / fixed : 1;
	float rest = room - fixed * kf;
	float ka = autos > rest && autos > 0 ? fmaxf(0, rest) / autos : 1;
	rest -= autos * ka;
	for (int i = 0; i < nt; i++) {
		int t = s->tw[i];
		if (t > 0)
			tw[i] = t * kf;
		else if (t == -1)
			tw[i] = fmaxf(amx[i] * ka, fminf(amn[i], amx[i]));
		else
			tw[i] = frs > 0 ? fmaxf(0, rest) * (-t / 10.0f) / frs : 0;
	}
	if (frs <= 0 && rest > 0 && autos > 0) { // sin fr: las auto se estiran
		int na = 0;
		for (int i = 0; i < nt; i++)
			na += s->tw[i] == -1;
		for (int i = 0; i < nt; i++)
			if (s->tw[i] == -1)
				tw[i] += rest / na;
	}
	for (int i = 0; i < nt; i++) // una pista fr no baja de lo mínimo de lo que tiene
		if (s->tw[i] < -1 && tw[i] < amn[i])
			tw[i] = fminf(amn[i], cw);
	float tx[17];
	tx[0] = 0;
	for (int i = 0; i < nt; i++)
		tx[i + 1] = tx[i] + tw[i] + cg;
	// filas
	float *rh = calloc((size_t)nrows + 1, sizeof(float));
	if (!rh) {
		free(it), free(col), free(row), free(span);
		return 0;
	}
	for (int i = 0; i < n; i++) {
		Box *c = it[i].b;
		Sty *is = c->s;
		resolve(is, cw);
		float area = tx[col[i] + span[i]] - tx[col[i]] - cg;
		if (c->kind == K_ATOM)
			atom_size(C, c, area);
		else {
			bool has;
			float w = spec_w(is, area, &has);
			if (!has)
				w = area - is->um[1] - is->um[3];
			c->w = clamp_w(is, w, area);
			layout_box(C, c, -1, NULL, 0, 0);
		}
		float rest2 = area - c->w - is->um[1] - is->um[3];
		if (rest2 > 0 && AUT(is, LML) && AUT(is, LMR))
			is->um[3] += rest2 / 2;
		else if (rest2 > 0 && AUT(is, LML))
			is->um[3] += rest2;
		rh[row[i]] = fmaxf(rh[row[i]], c->h + is->um[0] + is->um[2]);
	}
	float *ry = calloc((size_t)nrows + 1, sizeof(float));
	if (!ry) {
		free(rh), free(it), free(col), free(row), free(span);
		return 0;
	}
	float y = 0;
	for (int i = 0; i < nrows; i++) {
		ry[i] = y;
		y += rh[i] + (i < nrows - 1 ? rg : 0);
	}
	for (int i = 0; i < n; i++) {
		Box *c = it[i].b;
		Sty *is = c->s;
		uint8_t al = is->as != CSS_ALIGN_SELF_AUTO && is->as ? is->as : s->ai;
		float h = rh[row[i]], off = 0;
		if ((al == CSS_ALIGN_ITEMS_STRETCH || al == 0) && AUT(is, LH) && c->kind != K_ATOM) {
			float nh = h - is->um[0] - is->um[2];
			if (nh > c->h)
				c->h = nh;
		} else if (al == CSS_ALIGN_ITEMS_CENTER)
			off = (h - c->h - is->um[0] - is->um[2]) / 2;
		else if (al == CSS_ALIGN_ITEMS_FLEX_END)
			off = h - c->h - is->um[0] - is->um[2];
		c->x = cx0 + tx[col[i]] + is->um[3];
		c->y = cy0 + ry[row[i]] + off + is->um[0];
	}
	free(ry), free(rh), free(it), free(col), free(row), free(span);
	return dh >= 0 ? fmaxf(dh, y) : y;
}

/* --- tablas */
typedef struct {
	Box *row;
	Box *grp;
} TRow;

static int table_rows(Box *t, TRow *rows, int max, Box **caps, int *ncap)
{
	int n = 0;
	*ncap = 0;
	for (Box *c = t->first; c; c = c->next) {
		if (c->oof == O_ABS)
			continue;
		if (c->s->inner == I_CAPTION) {
			if (*ncap < 4)
				caps[(*ncap)++] = c;
		} else if (c->s->inner == I_ROWG) {
			for (Box *r = c->first; r && n < max; r = r->next)
				if (r->oof != O_ABS)
					rows[n++] = (TRow){r, c};
		} else if (n < max)
			rows[n++] = (TRow){c, NULL};
	}
	return n;
}

// los anchos mínimo y máximo de cada columna
static int table_cols(Ctx *C, Box *t, float *cmn, float *cmx, int maxc)
{
	TRow *rows = malloc(sizeof *rows * 512);
	Box *caps[4];
	int ncap, nr = rows ? table_rows(t, rows, 512, caps, &ncap) : 0, nc = 0;
	for (int i = 0; i < maxc; i++)
		cmn[i] = cmx[i] = 0;
	for (int pass = 0; pass < 2; pass++) // primero las de una columna, luego las que ocupan varias
		for (int r = 0; r < nr; r++) {
			Box *row = rows[r].row;
			int ci = 0;
			bool single = row->s->inner != I_ROW;
			for (Box *c = single ? row : row->first; c; c = single ? NULL : c->next) {
				if (c->oof == O_ABS)
					continue;
				int sp = single ? 1 : c->s->colspan;
				if (ci + sp > maxc)
					sp = maxc - ci;
				if (sp <= 0)
					break;
				float a, z;
				intr(C, c, &a, &z);
				if (pass == 0 && sp == 1) {
					cmn[ci] = fmaxf(cmn[ci], a);
					cmx[ci] = fmaxf(cmx[ci], z);
				} else if (pass == 1 && sp > 1) {
					float ha = 0, hz = 0;
					for (int k = ci; k < ci + sp; k++)
						ha += cmn[k], hz += cmx[k];
					if (a > ha)
						for (int k = ci; k < ci + sp; k++)
							cmn[k] += (a - ha) / sp;
					if (z > hz)
						for (int k = ci; k < ci + sp; k++)
							cmx[k] += (z - hz) / sp;
				}
				ci += sp;
			}
			if (ci > nc)
				nc = ci;
		}
	for (int i = 0; i < nc; i++)
		if (cmx[i] < cmn[i])
			cmx[i] = cmn[i];
	free(rows);
	return nc;
}

static float table(Ctx *C, Box *b, float cw, float cx0, float cy0)
{
	Sty *s = b->s;
	float cmn[64], cmx[64], w[64];
	int nc = table_cols(C, b, cmn, cmx, 64);
	float sh = s->bcol ? 0 : s->bsh, sv = s->bcol ? 0 : s->bsv;
	float room = cw - sh * (nc + 1), smn = 0, smx = 0;
	for (int i = 0; i < nc; i++)
		smn += cmn[i], smx += cmx[i];
	bool has;
	spec_w(s, 0, &has);
	for (int i = 0; i < nc; i++) {
		if (room >= smx)
			w[i] = smx > 0 ? cmx[i] + (room - smx) * cmx[i] / smx : room / nc;
		else if (room > smn && smx > smn)
			w[i] = cmn[i] + (cmx[i] - cmn[i]) * (room - smn) / (smx - smn);
		else
			w[i] = cmn[i];
	}
	TRow *rows = malloc(sizeof *rows * 512);
	Box *caps[4];
	int ncap = 0, nr = rows ? table_rows(b, rows, 512, caps, &ncap) : 0;
	float y = 0;
	for (int i = 0; i < ncap; i++) {
		Box *c = caps[i];
		size_block(C, c, cw, cw);
		c->x = cx0 + c->s->um[3];
		c->y = cy0 + y + c->s->um[0];
		layout_box(C, c, -1, NULL, 0, 0);
		y += c->h + c->s->um[0] + c->s->um[2];
	}
	y += sv;
	Box *grp = NULL;
	float gy = 0;
	for (int r = 0; r < nr; r++) {
		Box *row = rows[r].row;
		if (rows[r].grp != grp) {
			if (grp)
				grp->h = y - sv - gy;
			grp = rows[r].grp;
			if (grp) {
				resolve(grp->s, cw);
				grp->x = cx0, grp->y = cy0 + y, grp->w = cw;
				gy = y;
			}
		}
		bool single = row->s->inner != I_ROW;
		float x = sh, rh = 0;
		int ci = 0;
		if (single) { // algo que no es una fila: de lado a lado
			size_block(C, row, cw, cw);
			layout_box(C, row, -1, NULL, 0, 0);
			rh = row->h;
		} else {
			resolve(row->s, cw);
			for (Box *c = row->first; c; c = c->next) {
				if (c->oof == O_ABS)
					continue;
				int sp = c->s->colspan;
				if (ci + sp > nc)
					sp = nc - ci;
				if (sp <= 0)
					sp = 1;
				float cwid = 0;
				for (int k = ci; k < ci + sp && k < nc; k++)
					cwid += w[k] + (k > ci ? sh : 0);
				resolve(c->s, cwid);
				c->w = cwid;
				c->s->laid = false;
				layout_box(C, c, -1, NULL, 0, 0);
				c->x = x, c->y = 0;
				rh = fmaxf(rh, c->h);
				x += cwid + sh;
				ci += sp;
			}
			bool hh;
			float sp_h = spec_h(row->s, -1, &hh);
			if (hh)
				rh = fmaxf(rh, sp_h);
			for (Box *c = row->first; c; c = c->next) {
				if (c->oof == O_ABS)
					continue;
				float ch = c->h;
				c->h = rh;
				if (c->s->va == CSS_VERTICAL_ALIGN_MIDDLE)
					c->s->cy = (rh - ch) / 2;
				else if (c->s->va == CSS_VERTICAL_ALIGN_BOTTOM)
					c->s->cy = rh - ch;
			}
		}
		if (grp)
			row->x = 0, row->y = y - gy;
		else
			row->x = cx0, row->y = cy0 + y;
		row->w = cw, row->h = rh;
		if (single && !grp)
			row->x = cx0 + row->s->um[3];
		y += rh + sv;
	}
	if (grp)
		grp->h = y - sv - gy;
	free(rows);
	return y;
}

static void layout_box(Ctx *C, Box *b, float defh, BFC *bf, float ox, float oy)
{
	Sty *s = b->s;
	if (C->stop)
		return;
	if (b->kind == K_ATOM) {
		float w = b->w;
		atom_size(C, b, 0);
		b->w = w > 0 ? w : b->w;
		return;
	}
	if (s->laid && fabsf(s->layw - b->w) < 0.01f && !bf)
		return;
	free(s->fr);
	s->fr = NULL, s->nfr = s->cfr = 0;
	s->fbase = s->lbase = -1;
	s->laid = true, s->layw = b->w;
	float cw = fmaxf(0, b->w - hx(s));
	float cx0 = s->up[3] + s->bw[3], cy0 = s->up[0] + s->bw[0];
	bool hh;
	float dh = spec_h(s, defh, &hh);
	if (hh)
		dh = clamp_h(s, dh, defh) - vy(s);
	else
		dh = -1;
	bool own = bfc_root(b) || !bf;
	BFC local = {0};
	BFC *ub = own ? &local : bf;
	float uox = own ? 0 : ox, uoy = own ? 0 : oy;
	float ch = 0;
	s->cmt = s->cmb = 0;
	switch (s->inner) {
	case I_FLEX: ch = flex(C, b, cw, dh, cx0, cy0); break;
	case I_GRID: ch = grid(C, b, cw, dh, cx0, cy0); break;
	case I_TABLE: ch = table(C, b, cw, cx0, cy0); break;
	default:
		if (s->ifc)
			ch = ifc(C, b, cw, ub, uox, uoy, cx0, cy0);
		else
			ch = blocks(C, b, cw, dh, ub, uox, uoy, cx0, cy0, own);
		break;
	}
	if (own) {
		float fb = fl_bottom(&local);
		if (fb > ch)
			ch = fb;
	}
	free(local.f);
	if (!hh && s->ar > 0) // aspect-ratio: el alto sale del ancho (o el del contenido, si es más)
		ch = fmaxf(ch, cw / s->ar);
	float h = hh ? dh + vy(s) : clamp_h(s, ch + vy(s), defh);
	b->h = h;
}

/* ------------------------------------------------------------ pintar */
static void marker(Ctx *C, Box *b, float ax, float ay, Clip cl)
{
	Sty *s = b->s;
	if (!s->lst || !s->vis)
		return;
	char mk[16];
	int nmb = s->lnum > 0 ? s->lnum : 1;
	switch (s->lst) {
	case 1: snprintf(mk, sizeof mk, "\xe2\x80\xa2"); break;
	case 3: snprintf(mk, sizeof mk, "%c.", 'a' + (nmb - 1) % 26); break;
	case 4: snprintf(mk, sizeof mk, "%c.", 'A' + (nmb - 1) % 26); break;
	case 5:
	case 6: {
		static const char *const R[] = {"i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii"};
		snprintf(mk, sizeof mk, "%s.", nmb <= 12 ? R[nmb - 1] : "x");
		if (s->lst == 6)
			for (char *p = mk; *p; p++)
				*p = (char)toupper((unsigned char)*p);
		break;
	}
	default: snprintf(mk, sizeof mk, "%d.", nmb); break;
	}
	float gh = glyph_h(C, s), mw = tw_(C, mk, (int)strlen(mk), s);
	float x = ax + s->bw[3] + s->up[3] - mw - s->fpx * 0.45f;
	float y = ay + (s->fbase >= 0 ? s->fbase - gh * 0.8f : s->bw[0] + s->up[0] + (s->lh - gh) / 2);
	if (s->lstin)
		x = ax + s->bw[3] + s->up[3] - mw - 2;
	if (x < cl.x0)
		return;
	BrlText t = {s->fpx, s->col, -1, -1, 0, 0, 0, s->node};
	C->L->text(C->L->u, x, y, mw, gh, mk, (int)strlen(mk), &t);
}

static void paint_bg(Ctx *C, Sty *s, float x0, float y0, float x1, float y1, Clip cl)
{
	bool brd = s->bw[0] || s->bw[1] || s->bw[2] || s->bw[3];
	if (!(s->bg >> 24) && !brd)
		return;
	if (x1 <= cl.x0 || x0 >= cl.x1 || y1 <= cl.y0 || y0 >= cl.y1)
		return;
	float bw[4];
	uint32_t bc[4];
	for (int k = 0; k < 4; k++)
		bw[k] = (s->bc[k] >> 24) > 0x10 ? s->bw[k] : 0, bc[k] = s->bc[k];
	// recortado: los bordes que caen fuera no se pintan
	if (y0 < cl.y0)
		y0 = cl.y0, bw[0] = 0;
	if (y1 > cl.y1)
		y1 = cl.y1, bw[2] = 0;
	if (x0 < cl.x0)
		x0 = cl.x0, bw[3] = 0;
	if (x1 > cl.x1)
		x1 = cl.x1, bw[1] = 0;
	float rad = s->rad < 0 ? fminf(x1 - x0, y1 - y0) * -s->rad / 100 : s->rad;
	C->L->bg(C->L->u, x0, y0, x1, y1, (s->bg >> 24) > 0x10 ? s->bg : 0, bw, bc, fminf(rad, fminf(x1 - x0, y1 - y0) / 2));
}

static void emit(Ctx *C, Box *b, float ax, float ay, Clip cl, int depth);

// el bloque que contiene a una absoluta: el antepasado con posición (o la página)
static Box *abs_cb(Box *b)
{
	for (Box *p = b->parent; p; p = p->parent)
		if (p->kind != K_INLINE && p->s->pos != CSS_POSITION_STATIC && p->s->pos != 0)
			return p;
	return NULL;
}

static void child(Ctx *C, Box *c, float pax, float pay, Clip cl, int depth)
{
	Sty *s = c->s;
	float x = pax + c->x, y = pay + c->y;
	if (c->oof == O_ABS) {
		Box *cb = s->fixbg ? NULL : abs_cb(c);
		float cbx = 0, cby = 0, cbw = C->vw, cbh = s->fixbg && C->doch > 0 ? C->doch : C->vh;
		if (cb) {
			cbx = cb->s->ax + cb->s->bw[3], cby = cb->s->ay + cb->s->bw[0];
			cbw = cb->w - cb->s->bw[1] - cb->s->bw[3], cbh = cb->h - cb->s->bw[0] - cb->s->bw[2];
		}
		resolve(s, cbw);
		bool L = !AUT(s, LOL), R = !AUT(s, LOR), T = !AUT(s, LOT), B = !AUT(s, LOB);
		float lv = LV(s, LOL, cbw), rv = LV(s, LOR, cbw), tv = LV(s, LOT, cbh), bv = LV(s, LOB, cbh);
		if (c->kind == K_ATOM)
			atom_size_h(C, c, cbw, cbh);
		else {
			bool has;
			float w = spec_w(s, cbw, &has);
			if (!has) {
				if (L && R)
					w = cbw - lv - rv - s->um[1] - s->um[3];
				else
					w = fit_w(C, c, cbw - (L ? lv : 0) - (R ? rv : 0) - s->um[1] - s->um[3]);
			}
			c->w = clamp_w(s, w, cbw);
			s->laid = false;
			layout_box(C, c, cbh, NULL, 0, 0);
			bool hh;
			spec_h(s, cbh, &hh);
			if (!hh && T && B) {
				float h = cbh - tv - bv - s->um[0] - s->um[2];
				if (h > c->h)
					c->h = h;
			}
		}
		x = L ? cbx + lv + s->um[3] : R ? cbx + cbw - rv - s->um[1] - c->w : pax + s->sx + s->um[3];
		y = T ? cby + tv + s->um[0] : B ? cby + cbh - bv - s->um[2] - c->h : pay + s->sy + s->um[0];
		// left y right (o top y bottom) con margin: auto: en medio («inset: 0; margin: auto»)
		if (L && R && AUT(s, LML) && AUT(s, LMR) && cbw - lv - rv - c->w > 0)
			x = cbx + lv + (cbw - lv - rv - c->w) / 2;
		if (T && B && AUT(s, LMT) && AUT(s, LMB) && cbh - tv - bv - c->h > 0)
			y = cby + tv + (cbh - tv - bv - c->h) / 2;
	} else if (s->pos == CSS_POSITION_RELATIVE) {
		float cbw = c->parent ? c->parent->w : C->vw;
		if (!AUT(s, LOL))
			x += LV(s, LOL, cbw);
		else if (!AUT(s, LOR))
			x -= LV(s, LOR, cbw);
		if (!AUT(s, LOT))
			y += LV(s, LOT, -1);
		else if (!AUT(s, LOB))
			y -= LV(s, LOB, -1);
	}
	if (s->tx != 0) // transform: translate()
		x += s->txp ? c->w * s->tx / 100 : s->tx;
	if (s->ty != 0)
		y += s->typ ? c->h * s->ty / 100 : s->ty;
	emit(C, c, x, y, cl, depth + 1);
}

static void emit(Ctx *C, Box *b, float ax, float ay, Clip cl, int depth)
{
	if (C->stop || depth > 400)
		return;
	Sty *s = b->s;
	s->ax = ax, s->ay = ay;
	float x1 = ax + b->w, y1 = ay + b->h;
	if (s->vis && b->kind != K_INLINE && s->tag != T_HTML && s->tag != T_BODY) {
		paint_bg(C, s, ax, ay, x1, y1, cl);
		if (s->bgurl && C->L->bgimg && b->w > 1 && b->h > 1 && ay < cl.y1 && y1 > cl.y0) {
			BrlBg g = {s->bgurl, fmaxf(ax, cl.x0), fmaxf(ay, cl.y0), fminf(x1, cl.x1), fminf(y1, cl.y1), s->bgsz, s->bgsw, s->bgpx, s->bgpy, s->bgpxp, s->bgpyp, s->bgrep};
			// la posición va respecto a la caja entera (aunque se recorte)
			if (!g.pxp)
				g.px += ax - g.x0;
			if (!g.pyp)
				g.py += ay - g.y0;
			BrlText t = {s->fpx, s->col, s->link, s->clk, 0, 0, 0, s->node};
			if (g.x1 > g.x0 && g.y1 > g.y0)
				C->L->bgimg(C->L->u, &g, &t);
		}
	}
	if (y1 > C->bottom && y1 < 200000 && ay > -1000 && s->vis)
		C->bottom = y1;
	if (b->kind == K_ATOM) {
		if (s->vis && ay < cl.y1 && y1 > cl.y0 && ax < cl.x1 && x1 > cl.x0) {
			BrlText t = {s->fpx, s->col, s->link, s->clk, s->bold, 0, s->fit, s->node};
			C->L->box(C->L->u, ax + s->bw[3] + s->up[3], ay + s->bw[0] + s->up[0], b->w - hx(s), b->h - vy(s), s->img, s->wdg, &t);
		}
		return;
	}
	Clip in = cl;
	// lo que no cabe en una caja que recorta (o que se desplazaría: aquí no se puede) no se ve
	bool clipx = s->ovx && s->ovx != CSS_OVERFLOW_VISIBLE, clipy = s->ovy && s->ovy != CSS_OVERFLOW_VISIBLE;
	if (clipx)
		in.x0 = fmaxf(in.x0, ax + s->bw[3]), in.x1 = fminf(in.x1, x1 - s->bw[1]);
	if (clipy)
		in.y0 = fmaxf(in.y0, ay + s->bw[0]), in.y1 = fminf(in.y1, y1 - s->bw[2]);
	float cy = s->cy; /* celdas: el contenido, centrado */
	if (s->li && !s->item)
		marker(C, b, ax, ay + cy, in);
	if (s->ifc) {
		for (int i = 0; i < s->nfr; i++) { // fondos de los en línea, debajo del texto
			LFrag *f = &s->fr[i];
			if (f->t == F_BG && f->b->s->vis)
				paint_bg(C, f->b->s, ax + f->x, ay + cy + f->y, ax + f->x + f->w, ay + cy + f->y + f->h, in);
		}
		for (int i = 0; i < s->nfr; i++) {
			LFrag *f = &s->fr[i];
			if (f->t == F_TEXT) {
				Sty *ts = f->b->s;
				float X = ax + f->x, Y = ay + cy + f->y;
				if (!f->n || !ts->vis || ts->fpx < 1.5f || Y + f->h <= in.y0 || Y >= in.y1 || X >= in.x1 || X + f->w <= in.x0)
					continue;
				if (Y + f->h > C->bottom && Y < 200000)
					C->bottom = Y + f->h;
				BrlText t = {ts->fpx, ts->col, ts->link, ts->clk, ts->bold, ts->under, 0, ts->node};
				C->L->text(C->L->u, X, Y, f->w, f->h, f->s, f->n, &t);
			} else if (f->t == F_BOX || f->t == F_OOF)
				child(C, f->b, ax, ay + cy, in, depth);
		}
		return;
	}
	for (Box *c = b->first; c; c = c->next)
		if (c->kind == K_BLOCK || c->kind == K_ATOM)
			child(C, c, ax, ay + cy, in, depth);
}

#ifdef BRL_DEBUG
static void dump(Box *b, int d, float ax, float ay)
{
	if (d > 40)
		return;
	static const char *K[] = {"blk", "inl", "txt", "atom", "br"};
	char tag[16] = "";
	char cls[48] = "";
	if (b->s->node && b->kind != K_TEXT) {
		tag_of(b->s->node, tag, sizeof tag);
		attr(b->s->node, "class", cls, sizeof cls);
	}
	if (b->kind == K_TEXT)
		fprintf(stderr, "%*s\"%.*s\"\n", d, "", (int)(b->tn < 30 ? b->tn : 30), b->t);
	else
		fprintf(stderr, "%*s%s <%s.%s> in=%d out=%d oof=%d ifc=%d x=%.0f y=%.0f w=%.0f h=%.0f nfr=%d ov=%d,%d aut=%x pct=%x W=%.1f MW=%.1f H=%.1f MH=%.1f m=%.0f,%.0f,%.0f,%.0f p=%.0f,%.0f bg=%08x col=%08x bw=%.0f bc=%08x sc=%d\n", d, "", K[b->kind], tag, cls, b->s->inner, b->s->outer, b->oof,
		        b->s->ifc, ax + b->x, ay + b->y, b->w, b->h, b->s->nfr, b->s->ovx, b->s->ovy, b->s->aut, b->s->pct, b->s->lv[LW], b->s->lv[LMAXW], b->s->lv[LH], b->s->lv[LMINH], b->s->um[0], b->s->um[1], b->s->um[2], b->s->um[3], b->s->up[1], b->s->up[3], b->s->bg, b->s->col, b->s->bw[0], b->s->bc[0], b->s->scope);
	if (b->kind == K_TEXT)
		return;
	for (Box *c = b->first; c; c = c->next)
		dump(c, d + 1, b->kind == K_INLINE ? ax : ax + b->x, b->kind == K_INLINE ? ay : ay + b->y);
}
#endif

/* ------------------------------------------------------------ todo */
float brl_run(BrLay *L, dom_document *doc, uint32_t *page_bg)
{
	Ctx C = {0};
	C.L = L, C.vw = L->vw, C.vh = L->vh > 0 ? L->vh : 768;
	brcss_counters_clear();
	// la raíz: una caja de bloque con el <html> dentro
	Sty *rs = sty_new(&C, NULL);
	Box *root = rs ? box_new(&C, NULL, K_BLOCK, rs) : NULL;
	float total = 0;
	if (root) {
		rs->fpx = 16, rs->lh = 19.2f, rs->vis = 1, rs->col = 0xFF222120, rs->shrink = 1;
		rs->aut = (1u << LW) | (1u << LH) | (1u << LMINW) | (1u << LMAXW) | (1u << LMINH) | (1u << LMAXH) | (1u << LBASIS) | (1u << LOT) | (1u << LOR) |
		          (1u << LOB) | (1u << LOL) | (1u << LTIND) | (1u << LCGAP) | (1u << LRGAP);
		for (dom_node_internal *c = NI(doc)->first_child; c && !C.stop; c = c->next)
			if (c->type == DOM_ELEMENT_NODE)
				build_el(&C, root, (dom_node *)c, NULL, NULL);
		fixup(&C, root);
		if (!C.stop || root->first) {
			bool st = C.stop;
			C.stop = false; // lo que haya, se coloca y se pinta
			root->w = C.vw;
			layout_box(&C, root, C.vh, NULL, 0, 0);
			C.doch = root->h;
			Clip cl = {-1e9f, -1e9f, 1e9f, 1e9f};
			cl.x0 = 0, cl.x1 = C.vw;
			emit(&C, root, 0, 0, cl, 0);
#ifdef BRL_DEBUG
			if (getenv("BRL_DUMP"))
				dump(root, 0, 0, 0);
#endif
			total = fmaxf(root->h, C.bottom);
			C.stop = st;
		}
	}
	if (page_bg)
		*page_bg = C.page_bg;
	arena_free(&C);
	return total;
}
