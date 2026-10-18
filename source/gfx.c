/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - motor gráfico
#include "gfx.h"
#include "i18n.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

Theme T;
C3D_RenderTarget *g_top, *g_bot;
float g_time = 0, g_dt = 1.0f / 60.0f;

static C2D_TextBuf s_tbuf;
static float s_cw[0x800];        // cache de anchos (escala 1) para BMP básico
static bool  s_cw_ok[0x800];
// caché para el resto de caracteres (kanji, kana…): buscar un glifo en la fuente del sistema es lento
typedef struct { u32 cp; float w; signed char miss; bool wok; } GlyphC;
static GlyphC s_gc[4096];
static GlyphC *gcache(u32 cp) {
    unsigned h = (cp * 2654435761u) >> 20;           // 12 bits
    for (int i = 0; i < 16; i++) {
        GlyphC *g = &s_gc[(h + i) & 4095];
        if (g->cp == cp) return g;
        if (g->cp == 0) { g->cp = cp; g->miss = -1; g->wok = false; return g; }
    }
    GlyphC *g = &s_gc[h & 4095];                     // tabla llena en esa zona: se reutiliza
    g->cp = cp; g->miss = -1; g->wok = false;
    return g;
}
static float s_lineFeed = 30.0f;
static u64   s_last_ms;

#define Z 0.5f

static C2D_SpriteSheet s_sprites;   // logo, resplandor y halo (romfs:/gfx/sprites.t3x)
static void sprites_load(void);

// colores propios de Cloud3DS (azul marino, tarjetas, azul cielo y violeta)
void theme_set(bool dark) {
    T.dark = dark;
    if (dark) {
        T.bg = col_hex(0x0D121C);       T.bg2 = col_hex(0x131A27);
        T.surface = col_hex(0x18202E);  T.surface2 = col_hex(0x222B3B);
        T.pill = col_hex(0x18202E);     T.pill_hi = col_hex(0x2A3446);
        T.text = col_hex(0xECF0F7);     T.text2 = col_hex(0xB9C2D3);  T.text3 = col_hex(0x8B96AB);
        T.divider = col_hex(0x2A3446);  T.dim = RGBA(0, 0, 0, 140);   T.shadow = RGBA(0, 0, 0, 70);
    } else {
        T.bg = col_hex(0xF2F5FA);       T.bg2 = col_hex(0xE9EEF6);
        T.surface = col_hex(0xFFFFFF);  T.surface2 = col_hex(0xE7ECF4);
        T.pill = col_hex(0xFFFFFF);     T.pill_hi = col_hex(0xD9E0EA);
        T.text = col_hex(0x161C2A);     T.text2 = col_hex(0x3A4356);  T.text3 = col_hex(0x667085);
        T.divider = col_hex(0xD9E0EA);  T.dim = RGBA(10, 20, 40, 90); T.shadow = RGBA(16, 32, 64, 28);
    }
    T.accent = col_hex(0x3B9CFF);  T.accent2 = col_hex(0x7A5CFF);
    T.accent_soft = RGBA(0x3B, 0x9C, 0xFF, dark ? 46 : 36);
    T.ok = col_hex(0x2EC47A);      T.warn = col_hex(0xF5B02E);    T.danger = col_hex(0xF04A5A);
    T.link = T.accent;             T.quote_bar = T.divider;
    T.code_bg = T.bg2;             T.code_text = T.text;          T.user_bubble = T.surface2; T.user_text = T.text;
}

void gfx_init(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    C2D_Init(12000);
    C2D_Prepare();
    g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    fontEnsureMapped();
    s_tbuf = C2D_TextBufNew(9000);
    FINF_s *fi = C2D_FontGetInfo(NULL);
    if (fi) s_lineFeed = fi->lineFeed;
    s_last_ms = osGetTime();
    theme_set(true);
    sprites_load();
#ifdef G3_FONTDBG
    { void font_debug(void); font_debug(); }
#endif
}

void gfx_exit(void) {
    if (s_sprites) C2D_SpriteSheetFree(s_sprites);
    C2D_TextBufDelete(s_tbuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void gfx_tick(void) {
    u64 now = osGetTime();
    float dt = (now - s_last_ms) / 1000.0f;
    s_last_ms = now;
    if (dt <= 0) dt = 1.0f / 60.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_dt = dt;
    g_time += dt;
}

void gfx_frame_begin(void) {
    C2D_TextBufClear(s_tbuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void gfx_frame_end(void) { C3D_FrameEnd(0); }

static C3D_RenderTarget *s_cur;

void gfx_target(C3D_RenderTarget *t, u32 clear) {
    C2D_TargetClear(t, clear);
    C2D_SceneBegin(t);
    s_cur = t;
}

// recorte rectangular (coordenadas de pantalla; el framebuffer de la 3DS está girado 90°)
void gfx_clip(float x, float y, float w, float h) {
    C2D_Flush();
    int sw = s_cur == g_top ? TOP_W : BOT_W;
    int l = 240 - (int)(y + h), t = sw - (int)(x + w), r = 240 - (int)y, b = sw - (int)x;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > 240) r = 240;
    if (b > sw) b = sw;
    if (r < l) r = l;
    if (b < t) b = t;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, l, t, r, b);
}

void gfx_clip_off(void) {
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// ---------------------------------------------------------------- color
u32 col_alpha(u32 c, float a) {
    a = clampf(a, 0, 1);
    u32 al = (u32)(((c >> 24) & 0xFF) * a);
    return (c & 0x00FFFFFF) | (al << 24);
}
u32 col_lerp(u32 a, u32 b, float t) {
    t = clampf(t, 0, 1);
    u32 r = 0;
    for (int i = 0; i < 32; i += 8) {
        float ca = (a >> i) & 0xFF, cb = (b >> i) & 0xFF;
        r |= ((u32)(ca + (cb - ca) * t) & 0xFF) << i;
    }
    return r;
}
u32 col_hex(u32 rgb) { return C2D_Color32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF); }

u32 brand_grad(float t) {
    // azul cielo -> violeta -> azul claro -> azul cielo (cíclico)
    static const u32 stops[4] = {0x3B9CFF, 0x7A5CFF, 0x5AB8FF, 0x3B9CFF};
    t = t - floorf(t);
    float f = t * 3.0f;
    int i = (int)f; if (i > 2) i = 2;
    return col_lerp(col_hex(stops[i]), col_hex(stops[i + 1]), f - i);
}

// ---------------------------------------------------------------- formas
void draw_rect(float x, float y, float w, float h, u32 c) {
    if (w <= 0 || h <= 0) return;
    C2D_DrawRectSolid(x, y, Z, w, h, c);
}

static void corner_fan(float cx, float cy, float r, float a0, u32 c) {
    const int N = 6;
    float step = (float)M_PI_2 / N;
    float px = cx + cosf(a0) * r, py = cy + sinf(a0) * r;
    for (int i = 1; i <= N; i++) {
        float a = a0 + step * i;
        float nx = cx + cosf(a) * r, ny = cy + sinf(a) * r;
        C2D_DrawTriangle(cx, cy, c, px, py, c, nx, ny, c, Z);
        px = nx; py = ny;
    }
}

void draw_rrect(float x, float y, float w, float h, float r, u32 c) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) { draw_rect(x, y, w, h, c); return; }
    C2D_DrawRectSolid(x + r, y, Z, w - 2 * r, h, c);
    C2D_DrawRectSolid(x, y + r, Z, r, h - 2 * r, c);
    C2D_DrawRectSolid(x + w - r, y + r, Z, r, h - 2 * r, c);
    corner_fan(x + r, y + r, r, (float)M_PI, c);
    corner_fan(x + w - r, y + r, r, (float)(M_PI * 1.5), c);
    corner_fan(x + w - r, y + h - r, r, 0, c);
    corner_fan(x + r, y + h - r, r, (float)M_PI_2, c);
}

void draw_rrect_border(float x, float y, float w, float h, float r, float bw, u32 border, u32 fill) {
    draw_rrect(x, y, w, h, r, border);
    draw_rrect(x + bw, y + bw, w - 2 * bw, h - 2 * bw, r - bw > 0 ? r - bw : 0, fill);
}

void draw_rrect_outline(float x, float y, float w, float h, float r, float th, u32 c) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    float o = th / 2;
    draw_line(x + r, y + o, x + w - r, y + o, th, c);
    draw_line(x + r, y + h - o, x + w - r, y + h - o, th, c);
    draw_line(x + o, y + r, x + o, y + h - r, th, c);
    draw_line(x + w - o, y + r, x + w - o, y + h - r, th, c);
    if (r > o) {
        draw_arc(x + r, y + r, r - o, th, (float)M_PI, (float)(M_PI * 1.5), c);
        draw_arc(x + w - r, y + r, r - o, th, (float)(M_PI * 1.5), (float)(M_PI * 2), c);
        draw_arc(x + w - r, y + h - r, r - o, th, 0, (float)M_PI_2, c);
        draw_arc(x + r, y + h - r, r - o, th, (float)M_PI_2, (float)M_PI, c);
    }
}

void draw_grad_h(float x, float y, float w, float h, u32 c0, u32 c1) {
    C2D_DrawRectangle(x, y, Z, w, h, c0, c1, c0, c1);
}
void draw_grad_v(float x, float y, float w, float h, u32 c0, u32 c1) {
    C2D_DrawRectangle(x, y, Z, w, h, c0, c0, c1, c1);
}
void draw_grad4(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br) {
    C2D_DrawRectangle(x, y, Z, w, h, tl, tr, bl, br);
}

void draw_circle(float x, float y, float r, u32 c) { C2D_DrawCircleSolid(x, y, Z, r, c); }

void draw_arc(float x, float y, float r, float th, float a0, float a1, u32 c) {
    int n = (int)(fabsf(a1 - a0) * r / 3.0f) + 4;
    if (n > 48) n = 48;
    float ri = r - th / 2, ro = r + th / 2;
    for (int i = 0; i < n; i++) {
        float t0 = a0 + (a1 - a0) * i / n, t1 = a0 + (a1 - a0) * (i + 1) / n;
        float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
        C2D_DrawTriangle(x + c0 * ri, y + s0 * ri, c, x + c0 * ro, y + s0 * ro, c, x + c1 * ro, y + s1 * ro, c, Z);
        C2D_DrawTriangle(x + c0 * ri, y + s0 * ri, c, x + c1 * ro, y + s1 * ro, c, x + c1 * ri, y + s1 * ri, c, Z);
    }
}
void draw_ring(float x, float y, float r, float th, u32 c) { draw_arc(x, y, r, th, 0, (float)(2 * M_PI), c); }

void draw_line(float x0, float y0, float x1, float y1, float th, u32 c) {
    C2D_DrawLine(x0, y0, c, x1, y1, c, th, Z);
}
void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
    C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, Z);
}

// Destello de 4 puntas (curva tipo astroide), con degradado por vértice
static void sparkle_impl(float cx, float cy, float r, float rot, float alpha, bool grad, u32 mono) {
    const int N = 40;
    float pxp = 0, pyp = 0; u32 pc = 0;
    u32 center = grad ? col_alpha(col_hex(0x9BB8FF), alpha) : mono;
    float cr = cosf(rot), sr = sinf(rot);
    for (int i = 0; i <= N; i++) {
        float t = (float)(2 * M_PI) * i / N;
        float ct = cosf(t), st = sinf(t);
        float x = (ct >= 0 ? 1 : -1) * powf(fabsf(ct), 2.6f);
        float y = (st >= 0 ? 1 : -1) * powf(fabsf(st), 2.6f);
        float px = cx + (x * cr - y * sr) * r;
        float py = cy + (x * sr + y * cr) * r;
        u32 c = grad ? col_alpha(brand_grad((float)i / N * 0.75f + g_time * 0.05f), alpha) : mono;
        if (i > 0) C2D_DrawTriangle(cx, cy, center, pxp, pyp, pc, px, py, c, Z);
        pxp = px; pyp = py; pc = c;
    }
}
void draw_sparkle(float cx, float cy, float r, float rot, float alpha) { sparkle_impl(cx, cy, r, rot, alpha, true, 0); }

// logo de Cloud3DS: imágenes suavizadas generadas en el PC (tools/gen_art.py), no figuras.
// Están centradas en la caja real de la nube, así que (cx, cy) es su centro visual.
enum { SPR_LOGO256, SPR_LOGO128, SPR_LOGO64, SPR_LOGO32, SPR_GLOW, SPR_HALO };

static void sprites_load(void) {
    s_sprites = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
    if (!s_sprites) return;
    C2D_Image im = C2D_SpriteSheetGetImage(s_sprites, 0);
    C3D_TexSetFilter(im.tex, GPU_LINEAR, GPU_LINEAR);   // escalado suave
}

static void sprite_at(int idx, float cx, float cy, float size, float alpha) {
    if (!s_sprites || alpha <= 0.003f || size <= 0.5f) return;
    C2D_Image im = C2D_SpriteSheetGetImage(s_sprites, idx);
    float sc = size / im.subtex->width;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, clampf(alpha, 0, 1));
    C2D_DrawImageAt(im, cx - size / 2, cy - size / 2, Z, &tint, sc, sc);
}

// r: «radio» de la nube; su caja mide 1.7r de ancho y 1.24r de alto
void draw_cloud(float cx, float cy, float r, float alpha) {
    float size = 1.86f * r;                      // la nube ocupa el 91 % del ancho de la imagen
    int idx = size > 128 ? SPR_LOGO256 : size > 64 ? SPR_LOGO128 : size > 32 ? SPR_LOGO64 : SPR_LOGO32;
    sprite_at(idx, cx, cy, size, alpha);
}

void draw_cloud_glow(float cx, float cy, float r, float alpha) { sprite_at(SPR_GLOW, cx, cy, 3.2f * r, alpha); }
void draw_halo(float cx, float cy, float radius, float alpha) { sprite_at(SPR_HALO, cx, cy, 2 * radius, alpha); }
void draw_sparkle_mono(float cx, float cy, float r, float rot, u32 c) { sparkle_impl(cx, cy, r, rot, 1, false, c); }

// barra «cargando» con un brillo que se desplaza
void draw_shimmer_bar(float x, float y, float w, float h, float phase) {
    u32 a = col_alpha(col_hex(0x4796E3), 0.55f), b = col_alpha(col_hex(0x9177C7), 0.25f), c = col_alpha(T.surface2, 0.6f);
    float p = phase - floorf(phase);
    float seg = w / 3;
    float off = p * w;
    // fondo
    draw_rrect(x, y, w, h, h / 2, c);
    // brillo
    float sx = x + off - seg;
    float ex = sx + seg * 2;
    if (sx < x) sx = x;
    if (ex > x + w) ex = x + w;
    if (ex > sx) {
        float mid = (sx + ex) / 2;
        draw_grad_h(sx, y, mid - sx, h, col_alpha(b, 0), a);
        draw_grad_h(mid, y, ex - mid, h, a, col_alpha(b, 0));
    }
}

// ---------------------------------------------------------------- utf8 / texto
int utf8_len(u8 c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

u32 utf8_next(const char **pp) {
    const u8 *p = (const u8 *)*pp;
    u32 c = *p;
    int n = utf8_len(c);
    if (n == 1) { *pp += 1; return c; }
    u32 cp = c & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp += i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pp += n;
    return cp;
}

static float cjk_scale(float s);
// en japonés los kanji van más grandes (ver cjk_scale), así que las líneas necesitan algo más de aire
float text_lineh(float scale) { return s_lineFeed * (g_lang == LANG_JA ? 0.12f * scale + 0.88f * cjk_scale(scale) : scale); }

// glifos que la fuente no tiene (p. ej. «¡» en la fuente de algunos emuladores): no se dibujan
static int s_alter = -1;
static unsigned char s_miss[0x800];          // 0 = sin calcular, 1 = está, 2 = falta
static bool glyph_missing(u32 cp) {
    if (cp < 0x80) return false;
    if (cp < 0x800 && s_miss[cp]) return s_miss[cp] == 2;
    GlyphC *gc = cp >= 0x800 ? gcache(cp) : NULL;
    if (gc && gc->miss >= 0) return gc->miss;
    if (s_alter < 0) { FINF_s *fi = C2D_FontGetInfo(NULL); s_alter = fi ? fi->alterCharIndex : 0xFFFF; }
    int gi = C2D_FontGlyphIndexFromCodePoint(NULL, cp);
    bool miss = gi == s_alter;
    // glifo vacío que no es un espacio (la fuente libre de los emuladores trae «¡» en blanco)
    bool space = cp == 0xA0 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200F) || cp == 0x202F || cp == 0xFEFF;
    if (!miss && !space) { charWidthInfo_s *wi = C2D_FontGetCharWidthInfo(NULL, gi); if (wi && wi->glyphWidth == 0) miss = true; }
    if (cp < 0x800) s_miss[cp] = miss ? 2 : 1;
    else { gc = gcache(cp); gc->miss = miss; }
    return miss;
}

// sustituto para glifos que faltan en algunas fuentes (p. ej. la fuente libre de los emuladores no trae «ん»):
// hiragana -> katakana equivalente; 0 = no hay sustituto (se omite)
static int utf8_encode(u32 cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
static u32 glyph_subst(u32 cp) {
    if (cp >= 0x3041 && cp <= 0x3096 && !glyph_missing(cp + 0x60)) return cp + 0x60;
    return 0;
}

// Japonés/chino: a los tamaños pequeños de la interfaz los kanji no se distinguen, así que
// se dibujan algo más grandes (hasta un 35 %) que el texto latino que los rodea
static inline bool is_cjk(u32 cp) { return cp >= 0x2E80 && cp <= 0xFFEF; }
static float cjk_scale(float s) {
    float b = 0.62f / s;
    if (b < 1) b = 1;
    if (b > 1.35f) b = 1.35f;
    return s * b;
}
static float s_base = -1;
static float cjk_dy(float s) {            // desplazamiento para que compartan la línea base
    if (s_base < 0) { FINF_s *fi = C2D_FontGetInfo(NULL); s_base = (fi && fi->tglp) ? fi->tglp->baselinePos : 22; }
    return (s - cjk_scale(s)) * s_base * 0.55f;   // se reparte el crecimiento arriba y abajo
}

float glyph_w(u32 cp, float scale) {
    if (cp == '\t') return glyph_w(' ', scale) * 4;
    if (is_cjk(cp)) scale = cjk_scale(scale);
    if (cp < 0x800 && s_cw_ok[cp]) return s_cw[cp] * scale;
    if (cp >= 0x800) { GlyphC *g = gcache(cp); if (g->wok) return g->w * scale; }
    int gi = C2D_FontGlyphIndexFromCodePoint(NULL, cp);
    charWidthInfo_s *wi = C2D_FontGetCharWidthInfo(NULL, gi);
    float w = wi ? wi->charWidth : 10;
    if (glyph_missing(cp)) {
        u32 sb = glyph_subst(cp);
        wi = sb ? C2D_FontGetCharWidthInfo(NULL, C2D_FontGlyphIndexFromCodePoint(NULL, sb)) : NULL;
        w = wi ? wi->charWidth : 0;
    }
    if (cp < 0x800) { s_cw[cp] = w; s_cw_ok[cp] = true; }
    else { GlyphC *g = gcache(cp); g->w = w; g->wok = true; }
    return w * scale;
}

// ajuste entre un número y la letra siguiente («Cloud3DS» en vez de «Cloud3 DS»):
// algunas fuentes dan a los dígitos un ancho fijo con mucho aire a la derecha
static float glyph_left(u32 cp) {
    charWidthInfo_s *wi = C2D_FontGetCharWidthInfo(NULL, C2D_FontGlyphIndexFromCodePoint(NULL, cp));
    return wi ? wi->left : 0;
}
static float s_kern[10]; static bool s_kern_ok;
static float kern_pair(u32 a, u32 b, float scale) {
    if (a < '0' || a > '9') return 0;
    if (!((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))) return 0;
    if (!s_kern_ok) {
        for (int d = 0; d < 10; d++) {
            charWidthInfo_s *wi = C2D_FontGetCharWidthInfo(NULL, C2D_FontGlyphIndexFromCodePoint(NULL, '0' + d));
            s_kern[d] = wi ? (float)wi->charWidth - (float)(wi->left + wi->glyphWidth) : 0;
        }
        s_kern_ok = true;
    }
    // espacio visible entre ambos glifos; se deja en ~1.5 px (a escala 1)
    float k = s_kern[a - '0'] + glyph_left(b) - 1.5f;
    return k > 0 ? k * scale : 0;
}

// ¿necesita el texto el dibujado por tramos (glifos que faltan o pares a ajustar)?
static bool text_special(const char *s, int n, float scale) {
    const char *p = s, *e = s + n;
    u32 prev = 0;
    bool boost = cjk_scale(scale) != scale;
    while (p < e && *p) {
        u32 cp = utf8_next(&p);
        if (kern_pair(prev, cp, 1) > 0 || glyph_missing(cp) || (boost && is_cjk(cp))) return true;
        prev = cp;
    }
    return false;
}

float text_width_n(const char *s, int n, float scale) {
    float w = 0;
    const char *p = s, *e = s + n;
    u32 prev = 0;
    while (p < e && *p) {
        u32 cp = utf8_next(&p);
        if (cp == '\n') break;
        w += glyph_w(cp, scale) - kern_pair(prev, cp, scale);
        prev = cp;
    }
    return w;
}
float text_width(const char *s, float scale) { return text_width_n(s, (int)strlen(s), scale); }

static void text_draw_raw(const char *s, float x, float y, float scale, u32 c) {
    C2D_Text t;
    C2D_TextParse(&t, s_tbuf, s);
    C2D_DrawText(&t, C2D_WithColor, x, y, Z, scale, scale, c);
}

void text_draw(const char *s, float x, float y, float scale, u32 c) {
    if (!s || !*s) return;
    int n = (int)strlen(s);
    if (!text_special(s, n, scale)) { text_draw_raw(s, x, y, scale, c); return; }
    // por tramos: se corta en cada ajuste, en cada cambio latino/japonés y se saltan los glifos que faltan
    char seg[512];
    int sl = 0;
    bool segcjk = false;
    float cx = x;
    const char *p = s;
    u32 prev = 0;
    float bs = cjk_scale(scale), dy = cjk_dy(scale);
#define FLUSH() do { if (sl) { seg[sl] = 0; \
        if (segcjk) text_draw_raw(seg, cx, y + dy, bs, c); else text_draw_raw(seg, cx, y, scale, c); \
        cx += text_width_n(seg, sl, scale); sl = 0; } } while (0)
    while (*p) {
        const char *q = p;
        u32 cp = utf8_next(&q);
        float k = kern_pair(prev, cp, scale);
        bool miss = glyph_missing(cp);
        bool cj = is_cjk(cp);
        if (cp == '\n' || k > 0 || miss || sl > 500 || (sl && cj != segcjk)) {
            FLUSH();
            cx -= k;
            if (cp == '\n') { y += text_lineh(scale); cx = x; prev = 0; p = q; continue; }
        }
        if (!sl) segcjk = cj;
        if (!miss) { memcpy(seg + sl, p, q - p); sl += (int)(q - p); }
        else if (glyph_subst(cp)) sl += utf8_encode(glyph_subst(cp), seg + sl);
        prev = cp;
        p = q;
    }
    FLUSH();
#undef FLUSH
}

void text_draw_n(const char *s, int n, float x, float y, float scale, u32 c) {
    char tmp[512];
    if (n <= 0) return;
    if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    text_draw(tmp, x, y, scale, c);
}

void text_draw_center(const char *s, float cx, float y, float scale, u32 c) {
    text_draw(s, cx - text_width(s, scale) / 2, y, scale, c);
}
void text_draw_right(const char *s, float rx, float y, float scale, u32 c) {
    text_draw(s, rx - text_width(s, scale), y, scale, c);
}

void text_draw_clip(const char *s, float x, float y, float maxw, float scale, u32 c) {
    if (text_width(s, scale) <= maxw) { text_draw(s, x, y, scale, c); return; }
    float ell = glyph_w('.', scale) * 3;
    float w = 0;
    const char *p = s, *last = s;
    while (*p) {
        const char *q = p;
        u32 cp = utf8_next(&q);
        if (cp == '\n') break;
        float gw = glyph_w(cp, scale);
        if (w + gw + ell > maxw) break;
        w += gw; p = q; last = p;
    }
    char tmp[512];
    int n = (int)(last - s);
    if (n > 500) n = 500;
    memcpy(tmp, s, n);
    strcpy(tmp + n, "...");
    text_draw(tmp, x, y, scale, c);
}

// texto con degradado animado (cada glifo un color)
void text_draw_grad(const char *s, float x, float y, float scale, float phase, float alpha) {
    float total = text_width(s, scale);
    float cx = x;
    const char *p = s;
    char g[8];
    u32 prev = 0;
    while (*p) {
        const char *q = p;
        u32 cp = utf8_next(&q);
        int n = (int)(q - p);
        memcpy(g, p, n); g[n] = 0;
        if (glyph_missing(cp) && glyph_subst(cp)) g[utf8_encode(glyph_subst(cp), g)] = 0;
        cx -= kern_pair(prev, cp, scale);
        prev = cp;
        float gw = glyph_w(cp, scale);
        float t = total > 0 ? (cx - x) / total : 0;
        u32 c = col_alpha(brand_grad(t * 0.6f + phase), alpha);
        if (cp != ' ' && gw > 0) {
            if (is_cjk(cp)) text_draw_raw(g, cx, y + cjk_dy(scale), cjk_scale(scale), c);
            else text_draw_raw(g, cx, y, scale, c);
        }
        cx += gw;
        p = q;
    }
}

// envoltura simple de palabras (para etiquetas/descripciones); devuelve nº de líneas
int text_wrap_lines(const char *s, float maxw, float scale, int maxlines, float x, float y, u32 c, bool draw) {
    int lines = 0;
    const char *p = s;
    float lh = text_lineh(scale);
    while (*p && lines < maxlines) {
        const char *ls = p, *lastbreak = NULL, *q = p;
        float w = 0;
        while (*q) {
            const char *n = q;
            u32 cp = utf8_next(&n);
            if (cp == '\n') { lastbreak = q; break; }
            float gw = glyph_w(cp, scale);
            if (w + gw > maxw) break;
            if (cp == ' ') lastbreak = q;
            w += gw; q = n;
            if (cp >= 0x2E80) {                        // chino/japonés: corte tras cada carácter (salvo antes de 。、…)
                const char *t = q; u32 nx = *t ? utf8_next(&t) : 0;
                if (!(nx == 0x3001 || nx == 0x3002 || nx == 0xFF01 || nx == 0xFF1F || nx == 0x300D || nx == 0xFF09 || nx == 0x30FC)) lastbreak = q;
            }
        }
        const char *le;
        if (!*q) le = q;
        else if (lastbreak && lastbreak > ls) le = lastbreak;
        else le = q > ls ? q : q + utf8_len(*q);
        if (lines == maxlines - 1 && *le && draw) {
            char tmp[512]; int n = (int)(strlen(ls) < 500 ? strlen(ls) : 500);
            memcpy(tmp, ls, n); tmp[n] = 0;
            for (int i = 0; i < n; i++) if (tmp[i] == '\n') tmp[i] = ' ';
            text_draw_clip(tmp, x, y + lines * lh, maxw, scale, c);
        } else if (draw) text_draw_n(ls, (int)(le - ls), x, y + lines * lh, scale, c);
        lines++;
        p = le;
        while (*p == ' ' || *p == '\n') p++;
    }
    return lines;
}

// ---------------------------------------------------------------- animación
float ease_out_cubic(float t) { t = clampf(t, 0, 1); float u = 1 - t; return 1 - u * u * u; }
float ease_in_out(float t) { t = clampf(t, 0, 1); return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
float ease_out_back(float t) {
    t = clampf(t, 0, 1);
    const float c1 = 1.70158f, c3 = c1 + 1;
    return 1 + c3 * powf(t - 1, 3) + c1 * powf(t - 1, 2);
}
float approach(float v, float target, float speed) {
    float k = 1 - expf(-g_dt * speed);
    float r = v + (target - v) * k;
    if (fabsf(r - target) < 0.01f) r = target;
    return r;
}

// ---------------------------------------------------------------- texturas
static int next_pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static inline u32 morton_offset(u32 x, u32 y) {
    // posición dentro de un tile 8x8 (orden Z)
    static const u8 tbl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    return tbl[x & 7] | (tbl[y & 7] << 1);
}

static void sub_setup(Texture *t, int w, int h, int W, int H) {
    t->sub.width = w; t->sub.height = h;
    t->sub.left = 0; t->sub.top = 1.0f;
    t->sub.right = (float)w / W; t->sub.bottom = 1.0f - (float)h / H;
    t->img.tex = &t->tex; t->img.subtex = &t->sub;
    t->w = w; t->h = h;
}

bool tex_from_rgba(Texture *t, const u8 *rgba, int w, int h) {
    if (w > 1024 || h > 1024 || w <= 0 || h <= 0) return false;
    int W = next_pow2(w), H = next_pow2(h);
    if (t->ok && (t->tex.width != W || t->tex.height != H || t->tex.fmt != GPU_RGBA8)) tex_free(t);
    if (!t->ok) {
        if (!C3D_TexInit(&t->tex, W, H, GPU_RGBA8)) return false;
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        t->ok = true;
    }
    u32 *dst = (u32 *)t->tex.data;
    memset(dst, 0, W * H * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const u8 *s = rgba + (y * w + x) * 4;
            u32 off = ((y >> 3) * (W >> 3) + (x >> 3)) * 64 + morton_offset(x, y);
            dst[off] = ((u32)s[0] << 24) | ((u32)s[1] << 16) | ((u32)s[2] << 8) | s[3];
        }
    }
    C3D_TexFlush(&t->tex);
    sub_setup(t, w, h, W, H);
    return true;
}

bool tex_from_rgb565(Texture *t, const u16 *px, int w, int h) {
    int W = next_pow2(w), H = next_pow2(h);
    if (t->ok && (t->tex.width != W || t->tex.height != H || t->tex.fmt != GPU_RGB565)) tex_free(t);
    if (!t->ok) {
        if (!C3D_TexInit(&t->tex, W, H, GPU_RGB565)) return false;
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
        t->ok = true;
    }
    u16 *dst = (u16 *)t->tex.data;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 off = ((y >> 3) * (W >> 3) + (x >> 3)) * 64 + morton_offset(x, y);
            dst[off] = px[y * w + x];
        }
    C3D_TexFlush(&t->tex);
    sub_setup(t, w, h, W, H);
    return true;
}

void tex_free(Texture *t) {
    if (t->ok) C3D_TexDelete(&t->tex);
    t->ok = false;
}

void draw_texture(Texture *t, float x, float y, float w, float h, float alpha) {
    if (!t->ok) return;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt(t->img, x, y, Z, alpha < 0.999f ? &tint : NULL, w / t->w, h / t->h);
}
#ifdef G3_FONTDBG
void font_debug(void) {
    FILE *f = fopen("sdmc:/fontdbg.txt", "w");
    if (!f) return;
    u32 cps[] = {0xA1, 0xBF, 0xDA, 'M', 0xE1};
    fprintf(f, "alter=%d\n", C2D_FontGetInfo(NULL)->alterCharIndex);
    for (int i = 0; i < 5; i++) { int gi = C2D_FontGlyphIndexFromCodePoint(NULL, cps[i]);
        charWidthInfo_s *wi = C2D_FontGetCharWidthInfo(NULL, gi);
        fprintf(f, "%04lx gi=%d l=%d cw=%d gw=%d miss=%d\n", cps[i], gi, wi->left, wi->charWidth, wi->glyphWidth, glyph_missing(cps[i])); }
    fclose(f);
}
#endif

// ------------------------------------------------------------ caché de glifos de la fuente del sistema
// libctru busca cada carácter recorriendo las tablas de la fuente; para kanji/kana eso es una búsqueda
// lineal de miles de entradas por carácter y por fotograma (el japonés iba a tirones). El enlazador
// redirige aquí todas las llamadas (-Wl,--wrap=fontGlyphIndexFromCodePoint) y se guarda el resultado.
int __real_fontGlyphIndexFromCodePoint(CFNT_s *font, u32 codePoint);
int __wrap_fontGlyphIndexFromCodePoint(CFNT_s *font, u32 codePoint) {
    if (codePoint < 0x80) return __real_fontGlyphIndexFromCodePoint(font, codePoint);
    static struct { CFNT_s *f; u32 cp; int gi; } cache[8192];
    unsigned h = ((codePoint * 2654435761u) ^ (unsigned)(uintptr_t)font) >> 19;   // 13 bits
    for (int i = 0; i < 8; i++) {
        unsigned k = (h + i) & 8191;
        if (cache[k].f == font && cache[k].cp == codePoint) return cache[k].gi;
        if (!cache[k].f) {
            int gi = __real_fontGlyphIndexFromCodePoint(font, codePoint);
            cache[k].f = font; cache[k].cp = codePoint; cache[k].gi = gi;
            return gi;
        }
    }
    int gi = __real_fontGlyphIndexFromCodePoint(font, codePoint);
    cache[h & 8191].f = font; cache[h & 8191].cp = codePoint; cache[h & 8191].gi = gi;
    return gi;
}
