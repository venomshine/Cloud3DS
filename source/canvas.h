/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* <canvas> 2D de las páginas: cada lienzo son sus píxeles RGBA; los caminos se rellenan y se trazan con el
 * rasterizador de nanosvg, el texto con stb_truetype (Liberation Sans) y lo demás, aquí. Los usa el JavaScript
 * (jsdom.c) y el navegador pinta sus píxeles como una imagen («canvas:N»). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int  cv_new(const void *key, int w, int h); /* un lienzo (id > 0) de su elemento (key) */
int  cv_of(const void *key);                /* el lienzo de un elemento (0: no tiene) */
void cv_free(int id);
void cv_free_all(void);
bool cv_resize(int id, int w, int h);      /* (borra, como al cambiar width/height) */
void cv_size(int id, int *w, int *h);
/* una copia de sus píxeles (RGBA, malloc) si cambió desde la última vez (o siempre, con force); NULL si no */
uint8_t *cv_take(int id, int *w, int *h, bool force);
bool cv_dirty_any(void);

/* estado (lo de save/restore) */
void cv_save(int id);
void cv_restore(int id);
void cv_set_transform(int id, const float m[6]);   /* a b c d e f, como setTransform */
void cv_get_transform(int id, float m[6]);
void cv_style(int id, uint32_t fill, uint32_t stroke, float alpha, float line_w, int cap, int join, float miter, int comp);

/* caminos */
void cv_begin(int id);
void cv_move(int id, float x, float y);
void cv_line(int id, float x, float y);
void cv_bezier(int id, float c1x, float c1y, float c2x, float c2y, float x, float y);
void cv_quad(int id, float cx, float cy, float x, float y);
void cv_ellipse(int id, float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw);
void cv_arc_to(int id, float x1, float y1, float x2, float y2, float r);
void cv_rect(int id, float x, float y, float w, float h);
void cv_close(int id);
void cv_fill(int id, bool evenodd);
void cv_stroke(int id);

/* directo */
void cv_fill_rect(int id, float x, float y, float w, float h);
void cv_clear_rect(int id, float x, float y, float w, float h);
void cv_draw_image(int id, const uint8_t *src, int sw_, int sh_, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
float cv_text(int id, const char *s, float x, float y, float px, bool bold, bool draw); /* dibuja (o mide) y da el ancho */
void cv_text_metrics(float px, bool bold, float *ascent, float *descent);

/* píxeles */
bool cv_get(int id, int x, int y, int w, int h, uint8_t *out);         /* w*h*4 */
void cv_put(int id, const uint8_t *in, int w, int h, int dx, int dy);
char *cv_png_data_url(int id);                                         /* data:image/png;base64,… (malloc) */
