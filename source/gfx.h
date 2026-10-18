#pragma once

#include "app.h"
#include <citro2d.h>

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

typedef struct {
	u32 bg, card, card2, line, text, muted, accent, accent2, ok, warn, err, on_accent, shadow;
} Theme;

extern Theme T;

enum { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

typedef enum {
	IC_CLOUD, IC_WIFI_OFF, IC_UP, IC_DOWN, IC_FOLDER, IC_TRASH, IC_PENCIL, IC_LINK, IC_PHONE,
	IC_LAPTOP, IC_TABLET, IC_GEAR, IC_LIST, IC_DEVICES, IC_LOCK, IC_CHECK, IC_CROSS, IC_ALERT,
	IC_INFO, IC_KEY, IC_GLOBE, IC_MOON, IC_HEART, IC_POWER,
} Icon;

void gfx_init(void);
void gfx_exit(void);
void gfx_set_theme(bool light);

void gfx_frame_begin(void);
void gfx_frame_end(void);
void gfx_target_top(void);
void gfx_target_bot(void);
bool gfx_on_top(void);

u32  gfx_alpha(u32 c, float a); /* multiplica el alfa */
u32  gfx_mix(u32 a, u32 b, float t);

void gfx_rect(float x, float y, float w, float h, u32 c);
void gfx_rrect(float x, float y, float w, float h, float r, u32 c);
void gfx_circle(float x, float y, float r, u32 c);
void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c);
void gfx_line(float x1, float y1, float x2, float y2, float th, u32 c);
void gfx_grad_h(float x, float y, float w, float h, u32 c1, u32 c2);
void gfx_brand_bar(float x, float y, float w, float h, float frac); /* barra con el degradado de la marca */

/* Texto con la fuente del sistema. size ~ altura en píxeles.
 * maxw > 0 recorta con «…». Devuelve el ancho dibujado. */
float gfx_text(float x, float y, float size, u32 color, int align, float maxw, const char *s);
float gfx_text_w(float size, const char *s);
float gfx_wrap(float x, float y, float size, u32 color, int align, float w, const char *s, int max_lines);
float gfx_title(float x, float y, float size, u32 color); /* «Cloud3DS» con el ajuste 3→D */

void gfx_icon(Icon ic, float cx, float cy, float s, u32 c);

void gfx_scissor(float x, float y, float w, float h);
void gfx_scissor_off(void);

/* Animación */
float ease_out_cubic(float t);
float ease_in_out(float t);
float ease_out_back(float t);
float approach(float cur, float target, float speed, float dt);
float clampf(float v, float a, float b);
