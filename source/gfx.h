/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - motor gráfico (citro2d) : formas, degradados, texto y utilidades
#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

typedef struct {
    u32 bg, bg2, surface, surface2, pill, pill_hi, text, text2, text3;
    u32 accent, accent_soft, divider, code_bg, code_text, user_bubble, user_text;
    u32 danger, ok, link, dim, quote_bar;
    u32 warn, accent2, shadow;
    bool dark;
} Theme;

extern Theme T;
extern C3D_RenderTarget *g_top, *g_bot;
extern float g_time;   // segundos desde el arranque
extern float g_dt;     // delta de frame

void gfx_init(void);
void gfx_exit(void);
void theme_set(bool dark);
void gfx_tick(void);          // calcula g_dt (antes de la lógica)
void gfx_frame_begin(void);   // empieza a dibujar (después de la lógica)
void gfx_frame_end(void);
void gfx_target(C3D_RenderTarget *t, u32 clear);
void gfx_clip(float x, float y, float w, float h);   // solo se dibuja dentro del rectángulo
void gfx_clip_off(void);

// color
static inline u32 RGBA(u8 r, u8 g, u8 b, u8 a) { return C2D_Color32(r, g, b, a); }
u32 col_alpha(u32 c, float a);     // multiplica alfa
u32 col_lerp(u32 a, u32 b, float t);
u32 col_hex(u32 rgb);              // 0xRRGGBB -> opaco

// formas
void draw_rect(float x, float y, float w, float h, u32 c);
void draw_rrect(float x, float y, float w, float h, float r, u32 c);
void draw_rrect_border(float x, float y, float w, float h, float r, float bw, u32 border, u32 fill);
void draw_rrect_outline(float x, float y, float w, float h, float r, float th, u32 c);
void draw_grad_h(float x, float y, float w, float h, u32 c0, u32 c1);
void draw_grad_v(float x, float y, float w, float h, u32 c0, u32 c1);
void draw_grad4(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br);
void draw_circle(float x, float y, float r, u32 c);
void draw_ring(float x, float y, float r, float th, u32 c);
void draw_arc(float x, float y, float r, float th, float a0, float a1, u32 c);
void draw_line(float x0, float y0, float x1, float y1, float th, u32 c);
void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);
void draw_sparkle(float cx, float cy, float r, float rot, float alpha);
void draw_sparkle_mono(float cx, float cy, float r, float rot, u32 c);
void draw_shimmer_bar(float x, float y, float w, float h, float phase);
u32  brand_grad(float t);         // degradado de la marca: azul cielo -> violeta, t en [0,1]
void draw_cloud(float cx, float cy, float r, float alpha);       // logo (imagen suavizada), centrado en (cx, cy)
void draw_cloud_glow(float cx, float cy, float r, float alpha);  // resplandor con la forma del logo
void draw_halo(float cx, float cy, float radius, float alpha);   // halo radial suave

// texto (fuente del sistema)
float text_lineh(float scale);
float glyph_w(u32 cp, float scale);
float text_width(const char *s, float scale);
float text_width_n(const char *s, int n, float scale);
void  text_draw(const char *s, float x, float y, float scale, u32 c);
void  text_draw_n(const char *s, int n, float x, float y, float scale, u32 c);
void  text_draw_center(const char *s, float cx, float y, float scale, u32 c);
void  text_draw_right(const char *s, float rx, float y, float scale, u32 c);
void  text_draw_clip(const char *s, float x, float y, float maxw, float scale, u32 c);
void  text_draw_grad(const char *s, float x, float y, float scale, float phase, float alpha);
int   text_wrap_lines(const char *s, float maxw, float scale, int maxlines, float x, float y, u32 c, bool draw);

// utf8
u32 utf8_next(const char **p);
int utf8_len(u8 c);

// animación
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float ease_out_cubic(float t);
float ease_in_out(float t);
float ease_out_back(float t);
float approach(float v, float target, float speed);   // suavizado exponencial

// texturas
typedef struct { C3D_Tex tex; Tex3DS_SubTexture sub; C2D_Image img; int w, h; bool ok; } Texture;
bool tex_from_rgba(Texture *t, const u8 *rgba, int w, int h);   // w,h <= 1024
bool tex_from_rgb565(Texture *t, const u16 *px, int w, int h);  // reutilizable (cámara)
void tex_free(Texture *t);
void draw_texture(Texture *t, float x, float y, float w, float h, float alpha);
