#include "gfx.h"
#include "util.h"

#include <math.h>
#include <string.h>

Theme T;

static C3D_RenderTarget *top, *bot;
static C2D_TextBuf tbuf;
static bool cur_top;
static float line_feed = 30.0f;

#define RGBA(r, g, b, a) C2D_Color32(r, g, b, a)

void gfx_set_theme(bool light)
{
	if (light) {
		T.bg = RGBA(0xF2, 0xF5, 0xFA, 0xFF);
		T.card = RGBA(0xFF, 0xFF, 0xFF, 0xFF);
		T.card2 = RGBA(0xE7, 0xEC, 0xF4, 0xFF);
		T.line = RGBA(0xD9, 0xE0, 0xEA, 0xFF);
		T.text = RGBA(0x16, 0x1C, 0x2A, 0xFF);
		T.muted = RGBA(0x66, 0x70, 0x85, 0xFF);
		T.shadow = RGBA(0x10, 0x20, 0x40, 0x18);
	} else {
		T.bg = RGBA(0x0D, 0x12, 0x1C, 0xFF);
		T.card = RGBA(0x18, 0x20, 0x2E, 0xFF);
		T.card2 = RGBA(0x22, 0x2B, 0x3B, 0xFF);
		T.line = RGBA(0x2A, 0x34, 0x46, 0xFF);
		T.text = RGBA(0xEC, 0xF0, 0xF7, 0xFF);
		T.muted = RGBA(0x8B, 0x96, 0xAB, 0xFF);
		T.shadow = RGBA(0x00, 0x00, 0x00, 0x40);
	}
	T.accent = RGBA(0x3B, 0x9C, 0xFF, 0xFF);  /* azul cielo */
	T.accent2 = RGBA(0x7A, 0x5C, 0xFF, 0xFF); /* violeta */
	T.ok = RGBA(0x2E, 0xC4, 0x7A, 0xFF);
	T.warn = RGBA(0xF5, 0xB0, 0x2E, 0xFF);
	T.err = RGBA(0xF0, 0x4A, 0x5A, 0xFF);
	T.on_accent = RGBA(0xFF, 0xFF, 0xFF, 0xFF);
}

void gfx_init(void)
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(12288);
	CFNT_s *f = fontGetSystemFont();
	if (f)
		line_feed = fontGetInfo(f)->lineFeed;
	gfx_set_theme(false);
}

void gfx_exit(void)
{
	C2D_TextBufDelete(tbuf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
}

void gfx_frame_begin(void)
{
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void gfx_frame_end(void)
{
	C3D_FrameEnd(0);
}

void gfx_target_top(void)
{
	C2D_TargetClear(top, T.bg);
	C2D_SceneBegin(top);
	cur_top = true;
}

void gfx_target_bot(void)
{
	C2D_TargetClear(bot, T.bg);
	C2D_SceneBegin(bot);
	cur_top = false;
}

bool gfx_on_top(void)
{
	return cur_top;
}

u32 gfx_alpha(u32 c, float a)
{
	u32 al = (c >> 24) & 0xFF;
	al = (u32)(al * clampf(a, 0, 1) + 0.5f);
	return (c & 0x00FFFFFF) | (al << 24);
}

u32 gfx_mix(u32 a, u32 b, float t)
{
	t = clampf(t, 0, 1);
	u32 o = 0;
	for (int s = 0; s < 32; s += 8) {
		float x = ((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t;
		o |= ((u32)(x + 0.5f) & 0xFF) << s;
	}
	return o;
}

void gfx_rect(float x, float y, float w, float h, u32 c)
{
	if (w <= 0 || h <= 0)
		return;
	C2D_DrawRectSolid(x, y, 0, w, h, c);
}

void gfx_circle(float x, float y, float r, u32 c)
{
	C2D_DrawCircleSolid(x, y, 0, r, c);
}

void gfx_rrect(float x, float y, float w, float h, float r, u32 c)
{
	if (w <= 0 || h <= 0)
		return;
	if (r > h / 2)
		r = h / 2;
	if (r > w / 2)
		r = w / 2;
	if (r < 1) {
		gfx_rect(x, y, w, h, c);
		return;
	}
	/* sin solapes para que el alfa se vea uniforme */
	gfx_rect(x + r, y, w - 2 * r, h, c);
	gfx_rect(x, y + r, r, h - 2 * r, c);
	gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
	/* esquinas como abanicos de triángulos */
	const int N = 6;
	float cx[4] = {x + r, x + w - r, x + w - r, x + r};
	float cy[4] = {y + r, y + r, y + h - r, y + h - r};
	float a0[4] = {M_PI, 1.5f * M_PI, 0, 0.5f * M_PI};
	for (int k = 0; k < 4; k++) {
		for (int i = 0; i < N; i++) {
			float t1 = a0[k] + (0.5f * M_PI) * i / N;
			float t2 = a0[k] + (0.5f * M_PI) * (i + 1) / N;
			C2D_DrawTriangle(cx[k], cy[k], c, cx[k] + cosf(t1) * r, cy[k] + sinf(t1) * r, c,
			                 cx[k] + cosf(t2) * r, cy[k] + sinf(t2) * r, c, 0);
		}
	}
}

void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c)
{
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0);
}

void gfx_line(float x1, float y1, float x2, float y2, float th, u32 c)
{
	C2D_DrawLine(x1, y1, c, x2, y2, c, th, 0);
}

void gfx_grad_h(float x, float y, float w, float h, u32 c1, u32 c2)
{
	if (w <= 0 || h <= 0)
		return;
	C2D_DrawRectangle(x, y, 0, w, h, c1, c2, c1, c2);
}

void gfx_brand_bar(float x, float y, float w, float h, float frac)
{
	float r = h / 2;
	gfx_rrect(x, y, w, h, r, T.card2);
	frac = clampf(frac, 0, 1);
	if (frac <= 0)
		return;
	/* el degradado es fijo sobre toda la barra; el recorte lo «descubre» */
	float fw = w * frac;
	if (fw < h)
		fw = h;
	gfx_scissor(x, y, fw, h);
	gfx_circle(x + r, y + r, r, T.accent);
	gfx_grad_h(x + r, y, w - 2 * r, h, T.accent, T.accent2);
	gfx_circle(x + w - r, y + r, r, T.accent2);
	gfx_scissor_off();
	gfx_circle(x + fw - r, y + r, r * 0.45f, gfx_alpha(T.on_accent, 0.9f));
}

/* ---------- texto ---------- */

typedef struct {
	u32 h;
	u16 size10, maxw;
	short cut; /* bytes que caben (-1 = cabe entero) */
	float w;
} TCache;

static TCache tc[1024];

static u32 hash_s(const char *s, u32 h)
{
	while (*s)
		h = (h ^ (u8)*s++) * 16777619u;
	return h;
}

static float sc_of(float size)
{
	return size / line_feed * 1.12f;
}

static float raw_w(const char *s, float size)
{
	C2D_Text t;
	C2D_TextParse(&t, tbuf, s);
	float w, h;
	C2D_TextGetDimensions(&t, sc_of(size), sc_of(size), &w, &h);
	return w;
}

static int prev_cp(const char *s, int i)
{
	i--;
	while (i > 0 && ((u8)s[i] & 0xC0) == 0x80)
		i--;
	return i;
}

static TCache *measure(const char *s, float size, float maxw)
{
	u32 h = hash_s(s, 2166136261u);
	u16 s10 = (u16)(size * 10), mw = (u16)(maxw > 0 ? maxw : 0);
	h = (h ^ s10) * 16777619u;
	h = (h ^ mw) * 16777619u;
	TCache *e = &tc[h & 1023];
	if (e->h == h && e->size10 == s10 && e->maxw == mw)
		return e;
	e->h = h;
	e->size10 = s10;
	e->maxw = mw;
	e->cut = -1;
	e->w = raw_w(s, size);
	if (maxw > 0 && e->w > maxw) {
		/* busca el corte más largo que quepa con «…» */
		int n = strlen(s);
		char tmp[512];
		int lo = 0, hi = n < 500 ? n : 500, best = 0;
		float bw = 0;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			int cut = mid;
			while (cut > 0 && ((u8)s[cut] & 0xC0) == 0x80)
				cut--;
			memcpy(tmp, s, cut);
			strcpy(tmp + cut, "…");
			float w = raw_w(tmp, size);
			if (w <= maxw) {
				best = cut;
				bw = w;
				lo = mid + 1;
			} else {
				hi = cut - 1;
			}
		}
		/* sin espacio colgando antes de los puntos */
		while (best > 0 && s[best - 1] == ' ')
			best = prev_cp(s, best);
		e->cut = best;
		e->w = bw;
	}
	return e;
}

float gfx_text_w(float size, const char *s)
{
	if (!s || !*s)
		return 0;
	return measure(s, size, 0)->w;
}

float gfx_text(float x, float y, float size, u32 color, int align, float maxw, const char *s)
{
	if (!s || !*s)
		return 0;
	TCache *m = measure(s, size, maxw);
	char tmp[512];
	const char *draw = s;
	if (m->cut >= 0) {
		memcpy(tmp, s, m->cut);
		strcpy(tmp + m->cut, "…");
		draw = tmp;
	}
	float w = m->w;
	if (align == AL_CENTER)
		x -= w / 2;
	else if (align == AL_RIGHT)
		x -= w;
	C2D_Text t;
	C2D_TextParse(&t, tbuf, draw);
	float sc = sc_of(size);
	C2D_DrawText(&t, C2D_WithColor, roundf(x), roundf(y - size * 0.08f), 0, sc, sc, color);
	return w;
}

float gfx_title(float x, float y, float size, u32 color)
{
	/* la fuente deja demasiado aire entre «3» y «D» */
	float w1 = gfx_text(x, y, size, color, AL_LEFT, 0, "Cloud3");
	float w2 = gfx_text(x + w1 - size * 0.08f, y, size, color, AL_LEFT, 0, "DS");
	return w1 + w2 - size * 0.08f;
}

static bool is_cjk(const u8 *p)
{
	/* U+3000 en adelante (CJK, kana, puntuación japonesa) */
	return p[0] >= 0xE3 && p[0] <= 0xE9;
}

float gfx_wrap(float x, float y, float size, u32 color, int align, float w, const char *s, int max_lines)
{
	char line[512], cand[512];
	int ln = 0, lines = 0;
	float lh = size * 1.3f;
	const char *p = s;
	line[0] = 0;
	while (*p) {
		/* siguiente token: palabra con su espacio, o un carácter CJK */
		const char *q = p;
		if (*q == '\n') {
			gfx_text(align == AL_CENTER ? x + w / 2 : x, y + lines * lh, size, color, align, w, line);
			lines++;
			ln = 0;
			line[0] = 0;
			p++;
			continue;
		}
		if (is_cjk((const u8 *)q)) {
			q += 3;
			/* no empezar línea con puntuación de cierre */
			while (*q && (strncmp(q, "。", 3) == 0 || strncmp(q, "、", 3) == 0 || strncmp(q, "」", 3) == 0 ||
			              strncmp(q, "！", 3) == 0 || strncmp(q, "？", 3) == 0))
				q += 3;
		} else {
			while (*q && *q != ' ' && *q != '\n' && !is_cjk((const u8 *)q))
				q++;
			while (*q == ' ')
				q++;
		}
		int tl = q - p;
		if (ln + tl >= (int)sizeof cand - 4)
			break;
		memcpy(cand, line, ln);
		memcpy(cand + ln, p, tl);
		cand[ln + tl] = 0;
		/* el ancho se mide sin el espacio final */
		char meas[512];
		strcpy(meas, cand);
		int ml = strlen(meas);
		while (ml && meas[ml - 1] == ' ')
			meas[--ml] = 0;
		if (ln > 0 && gfx_text_w(size, meas) > w) {
			if (max_lines && lines + 1 >= max_lines) {
				/* última línea permitida: recorta con «…» */
				strcpy(line + ln, p);
				gfx_text(align == AL_CENTER ? x + w / 2 : x, y + lines * lh, size, color, align, w, line);
				return (lines + 1) * lh;
			}
			while (ln && line[ln - 1] == ' ')
				line[--ln] = 0;
			gfx_text(align == AL_CENTER ? x + w / 2 : x, y + lines * lh, size, color, align, 0, line);
			lines++;
			memcpy(line, p, tl);
			ln = tl;
			line[ln] = 0;
		} else {
			memcpy(line, cand, ln + tl + 1);
			ln += tl;
		}
		p = q;
	}
	if (ln) {
		while (ln && line[ln - 1] == ' ')
			line[--ln] = 0;
		gfx_text(align == AL_CENTER ? x + w / 2 : x, y + lines * lh, size, color, align, w, line);
		lines++;
	}
	return lines * lh;
}

/* ---------- recorte ---------- */

void gfx_scissor(float x, float y, float w, float h)
{
	C2D_Flush();
	int W = cur_top ? TOP_W : BOT_W;
	int l = 240 - (int)(y + h), t = W - (int)(x + w), r = 240 - (int)y, b = W - (int)x;
	if (l < 0) l = 0;
	if (t < 0) t = 0;
	if (r > 240) r = 240;
	if (b > W) b = W;
	if (r < l) r = l;
	if (b < t) b = t;
	C3D_SetScissor(GPU_SCISSOR_NORMAL, l, t, r, b);
}

void gfx_scissor_off(void)
{
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

/* ---------- íconos vectoriales (cx, cy = centro; s = tamaño) ---------- */

static void ring(float cx, float cy, float r, float th, u32 c, u32 bg)
{
	gfx_circle(cx, cy, r, c);
	gfx_circle(cx, cy, r - th, bg);
}

static void arc(float cx, float cy, float r, float a0, float a1, float th, u32 c)
{
	const int N = 10;
	for (int i = 0; i < N; i++) {
		float t1 = a0 + (a1 - a0) * i / N, t2 = a0 + (a1 - a0) * (i + 1) / N;
		gfx_line(cx + cosf(t1) * r, cy + sinf(t1) * r, cx + cosf(t2) * r, cy + sinf(t2) * r, th, c);
	}
	gfx_circle(cx + cosf(a0) * r, cy + sinf(a0) * r, th / 2, c);
	gfx_circle(cx + cosf(a1) * r, cy + sinf(a1) * r, th / 2, c);
}

static void arrow(float cx, float cy, float s, u32 c, bool up)
{
	float d = up ? -1 : 1;
	float th = s * 0.14f;
	gfx_rect(cx - th / 2, cy - s * 0.32f, th, s * 0.64f, c);
	gfx_tri(cx, cy + d * s * 0.42f, cx - s * 0.30f, cy + d * s * 0.08f, cx + s * 0.30f, cy + d * s * 0.08f, c);
	gfx_rect(cx - s * 0.38f, cy - d * s * 0.42f - (up ? 0 : th), s * 0.76f, th, c);
}

void gfx_icon(Icon ic, float cx, float cy, float s, u32 c)
{
	float th = s * 0.12f;
	if (th < 1.5f)
		th = 1.5f;
	switch (ic) {
	case IC_CLOUD:
		gfx_circle(cx - s * 0.18f, cy + s * 0.02f, s * 0.22f, c);
		gfx_circle(cx + s * 0.08f, cy - s * 0.08f, s * 0.28f, c);
		gfx_circle(cx + s * 0.28f, cy + s * 0.08f, s * 0.17f, c);
		gfx_rrect(cx - s * 0.40f, cy + s * 0.02f, s * 0.85f, s * 0.24f, s * 0.12f, c);
		break;
	case IC_WIFI_OFF:
		for (int i = 0; i < 3; i++)
			arc(cx, cy + s * 0.30f, s * (0.18f + 0.17f * i), -2.36f, -0.78f, th, c);
		gfx_circle(cx, cy + s * 0.30f, th * 0.9f, c);
		gfx_line(cx - s * 0.40f, cy - s * 0.40f, cx + s * 0.40f, cy + s * 0.40f, th * 1.2f, T.err);
		break;
	case IC_UP:
	case IC_DOWN:
		arrow(cx, cy, s, c, ic == IC_UP);
		break;
	case IC_FOLDER:
		gfx_rrect(cx - s * 0.42f, cy - s * 0.30f, s * 0.38f, s * 0.2f, s * 0.05f, c);
		gfx_rrect(cx - s * 0.42f, cy - s * 0.20f, s * 0.84f, s * 0.54f, s * 0.07f, c);
		break;
	case IC_TRASH:
		gfx_rect(cx - s * 0.36f, cy - s * 0.30f, s * 0.72f, th, c);
		gfx_rect(cx - s * 0.12f, cy - s * 0.40f, s * 0.24f, th, c);
		gfx_rrect(cx - s * 0.27f, cy - s * 0.18f, s * 0.54f, s * 0.58f, s * 0.06f, c);
		break;
	case IC_PENCIL:
		gfx_line(cx - s * 0.28f, cy + s * 0.28f, cx + s * 0.26f, cy - s * 0.26f, s * 0.2f, c);
		gfx_tri(cx - s * 0.40f, cy + s * 0.40f, cx - s * 0.36f, cy + s * 0.16f, cx - s * 0.16f, cy + s * 0.36f, c);
		break;
	case IC_LINK:
		ring(cx - s * 0.16f, cy + s * 0.04f, s * 0.24f, th, c, T.card);
		ring(cx + s * 0.16f, cy - s * 0.04f, s * 0.24f, th, c, T.card);
		break;
	case IC_PHONE:
		gfx_rrect(cx - s * 0.24f, cy - s * 0.42f, s * 0.48f, s * 0.84f, s * 0.09f, c);
		gfx_rect(cx - s * 0.17f, cy - s * 0.30f, s * 0.34f, s * 0.56f, T.card);
		gfx_circle(cx, cy + s * 0.34f, s * 0.04f, T.card);
		break;
	case IC_TABLET:
		gfx_rrect(cx - s * 0.34f, cy - s * 0.42f, s * 0.68f, s * 0.84f, s * 0.08f, c);
		gfx_rect(cx - s * 0.27f, cy - s * 0.33f, s * 0.54f, s * 0.62f, T.card);
		break;
	case IC_LAPTOP:
		gfx_rrect(cx - s * 0.34f, cy - s * 0.32f, s * 0.68f, s * 0.48f, s * 0.05f, c);
		gfx_rect(cx - s * 0.27f, cy - s * 0.25f, s * 0.54f, s * 0.34f, T.card);
		gfx_rrect(cx - s * 0.46f, cy + s * 0.18f, s * 0.92f, s * 0.12f, s * 0.05f, c);
		break;
	case IC_GEAR:
		for (int i = 0; i < 8; i++) {
			float a = i * M_PI / 4;
			gfx_line(cx, cy, cx + cosf(a) * s * 0.42f, cy + sinf(a) * s * 0.42f, s * 0.18f, c);
		}
		ring(cx, cy, s * 0.32f, s * 0.32f - s * 0.12f, c, c);
		gfx_circle(cx, cy, s * 0.12f, T.card);
		break;
	case IC_LIST:
		for (int i = 0; i < 3; i++) {
			float yy = cy - s * 0.28f + i * s * 0.28f;
			gfx_circle(cx - s * 0.32f, yy, s * 0.07f, c);
			gfx_rrect(cx - s * 0.18f, yy - th / 2, s * 0.56f, th, th / 2, c);
		}
		break;
	case IC_DEVICES:
		gfx_rrect(cx - s * 0.44f, cy - s * 0.32f, s * 0.60f, s * 0.44f, s * 0.05f, c);
		gfx_rect(cx - s * 0.38f, cy - s * 0.26f, s * 0.48f, s * 0.32f, T.card);
		gfx_rect(cx - s * 0.24f, cy + s * 0.14f, s * 0.20f, s * 0.08f, c);
		gfx_rrect(cx + s * 0.12f, cy - s * 0.10f, s * 0.32f, s * 0.52f, s * 0.06f, c);
		gfx_rect(cx + s * 0.17f, cy - s * 0.03f, s * 0.22f, s * 0.36f, T.card);
		break;
	case IC_LOCK:
		arc(cx, cy - s * 0.08f, s * 0.20f, M_PI, 2 * M_PI, th, c);
		gfx_line(cx - s * 0.20f, cy - s * 0.08f, cx - s * 0.20f, cy + s * 0.02f, th, c);
		gfx_line(cx + s * 0.20f, cy - s * 0.08f, cx + s * 0.20f, cy + s * 0.02f, th, c);
		gfx_rrect(cx - s * 0.32f, cy, s * 0.64f, s * 0.42f, s * 0.07f, c);
		break;
	case IC_CHECK:
		gfx_line(cx - s * 0.34f, cy + s * 0.02f, cx - s * 0.10f, cy + s * 0.26f, th * 1.4f, c);
		gfx_line(cx - s * 0.10f, cy + s * 0.26f, cx + s * 0.36f, cy - s * 0.24f, th * 1.4f, c);
		gfx_circle(cx - s * 0.10f, cy + s * 0.26f, th * 0.7f, c);
		break;
	case IC_CROSS:
		gfx_line(cx - s * 0.30f, cy - s * 0.30f, cx + s * 0.30f, cy + s * 0.30f, th * 1.3f, c);
		gfx_line(cx - s * 0.30f, cy + s * 0.30f, cx + s * 0.30f, cy - s * 0.30f, th * 1.3f, c);
		break;
	case IC_ALERT:
		gfx_tri(cx, cy - s * 0.42f, cx - s * 0.46f, cy + s * 0.38f, cx + s * 0.46f, cy + s * 0.38f, c);
		gfx_rect(cx - th / 2, cy - s * 0.14f, th, s * 0.30f, T.card);
		gfx_circle(cx, cy + s * 0.25f, th * 0.6f, T.card);
		break;
	case IC_INFO:
		gfx_circle(cx, cy, s * 0.44f, c);
		gfx_rect(cx - th / 2, cy - s * 0.06f, th, s * 0.30f, T.card);
		gfx_circle(cx, cy - s * 0.20f, th * 0.65f, T.card);
		break;
	case IC_KEY:
		ring(cx - s * 0.20f, cy, s * 0.20f, th * 1.1f, c, T.card);
		gfx_rect(cx - s * 0.02f, cy - th / 2, s * 0.44f, th, c);
		gfx_rect(cx + s * 0.24f, cy, th, s * 0.16f, c);
		gfx_rect(cx + s * 0.36f, cy, th, s * 0.12f, c);
		break;
	case IC_GLOBE:
		ring(cx, cy, s * 0.42f, th, c, T.card);
		gfx_rect(cx - s * 0.40f, cy - th / 2, s * 0.80f, th, c);
		gfx_rect(cx - th / 2, cy - s * 0.40f, th, s * 0.80f, c);
		arc(cx, cy, s * 0.42f, -1.2f, 1.2f, th * 0.8f, c);
		break;
	case IC_MOON:
		gfx_circle(cx, cy, s * 0.38f, c);
		gfx_circle(cx + s * 0.18f, cy - s * 0.14f, s * 0.32f, T.card);
		break;
	case IC_HEART:
		gfx_circle(cx - s * 0.17f, cy - s * 0.10f, s * 0.20f, c);
		gfx_circle(cx + s * 0.17f, cy - s * 0.10f, s * 0.20f, c);
		gfx_tri(cx - s * 0.36f, cy - s * 0.02f, cx + s * 0.36f, cy - s * 0.02f, cx, cy + s * 0.38f, c);
		break;
	case IC_POWER:
		arc(cx, cy + s * 0.04f, s * 0.32f, -1.1f, 4.24f, th, c);
		gfx_rect(cx - th / 2, cy - s * 0.42f, th, s * 0.40f, c);
		break;
	}
}

/* ---------- animación ---------- */

float clampf(float v, float a, float b)
{
	return v < a ? a : v > b ? b : v;
}

float ease_out_cubic(float t)
{
	t = clampf(t, 0, 1) - 1;
	return t * t * t + 1;
}

float ease_in_out(float t)
{
	t = clampf(t, 0, 1);
	return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}

float ease_out_back(float t)
{
	t = clampf(t, 0, 1);
	const float c1 = 1.70158f, c3 = c1 + 1;
	return 1 + c3 * powf(t - 1, 3) + c1 * powf(t - 1, 2);
}

float approach(float cur, float target, float speed, float dt)
{
	float k = 1 - expf(-speed * dt);
	float v = cur + (target - cur) * k;
	if (fabsf(target - v) < 0.01f)
		v = target;
	return v;
}
