/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 * Interfaz basada en Gemi3DS de VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 */
// Cloud3DS - íconos vectoriales dibujados con primitivas
#include "icons.h"
#include "gfx.h"
#include <math.h>

#define L(x0, y0, x1, y1) draw_line(cx + (x0) * k, cy + (y0) * k, cx + (x1) * k, cy + (y1) * k, th, c)

void draw_icon(IconId id, float cx, float cy, float s, u32 c) {
    float k = s / 24.0f;         // diseñados en una rejilla de 24x24 centrada en 0
    float th = 2.0f * k;
    if (th < 1.2f) th = 1.2f;
    switch (id) {
    case IC_MENU:
        L(-8, -6, 8, -6); L(-8, 0, 8, 0); L(-8, 6, 8, 6);
        break;
    case IC_PLUS:
        L(-7, 0, 7, 0); L(0, -7, 0, 7);
        break;
    case IC_SEND:
        draw_tri(cx - 8 * k, cy - 8 * k, cx + 9 * k, cy, cx - 3 * k, cy, c);
        draw_tri(cx - 8 * k, cy + 8 * k, cx + 9 * k, cy, cx - 3 * k, cy, c);
        break;
    case IC_MIC:
        draw_rrect(cx - 3.5f * k, cy - 10 * k, 7 * k, 13 * k, 3.5f * k, c);
        draw_arc(cx, cy - 1 * k, 7 * k, th, 0, (float)M_PI, c);
        L(0, 6, 0, 10); L(-4, 10, 4, 10);
        break;
    case IC_CAMERA:
        draw_rrect_outline(cx - 10 * k, cy - 6 * k, 20 * k, 15 * k, 3 * k, th, c);
        draw_rect(cx - 4 * k, cy - 9 * k, 8 * k, 3 * k, c);
        draw_ring(cx, cy + 1.5f * k, 4 * k, th, c);
        break;
    case IC_GLOBE:
        draw_ring(cx, cy, 9 * k, th, c);
        L(-9, 0, 9, 0);
        draw_arc(cx + 9 * k, cy, 12.7f * k, th * 0.8f, (float)(M_PI * 0.75), (float)(M_PI * 1.25), c);
        draw_arc(cx - 9 * k, cy, 12.7f * k, th * 0.8f, (float)(-M_PI * 0.25), (float)(M_PI * 0.25), c);
        break;
    case IC_IMAGE:
        draw_rrect_outline(cx - 10 * k, cy - 8 * k, 20 * k, 16 * k, 2 * k, th, c);
        draw_tri(cx - 8 * k, cy + 6 * k, cx - 2 * k, cy - 2 * k, cx + 4 * k, cy + 6 * k, c);
        draw_tri(cx + 1 * k, cy + 6 * k, cx + 4 * k, cy + 1 * k, cx + 8 * k, cy + 6 * k, c);
        draw_circle(cx + 4 * k, cy - 3 * k, 1.8f * k, c);
        break;
    case IC_GEAR: {
        draw_ring(cx, cy, 5 * k, th * 1.3f, c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            draw_line(cx + cosf(a) * 6.5f * k, cy + sinf(a) * 6.5f * k, cx + cosf(a) * 10 * k, cy + sinf(a) * 10 * k, th * 1.6f, c);
        }
        break;
    }
    case IC_TRASH:
        L(-8, -6, 8, -6); L(-3, -9, 3, -9);
        L(-6, -6, -5, 9); L(6, -6, 5, 9); L(-5, 9, 5, 9);
        L(-2, -2, -2, 5); L(2, -2, 2, 5);
        break;
    case IC_PIN:
        draw_tri(cx - 6 * k, cy - 9 * k, cx + 6 * k, cy - 9 * k, cx, cy - 3 * k, c);
        draw_rect(cx - 3 * k, cy - 9 * k, 6 * k, 8 * k, c);
        L(-7, -1, 7, -1); L(0, -1, 0, 10);
        break;
    case IC_EDIT: case IC_PEN:
        L(-7, 7, 6, -6);
        draw_tri(cx - 9 * k, cy + 9 * k, cx - 8 * k, cy + 4 * k, cx - 4 * k, cy + 8 * k, c);
        L(4, -8, 8, -4);
        break;
    case IC_CHAT:
        draw_rrect_outline(cx - 9 * k, cy - 8 * k, 18 * k, 13 * k, 4 * k, th, c);
        draw_tri(cx - 5 * k, cy + 4 * k, cx - 1 * k, cy + 4 * k, cx - 6 * k, cy + 9 * k, c);
        break;
    case IC_STOP:
        draw_rrect(cx - 6 * k, cy - 6 * k, 12 * k, 12 * k, 2 * k, c);
        break;
    case IC_CHEV_DOWN: L(-6, -3, 0, 3); L(0, 3, 6, -3); break;
    case IC_UP: L(-6, 3, 0, -3); L(0, -3, 6, 3); break;
    case IC_DOWN: L(-6, -3, 0, 3); L(0, 3, 6, -3); break;
    case IC_CHEV_RIGHT: L(-3, -6, 3, 0); L(3, 0, -3, 6); break;
    case IC_CHEV_LEFT: L(3, -6, -3, 0); L(-3, 0, 3, 6); break;
    case IC_CHECK: L(-7, 0, -2, 5); L(-2, 5, 8, -6); break;
    case IC_CLOSE: L(-6, -6, 6, 6); L(-6, 6, 6, -6); break;
    case IC_BULB:
        draw_ring(cx, cy - 3 * k, 6.5f * k, th, c);
        L(-3, 5, 3, 5); L(-2, 8, 2, 8);
        L(-3, 3, -3, 5); L(3, 3, 3, 5);
        break;
    case IC_CLOCK:
        for (int i = 0; i < 12; i++) {   // reloj discontinuo = chat temporal
            float a0 = i * (float)M_PI / 6, a1 = a0 + (float)M_PI / 10;
            draw_arc(cx, cy, 9 * k, th, a0, a1, c);
        }
        L(0, -5, 0, 0); L(0, 0, 4, 3);
        break;
    case IC_REFRESH:
        draw_arc(cx, cy, 8 * k, th, (float)(-M_PI * 0.3), (float)(M_PI * 1.5), c);
        draw_tri(cx + 4 * k, cy - 10 * k, cx + 10 * k, cy - 6 * k, cx + 3 * k, cy - 3 * k, c);
        break;
    case IC_QR:
        draw_rrect_outline(cx - 10 * k, cy - 10 * k, 8 * k, 8 * k, 1, th, c);
        draw_rrect_outline(cx + 2 * k, cy - 10 * k, 8 * k, 8 * k, 1, th, c);
        draw_rrect_outline(cx - 10 * k, cy + 2 * k, 8 * k, 8 * k, 1, th, c);
        draw_rect(cx + 3 * k, cy + 3 * k, 3 * k, 3 * k, c);
        draw_rect(cx + 7 * k, cy + 7 * k, 3 * k, 3 * k, c);
        draw_rect(cx + 7 * k, cy + 2 * k, 3 * k, 2 * k, c);
        break;
    case IC_SPEAKER:
        draw_rect(cx - 9 * k, cy - 3 * k, 5 * k, 6 * k, c);
        draw_tri(cx - 6 * k, cy, cx + 1 * k, cy - 8 * k, cx + 1 * k, cy + 8 * k, c);
        draw_arc(cx + 1 * k, cy, 5 * k, th, (float)(-M_PI / 4), (float)(M_PI / 4), c);
        draw_arc(cx + 1 * k, cy, 9 * k, th, (float)(-M_PI / 4), (float)(M_PI / 4), c);
        break;
    case IC_SEARCH:
        draw_ring(cx - 2 * k, cy - 2 * k, 6 * k, th, c);
        L(3, 3, 9, 9);
        break;
    case IC_GEM:
        draw_tri(cx - 9 * k, cy - 3 * k, cx + 9 * k, cy - 3 * k, cx, cy + 9 * k, c);
        draw_tri(cx - 5 * k, cy - 8 * k, cx + 5 * k, cy - 8 * k, cx - 9 * k, cy - 3 * k, c);
        draw_tri(cx + 5 * k, cy - 8 * k, cx + 9 * k, cy - 3 * k, cx - 9 * k, cy - 3 * k, c);
        break;
    case IC_USER:
        draw_ring(cx, cy - 4 * k, 4 * k, th, c);
        draw_arc(cx, cy + 10 * k, 8 * k, th, (float)M_PI, (float)(2 * M_PI), c);
        break;
    case IC_KEY:
        draw_ring(cx - 5 * k, cy, 4 * k, th, c);
        L(-1, 0, 9, 0); L(6, 0, 6, 4); L(9, 0, 9, 3);
        break;
    case IC_BOLT:
        draw_tri(cx + 2 * k, cy - 10 * k, cx - 6 * k, cy + 2 * k, cx + 1 * k, cy + 1 * k, c);
        draw_tri(cx - 1 * k, cy - 1 * k, cx + 6 * k, cy - 2 * k, cx - 2 * k, cy + 10 * k, c);
        break;
    case IC_BOOK:
        draw_rrect_outline(cx - 9 * k, cy - 8 * k, 9 * k, 16 * k, 1, th, c);
        draw_rrect_outline(cx, cy - 8 * k, 9 * k, 16 * k, 1, th, c);
        break;
    case IC_MAP:
        draw_ring(cx, cy - 3 * k, 5 * k, th, c);
        draw_tri(cx - 4.5f * k, cy - 1 * k, cx + 4.5f * k, cy - 1 * k, cx, cy + 9 * k, c);
        draw_circle(cx, cy - 3 * k, 1.8f * k, c);
        break;
    case IC_CODE:
        L(-4, -6, -9, 0); L(-9, 0, -4, 6); L(4, -6, 9, 0); L(9, 0, 4, 6); L(2, -8, -2, 8);
        break;
    case IC_INFO:
        draw_ring(cx, cy, 9 * k, th, c);
        L(0, -1, 0, 5); draw_circle(cx, cy - 4.5f * k, 1.4f * k, c);
        break;
    case IC_LOGOUT:
        L(-8, -8, -8, 8); L(-8, -8, 0, -8); L(-8, 8, 0, 8);
        L(-3, 0, 9, 0); L(5, -4, 9, 0); L(5, 4, 9, 0);
        break;
    case IC_PALETTE:
        draw_ring(cx, cy, 9 * k, th, c);
        draw_circle(cx - 4 * k, cy - 3 * k, 1.8f * k, c);
        draw_circle(cx + 1 * k, cy - 5 * k, 1.8f * k, c);
        draw_circle(cx + 5 * k, cy - 1 * k, 1.8f * k, c);
        break;
    case IC_TEXTSIZE:
        L(-9, 7, -5, -7); L(-5, -7, -1, 7); L(-7.5f, 2, -2.5f, 2);
        L(2, 7, 5, -2); L(5, -2, 8, 7);
        break;
    case IC_NOTE:
        draw_rrect_outline(cx - 8 * k, cy - 9 * k, 16 * k, 18 * k, 2 * k, th, c);
        L(-4, -4, 4, -4); L(-4, 0, 4, 0); L(-4, 4, 1, 4);
        break;
    case IC_WIFI:
        draw_arc(cx, cy + 7 * k, 14 * k, th, (float)(-M_PI * 0.75), (float)(-M_PI * 0.25), c);
        draw_arc(cx, cy + 7 * k, 9 * k, th, (float)(-M_PI * 0.75), (float)(-M_PI * 0.25), c);
        draw_circle(cx, cy + 6 * k, 2 * k, c);
        break;
    case IC_SPARK:
        draw_sparkle_mono(cx, cy, 10 * k, 0, c);
        break;
    case IC_FLIP:
        draw_arc(cx, cy, 8 * k, th, (float)(M_PI * 0.1), (float)(M_PI * 0.9), c);
        draw_arc(cx, cy, 8 * k, th, (float)(M_PI * 1.1), (float)(M_PI * 1.9), c);
        draw_tri(cx - 10 * k, cy - 2 * k, cx - 5 * k, cy - 2 * k, cx - 7.5f * k, cy + 2 * k, c);
        draw_tri(cx + 10 * k, cy + 2 * k, cx + 5 * k, cy + 2 * k, cx + 7.5f * k, cy - 2 * k, c);
        break;
    case IC_STAR:
        for (int i = 0; i < 5; i++) {
            float a = -M_PI / 2 + i * 2 * M_PI / 5, b = a + 2 * M_PI / 5 * 2;
            draw_line(cx + cosf(a) * 9 * k, cy + sinf(a) * 9 * k, cx + cosf(b) * 9 * k, cy + sinf(b) * 9 * k, th, c);
        }
        break;
    case IC_LINK:
        draw_rrect_outline(cx - 10 * k, cy - 4 * k, 11 * k, 8 * k, 4 * k, th, c);
        draw_rrect_outline(cx - 1 * k, cy - 4 * k, 11 * k, 8 * k, 4 * k, th, c);
        break;
    case IC_CLOUD:
        draw_arc(cx - 4 * k, cy + 1 * k, 5 * k, th, (float)(M_PI * 0.5), (float)(M_PI * 1.35), c);
        draw_arc(cx + 1 * k, cy - 1 * k, 6 * k, th, (float)(M_PI * 1.1), (float)(M_PI * 1.95), c);
        draw_arc(cx + 6 * k, cy + 2 * k, 4 * k, th, (float)(-M_PI * 0.45), (float)(M_PI * 0.5), c);
        L(-4, 6, 6, 6);
        break;
    case IC_FOLDER:
        draw_rrect_outline(cx - 10 * k, cy - 6 * k, 20 * k, 15 * k, 2 * k, th, c);
        L(-9, -6, -8, -9); L(-8, -9, -3, -9); L(-3, -9, -1, -6);
        break;
    case IC_UPLOAD:
        L(0, 5, 0, -8); L(-5, -3, 0, -8); L(0, -8, 5, -3); L(-8, 9, 8, 9);
        break;
    case IC_DOWNLOAD:
        L(0, -8, 0, 5); L(-5, 0, 0, 5); L(0, 5, 5, 0); L(-8, 9, 8, 9);
        break;
    case IC_PHONE:
        draw_rrect_outline(cx - 6 * k, cy - 10 * k, 12 * k, 20 * k, 2.5f * k, th, c);
        L(-2, 7, 2, 7);
        break;
    case IC_TABLET:
        draw_rrect_outline(cx - 8 * k, cy - 10 * k, 16 * k, 20 * k, 2.5f * k, th, c);
        L(-2, 7, 2, 7);
        break;
    case IC_LAPTOP:
        draw_rrect_outline(cx - 8 * k, cy - 8 * k, 16 * k, 12 * k, 1.5f * k, th, c);
        L(-11, 8, 11, 8);
        break;
    case IC_DEVICES:
        draw_rrect_outline(cx - 10 * k, cy - 8 * k, 14 * k, 11 * k, 1.5f * k, th, c);
        L(-6, 7, 0, 7);
        draw_rrect_outline(cx + 4 * k, cy - 3 * k, 7 * k, 12 * k, 1.5f * k, th, c);
        break;
    case IC_LOCK:
        draw_rrect_outline(cx - 7 * k, cy - 2 * k, 14 * k, 11 * k, 2 * k, th, c);
        draw_arc(cx, cy - 4 * k, 4.5f * k, th, (float)M_PI, (float)(2 * M_PI), c);
        L(-4.5f, -4, -4.5f, -2); L(4.5f, -4, 4.5f, -2);
        break;
    case IC_WIFI_OFF:
        draw_arc(cx, cy + 7 * k, 14 * k, th, (float)(-M_PI * 0.75), (float)(-M_PI * 0.25), c);
        draw_arc(cx, cy + 7 * k, 9 * k, th, (float)(-M_PI * 0.75), (float)(-M_PI * 0.25), c);
        draw_circle(cx, cy + 6 * k, 2 * k, c);
        L(-9, -9, 9, 9);
        break;
    case IC_POWER:
        draw_arc(cx, cy + 1 * k, 8 * k, th, (float)(-M_PI * 0.3), (float)(M_PI * 1.3), c);
        L(0, -10, 0, -1);
        break;
    case IC_SHIELD:
        L(0, -10, 8, -7); L(8, -7, 7, 2); L(7, 2, 0, 10);
        L(0, -10, -8, -7); L(-8, -7, -7, 2); L(-7, 2, 0, 10);
        break;
    case IC_HEART:
        draw_circle(cx - 4 * k, cy - 2 * k, 4.5f * k, c);
        draw_circle(cx + 4 * k, cy - 2 * k, 4.5f * k, c);
        draw_tri(cx - 8.3f * k, cy, cx + 8.3f * k, cy, cx, cy + 9 * k, c);
        break;
    }
}
