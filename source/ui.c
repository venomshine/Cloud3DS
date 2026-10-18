/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - componentes de interfaz
#include "ui.h"
#include "i18n.h"
#include "qrcodegen.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

Touch g_touch;
u32 g_kdown, g_kheld, g_krepeat;
Dialog g_dialog;

// ------------------------------------------------------------ entrada
void input_update(void) {
    hidScanInput();
    g_kdown = hidKeysDown();
    g_kheld = hidKeysHeld();
    g_krepeat = hidKeysDownRepeat();
    touchPosition tp;
    hidTouchRead(&tp);
    bool held = (g_kheld & KEY_TOUCH) != 0;
    Touch *t = &g_touch;
    t->down = held && !t->held;
    t->up = !held && t->held;
    if (t->down) {
        t->sx = t->px = t->x = tp.px; t->sy = t->py = t->y = tp.py;
        t->drag = false; t->hold = 0; t->long_fired = false;
    } else if (held) {
        t->px = t->x; t->py = t->y;
        t->x = tp.px; t->y = tp.py;
        t->hold += g_dt;
        if (abs(t->x - t->sx) > 6 || abs(t->y - t->sy) > 6) t->drag = true;
    } else if (!t->up) {
        t->px = t->x; t->py = t->y;
    }
    t->held = held;
}

static bool in_rect(int x, int y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

bool touch_in(float x, float y, float w, float h) {
    return g_touch.held && !g_touch.drag && in_rect(g_touch.x, g_touch.y, x, y, w, h) && in_rect(g_touch.sx, g_touch.sy, x, y, w, h);
}

bool tap(float x, float y, float w, float h) {
    return g_touch.up && !g_touch.drag && !g_touch.long_fired &&
           in_rect(g_touch.x, g_touch.y, x, y, w, h) && in_rect(g_touch.sx, g_touch.sy, x, y, w, h);
}

bool long_press(float x, float y, float w, float h) {
    if (g_touch.held && !g_touch.drag && !g_touch.long_fired && g_touch.hold > 0.6f && in_rect(g_touch.sx, g_touch.sy, x, y, w, h)) {
        g_touch.long_fired = true;
        return true;
    }
    return false;
}

// ------------------------------------------------------------ botones
bool ui_icon_button(IconId ic, float cx, float cy, float r, u32 fg, u32 bg) {
    bool pressed = touch_in(cx - r, cy - r, r * 2, r * 2);
    if (bg >> 24) draw_circle(cx, cy, r, bg);
    if (pressed) draw_circle(cx, cy, r, col_alpha(T.text, 0.12f));
    draw_icon(ic, cx, cy, r * 1.15f, fg);
    return tap(cx - r, cy - r, r * 2, r * 2);
}

bool ui_pill_button(const char *label, int ic, float x, float y, float w, float h, u32 fg, u32 bg) {
    label = tr(label);
    bool pressed = touch_in(x, y, w, h);
    float s = pressed ? 0.96f : 1.0f;
    float cx = x + w / 2, cy = y + h / 2;
    float W = w * s, H = h * s;
    draw_rrect(cx - W / 2, cy - H / 2, W, H, H / 2, pressed ? col_lerp(bg, T.text, 0.12f) : bg);
    float sc = h >= 34 ? 0.55f : 0.48f;
    float tw = text_width(label, sc) + (ic >= 0 ? h * 0.6f + 4 : 0);
    float tx = cx - tw / 2;
    if (ic >= 0) {
        draw_icon((IconId)ic, tx + h * 0.3f, cy, h * 0.55f, fg);
        tx += h * 0.6f + 4;
    }
    text_draw(label, tx, cy - text_lineh(sc) / 2, sc, fg);
    return tap(x, y, w, h);
}

// ------------------------------------------------------------ hojas
#define SH_TOP 36.0f
static float item_h(const SheetItem *it) { return it->header ? 24.0f : (it->sub[0] ? 40.0f : 32.0f); }

void sheet_open(Sheet *s, int kind, const char *title, bool from_left) {
    s->open = true; s->closing = false; s->from_left = from_left;
    s->anim = 0; s->n = 0; s->sel = -1; s->kind = kind;
    s->scroll = s->scroll_target = 0; s->chosen = -1; s->held_item = -1;
    s->swipe_item = -1; s->swipe_x = s->swipe_rest = 0; s->swiping = s->vscroll = false; s->swipe_action = SW_NONE;
    snprintf(s->title, sizeof(s->title), "%s", tr(title));
}

void sheet_close(Sheet *s) { if (s->open) s->closing = true; }
bool sheet_visible(Sheet *s) { return s->open; }

SheetItem *sheet_add(Sheet *s, int icon, const char *label, const char *sub, int right, int id) {
    if (s->n >= 320) return &s->items[319];
    SheetItem *it = &s->items[s->n++];
    memset(it, 0, sizeof(*it));
    it->icon = icon; it->icon_col = 0;
    snprintf(it->label, sizeof(it->label), "%s", label ? tr(label) : "");
    snprintf(it->sub, sizeof(it->sub), "%s", sub ? tr(sub) : "");
    it->right = right; it->id = id;
    if (s->sel < 0) s->sel = s->n - 1;
    return it;
}

void sheet_header(Sheet *s, const char *label) {
    SheetItem *it = sheet_add(s, -1, label, NULL, SR_NONE, -1);
    it->header = true;
    if (s->sel == s->n - 1) s->sel = -1;
}

SheetItem *sheet_selected(Sheet *s) { return (s->sel >= 0 && s->sel < s->n) ? &s->items[s->sel] : NULL; }

static float sheet_content_h(Sheet *s) {
    float h = 0;
    for (int i = 0; i < s->n; i++) h += item_h(&s->items[i]);
    return h;
}

static float panel_w(Sheet *s) { return s->from_left ? 276 : BOT_W; }

static void ensure_visible(Sheet *s) {
    float y = 0;
    for (int i = 0; i < s->sel && i < s->n; i++) y += item_h(&s->items[i]);
    float h = s->sel >= 0 ? item_h(&s->items[s->sel]) : 0;
    float view = SCR_H - SH_TOP;
    if (y < s->scroll_target) s->scroll_target = y - 4;
    if (y + h > s->scroll_target + view) s->scroll_target = y + h - view + 4;
}

static void move_sel(Sheet *s, int d) {
    int i = s->sel;
    for (int k = 0; k < s->n; k++) {
        i += d;
        if (i < 0) i = s->n - 1;
        if (i >= s->n) i = 0;
        if (!s->items[i].header && !s->items[i].disabled) { s->sel = i; break; }
    }
    ensure_visible(s);
}

// deslizar: anchos de los botones que quedan a la vista y umbral de la acción completa
#define SW_PIN_W 76.0f
#define SW_ACT_W 140.0f
#define SW_FULL 0.6f
static float swipe_full(Sheet *s) { return panel_w(s) * SW_FULL; }

// gestiona el deslizamiento horizontal; devuelve -4 si hay que hacer una acción, -5 si consumió el toque
static int sheet_swipe(Sheet *s, float list_y0) {
    float pw = panel_w(s);
    // al empezar a arrastrar se decide el eje (horizontal = deslizar el chat)
    if (g_touch.held && g_touch.drag && !s->swiping && !s->vscroll) {
        int dx = g_touch.x - g_touch.sx, dy = g_touch.y - g_touch.sy;
        int hit = -1;
        float y = list_y0;
        for (int i = 0; i < s->n; i++) {
            float h = item_h(&s->items[i]);
            if (g_touch.sy >= y && g_touch.sy < y + h && g_touch.sy > SH_TOP) hit = i;
            y += h;
        }
        if (abs(dx) > abs(dy) && hit >= 0 && s->items[hit].swipe && g_touch.sx < pw) {
            if (s->swipe_item != hit) { s->swipe_item = hit; s->swipe_x = s->swipe_rest = 0; }
            s->swiping = true;
            s->swipe_start = s->swipe_rest;
        } else { s->vscroll = true; s->swipe_rest = 0; }
    }
    if (s->swiping && g_touch.held) {
        float x = s->swipe_start + (g_touch.x - g_touch.sx);
        float lim = pw - 16;
        if (x > lim) x = lim;
        if (x < -lim) x = -lim;
        s->swipe_x = x;
        return -5;
    }
    if (s->swiping && g_touch.up) {
        s->swiping = false;
        float x = s->swipe_x, full = swipe_full(s);
        if (x > full) { s->swipe_action = SW_PIN; s->held_item = s->swipe_item; s->swipe_rest = 0; return -4; }
        if (x < -full) { s->swipe_action = SW_DELETE; s->held_item = s->swipe_item; s->swipe_rest = 0; return -4; }
        if (x > SW_PIN_W * 0.5f) s->swipe_rest = SW_PIN_W;
        else if (x < -SW_ACT_W * 0.4f) s->swipe_rest = -SW_ACT_W;
        else { s->swipe_rest = 0; }
        return -5;
    }
    if (g_touch.up) s->vscroll = false;
    // con los botones a la vista: tocar uno hace su acción, tocar fuera los oculta
    if (s->swipe_item >= 0 && s->swipe_rest != 0 && g_touch.up && !g_touch.drag) {
        float y = list_y0;
        for (int i = 0; i < s->swipe_item; i++) y += item_h(&s->items[i]);
        float h = item_h(&s->items[s->swipe_item]);
        bool row = g_touch.y >= y && g_touch.y < y + h && g_touch.sy >= y && g_touch.sy < y + h;
        int act = SW_NONE;
        if (row && s->swipe_rest > 0 && g_touch.x < SW_PIN_W) act = SW_PIN;
        if (row && s->swipe_rest < 0 && g_touch.x >= pw - SW_ACT_W && g_touch.x < pw)
            act = g_touch.x < pw - SW_ACT_W / 2 ? SW_RENAME : SW_DELETE;
        s->swipe_rest = 0;
        if (act) { s->swipe_action = act; s->held_item = s->swipe_item; return -4; }
        return -5;
    }
    return -1;
}

int sheet_update(Sheet *s) {
    if (!s->open) return -1;
    if (s->closing) {
        s->anim = approach(s->anim, 0, 18);
        if (s->anim < 0.02f) { s->open = false; s->closing = false; return -2; }
        return -1;
    }
    s->anim = approach(s->anim, 1, 14);
    float pw = panel_w(s);
    float maxs = sheet_content_h(s) - (SCR_H - SH_TOP);
    if (maxs < 0) maxs = 0;

    if (s->swipe_item >= s->n) s->swipe_item = -1;
    if (!s->swiping) s->swipe_x = approach(s->swipe_x, s->swipe_rest, 18);
    if (s->swipe_item >= 0 && s->swipe_rest != 0 && (g_kdown & (KEY_B | KEY_UP | KEY_DOWN))) {
        s->swipe_rest = 0;              // B o moverse con la cruceta oculta los botones
        if (g_kdown & KEY_B) return -1;
    }
    {
        int r = sheet_swipe(s, SH_TOP - s->scroll);
        if (r == -4) return -4;
        if (r == -5) { s->scroll = approach(s->scroll, s->scroll_target, 16); return -1; }
    }

    if (g_kdown & KEY_B) { sheet_close(s); return -1; }
    if (g_krepeat & KEY_DOWN) move_sel(s, 1);
    if (g_krepeat & KEY_UP) move_sel(s, -1);
    if (g_krepeat & KEY_RIGHT && !s->from_left) { s->scroll_target += 120; }
    if (g_krepeat & KEY_LEFT && !s->from_left) { s->scroll_target -= 120; }
    if ((g_kdown & KEY_A) && s->sel >= 0 && !s->items[s->sel].disabled) { s->hit_right = false; return s->sel; }
    if ((g_kdown & KEY_Y) && s->sel >= 0) { s->held_item = s->sel; return -3; }

    // táctil
    if (g_touch.held && g_touch.drag && g_touch.sy > SH_TOP && g_touch.sx < pw && !s->swiping) {
        s->scroll_target -= (g_touch.y - g_touch.py);
        s->scroll = s->scroll_target;
    }
    if (s->from_left && tap(pw, 0, BOT_W - pw, SCR_H)) { sheet_close(s); return -1; }
    if (tap(pw - 36, 0, 36, SH_TOP)) { sheet_close(s); return -1; }
    float y = SH_TOP - s->scroll;
    for (int i = 0; i < s->n; i++) {
        float h = item_h(&s->items[i]);
        if (!s->items[i].header && !s->items[i].disabled && y + h > SH_TOP) {
            if (long_press(0, y, pw, h)) { s->sel = i; s->held_item = i; return -3; }
            if (tap(0, y, pw, h) && g_touch.y > SH_TOP) { s->sel = i; s->hit_right = g_touch.x > pw - 48; return i; }
        }
        y += h;
    }
    if (s->scroll_target < 0) s->scroll_target = 0;
    if (s->scroll_target > maxs) s->scroll_target = maxs;
    s->scroll = approach(s->scroll, s->scroll_target, 16);
    return -1;
}

static void draw_toggle(float x, float y, bool on, float alpha) {
    u32 track = on ? T.accent : T.surface2;
    draw_rrect(x, y, 30, 16, 8, col_alpha(track, alpha));
    draw_circle(on ? x + 22 : x + 8, y + 8, 6, col_alpha(on ? (T.dark ? col_hex(0x062E6F) : 0xFFFFFFFF) : T.text3, alpha));
}

// botones bajo un chat deslizado: Fijar a la izquierda; Renombrar y Eliminar a la derecha.
// Tienen tamaño fijo y entran pegados al borde de la fila: el icono y el texto van fijos dentro
// del botón (si se ve el 20 % del botón, se ve el 20 % de su contenido), nunca se escalan.
static float s_sw_l, s_sw_r;    // bordes del panel: los botones nunca se dibujan fuera

static void swipe_button(IconId ic, const char *label, float cx, float x0, float x1, float y, float h,
                         float clip_y, float clip_h, u32 bg, u32 fg) {
    if (x0 < s_sw_l) x0 = s_sw_l;
    if (x1 > s_sw_r) x1 = s_sw_r;
    if (x1 - x0 < 0.5f) return;
    gfx_clip(x0, clip_y, x1 - x0, clip_h);
    draw_rect(x0, y, x1 - x0, h, bg);
    draw_icon(ic, cx, y + h / 2 - 8, 15, fg);
    text_draw_center(label, cx, y + h / 2 + 4, 0.4f, fg);
}

static void draw_swipe_actions(Sheet *s, SheetItem *it, float ox, float y, float pw, float h, float sx,
                               float clip_y, float clip_h) {
    u32 white = 0xFFFFFFFF;
    float full = swipe_full(s);
    s_sw_l = ox; s_sw_r = ox + pw;
    if (sx > 0) {
        // Fijar: pegado al borde izquierdo de la fila; al pasar del ancho se estira por la izquierda
        u32 bg = sx > full ? col_lerp(T.accent, T.text, 0.15f) : T.accent;
        swipe_button(IC_PIN, it->pinned ? tr("No fijar") : tr("Fijar"), ox + sx - SW_PIN_W / 2, ox, ox + sx, y, h,
                     clip_y, clip_h, bg, T.dark ? col_hex(0x062E6F) : white);
    } else {
        float w = -sx, edge = ox + pw - w;            // borde derecho de la fila desplazada
        float half = SW_ACT_W / 2;
        // Eliminar se estira desde la derecha y va tapando a Renombrar hasta ocupar toda la fila
        float del_w = w <= SW_ACT_W ? half : half + (w - SW_ACT_W) * (full - half) / (full - SW_ACT_W);
        float del_x = w <= SW_ACT_W ? edge + half : ox + pw - del_w;
        if (del_x < edge) del_x = edge;
        swipe_button(IC_PEN, tr("Renombrar"), edge + half / 2, edge, fminf(edge + half, del_x), y, h,
                     clip_y, clip_h, T.surface2, T.text);
        u32 dbg = w > full ? col_lerp(T.danger, 0xFF000000, 0.12f) : T.danger;
        swipe_button(IC_TRASH, tr("Eliminar"), del_x + half / 2, del_x, ox + pw, y, h, clip_y, clip_h, dbg, white);
    }
}

void sheet_draw(Sheet *s) {
    if (!s->open) return;
    float a = ease_out_cubic(s->anim);
    float pw = panel_w(s);
    draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
    float ox = s->from_left ? -(1 - a) * pw : 0;
    float oy = s->from_left ? 0 : (1 - a) * SCR_H;
    // panel
    draw_rect(ox, oy, pw, SCR_H, T.bg2);
    // lista
    float y = oy + SH_TOP - s->scroll;
    for (int i = 0; i < s->n; i++) {
        SheetItem *it = &s->items[i];
        float h = item_h(it);
        if (y + h < oy + SH_TOP || y > SCR_H) { y += h; continue; }
        if (it->header) {
            text_draw(it->label, ox + 14, y + 7, 0.45f, T.text3);
            y += h; continue;
        }
        bool sel = i == s->sel;
        bool press = touch_in(ox, y, pw, h) && g_touch.y > SH_TOP && !(i == s->swipe_item && s->swipe_rest != 0);
        float sx = (i == s->swipe_item) ? s->swipe_x : 0;
        bool clip = fabsf(sx) > 0.5f;
        if (clip) {
            float cy = fmaxf(y, oy + SH_TOP), ch = y + h - cy;
            draw_swipe_actions(s, it, ox, y, pw, h, sx, cy, ch);
            gfx_clip(ox, cy, pw, ch);
            draw_rect(ox + sx, y, pw, h, T.bg2);
        }
        float ox0 = ox;
        ox += sx;
        if (sel || press) draw_rrect(ox + 4, y + 1, pw - 8, h - 2, 10, press ? T.pill_hi : col_alpha(T.accent_soft, 1));
        float tx = ox + 14;
        u32 fg = it->disabled ? T.text3 : T.text;
        if (it->icon >= 0) {
            draw_icon((IconId)it->icon, ox + 24, y + h / 2, 18, it->icon_col ? it->icon_col : (it->disabled ? T.text3 : T.text2));
            tx = ox + 44;
        }
        float rw = it->right == SR_NONE ? 8 : (it->right == SR_VALUE ? text_width(it->value, 0.45f) + 20 : 44);
        if (it->sub[0]) {
            text_draw_clip(it->label, tx, y + 4, pw - (tx - ox) - rw, 0.5f, fg);
            text_draw_clip(it->sub, tx, y + 21, pw - (tx - ox) - rw, 0.42f, T.text3);
        } else {
            text_draw_clip(it->label, tx, y + (h - text_lineh(0.5f)) / 2, pw - (tx - ox) - rw, 0.5f, fg);
        }
        float rx = ox + pw - 14;
        switch (it->right) {
        case SR_CHECK: draw_icon(IC_CHECK, rx - 8, y + h / 2, 16, T.accent); break;
        case SR_CHEV: draw_icon(IC_CHEV_RIGHT, rx - 6, y + h / 2, 14, T.text3); break;
        case SR_ON: draw_toggle(rx - 30, y + h / 2 - 8, true, 1); break;
        case SR_OFF: draw_toggle(rx - 30, y + h / 2 - 8, false, 1); break;
        case SR_VALUE: text_draw_right(it->value, rx, y + (h - text_lineh(0.45f)) / 2, 0.45f, T.accent); break;
        case SR_CLEAR: {
            bool p = touch_in(ox + pw - 48, y, 48, h);
            draw_circle(rx - 8, y + h / 2, 11, p ? T.pill_hi : T.surface2);
            draw_icon(IC_CLOSE, rx - 8, y + h / 2, 11, T.text);
            break;
        }
        }
        ox = ox0;
        if (clip) gfx_clip_off();
        y += h;
    }
    // barra de desplazamiento
    float total = sheet_content_h(s), view = SCR_H - SH_TOP;
    if (total > view) {
        float bh = view * view / total;
        float by = SH_TOP + (view - bh) * (s->scroll / (total - view));
        draw_rrect(ox + pw - 4, oy + by, 3, bh, 1.5f, col_alpha(T.text3, 0.6f));
    }
    // cabecera
    draw_rect(ox, oy, pw, SH_TOP, T.bg2);
    draw_rect(ox, oy + SH_TOP - 1, pw, 1, T.divider);
    if (!s->from_left) draw_rrect(BOT_W / 2 - 16, oy + 3, 32, 3, 1.5f, T.text3);
    text_draw_clip(s->title, ox + 14, oy + 11, pw - 60, 0.58f, T.text);
    bool cp = touch_in(ox + pw - 36, oy, 36, SH_TOP);
    if (cp) draw_circle(ox + pw - 18, oy + 18, 14, T.pill_hi);
    draw_icon(IC_CLOSE, ox + pw - 18, oy + 18, 16, T.text2);
}

// ------------------------------------------------------------ aviso (pantalla superior)
static char s_toast[160];
static float s_toast_t = -1;
static int s_toast_ic = IC_CHECK;
static u32 s_toast_col;

void toast_icon(const char *msg, int ic, u32 col) {
    snprintf(s_toast, sizeof(s_toast), "%s", tr(msg));
    s_toast_t = 0;
    s_toast_ic = ic;
    s_toast_col = col;
}

void toast(const char *msg) { toast_icon(msg, IC_CHECK, T.accent); }

void toast_draw_top(void) {
    if (s_toast_t < 0) return;
    s_toast_t += g_dt;
    const float life = 3.0f;
    if (s_toast_t > life) { s_toast_t = -1; return; }
    float a = s_toast_t < 0.25f ? ease_out_cubic(s_toast_t / 0.25f) : clampf((life - s_toast_t) / 0.4f, 0, 1);
    float w = text_width(s_toast, 0.45f) + 46;
    if (w > 360) w = 360;
    float x = (TOP_W - w) / 2, y = SCR_H - 38 + (1 - a) * 10;
    draw_rrect(x, y + 2, w, 28, 14, col_alpha(T.shadow, a));
    draw_rrect(x, y, w, 28, 14, col_alpha(T.surface2, a));
    draw_circle(x + 15, y + 14, 9, col_alpha(s_toast_col, a));
    draw_icon((IconId)s_toast_ic, x + 15, y + 14, 12, col_alpha(0xFFFFFFFF, a));
    text_draw_clip(s_toast, x + 30, y + 7, w - 40, 0.45f, col_alpha(T.text, a));
}

// ------------------------------------------------------------ diálogo (pantalla inferior)
#define DLG_X 24.0f
#define DLG_W 272.0f
#define DLG_Y 38.0f
#define DLG_H 164.0f
#define DLG_BW 118.0f
#define DLG_BH 34.0f

void dialog_open(int ctx, const char *title, const char *msg, const char *ok, bool danger) {
    g_dialog.open = true; g_dialog.anim = 0; g_dialog.ctx = ctx; g_dialog.danger = danger;
    snprintf(g_dialog.title, sizeof(g_dialog.title), "%s", tr(title));
    snprintf(g_dialog.msg, sizeof(g_dialog.msg), "%s", tr(msg));
    snprintf(g_dialog.ok, sizeof(g_dialog.ok), "%s", tr(ok));
}

static float dlg_by(void) { return DLG_Y + DLG_H - DLG_BH - 14; }

int dialog_update(void) {
    if (!g_dialog.open) return -1;
    g_dialog.anim = approach(g_dialog.anim, 1, 16);
    float by = dlg_by();
    if ((g_kdown & KEY_B) || tap(DLG_X + 14, by, DLG_BW, DLG_BH)) { g_dialog.open = false; return 0; }
    if ((g_kdown & KEY_A) || tap(DLG_X + DLG_W - 14 - DLG_BW, by, DLG_BW, DLG_BH)) { g_dialog.open = false; return 1; }
    // tocar fuera también cancela
    if (g_touch.up && !g_touch.drag && !(g_touch.sx >= DLG_X && g_touch.sx < DLG_X + DLG_W && g_touch.sy >= DLG_Y && g_touch.sy < DLG_Y + DLG_H)) {
        g_dialog.open = false;
        return 0;
    }
    return -1;
}

static void dlg_button(const char *glyph, const char *label, float x, float y, u32 fg, u32 bg) {
    bool p = touch_in(x, y, DLG_BW, DLG_BH);
    draw_rrect(x, y, DLG_BW, DLG_BH, DLG_BH / 2, p ? col_lerp(bg, T.text, 0.15f) : bg);
    char t[64];
    snprintf(t, sizeof(t), "%s %s", glyph, label);
    float w = text_width(t, 0.47f);
    if (w > DLG_BW - 16) { snprintf(t, sizeof(t), "%s", label); w = text_width(t, 0.47f); }
    text_draw_clip(t, x + (DLG_BW - fminf(w, DLG_BW - 16)) / 2, y + (DLG_BH - text_lineh(0.47f)) / 2, DLG_BW - 16, 0.47f, fg);
}

void dialog_draw(void) {
    if (!g_dialog.open) return;
    float a = ease_out_cubic(g_dialog.anim);
    draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
    float y = DLG_Y + (1 - a) * 14;
    draw_rrect(DLG_X, y + 3, DLG_W, DLG_H, 16, col_alpha(T.shadow, a));
    draw_rrect(DLG_X, y, DLG_W, DLG_H, 16, T.surface);
    float tw = text_width(g_dialog.title, 0.58f);
    text_draw_clip(g_dialog.title, DLG_X + (DLG_W - fminf(tw, DLG_W - 28)) / 2, y + 14, DLG_W - 28, 0.58f, T.text);
    // texto centrado línea a línea
    float sc = 0.44f, lw = DLG_W - 36;
    int nl = text_wrap_lines(g_dialog.msg, lw, sc, 4, 0, 0, 0, false);
    const char *p = g_dialog.msg;
    for (int l = 0; l < nl && *p; l++) {
        char line[300]; int n = 0; float w = 0; const char *last_sp = NULL; int last_n = 0;
        while (*p && *p != '\n') {
            const char *q = p; u32 cp = utf8_next(&q);
            float gw = glyph_w(cp, sc);
            if (w + gw > lw && n) break;
            if (cp == ' ') { last_sp = q; last_n = n; }
            memcpy(line + n, p, q - p); n += (int)(q - p); w += gw; p = q;
            if (n > 280) break;
        }
        if (*p && *p != '\n' && last_sp) { n = last_n; p = last_sp; }
        line[n] = 0;
        if (*p == '\n') p++;
        while (*p == ' ') p++;
        text_draw_center(line, BOT_W / 2, y + 44 + l * text_lineh(sc), sc, T.text3);
    }
    float by = y + DLG_H - DLG_BH - 14;
    dlg_button(GLYPH_B, tr("Cancelar"), DLG_X + 14, by, T.text, T.surface2);
    dlg_button(GLYPH_A, g_dialog.ok, DLG_X + DLG_W - 14 - DLG_BW, by, 0xFFFFFFFF, g_dialog.danger ? T.danger : T.accent);
}

// ------------------------------------------------------------ teclado
bool kb_input(const char *hint, const char *initial, char *out, int outsz, bool multiline, bool password) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, outsz > 2000 ? 2000 : outsz - 1);
    swkbdSetHintText(&kb, tr(hint));
    if (initial) swkbdSetInitialText(&kb, initial);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, tr("Cancelar"), false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, tr("Aceptar"), true);
    u32 feat = SWKBD_DARKEN_TOP_SCREEN | SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET | SWKBD_ALLOW_POWER;
    if (multiline) feat |= SWKBD_MULTILINE;
    swkbdSetFeatures(&kb, feat);
    if (password) swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetValidation(&kb, SWKBD_ANYTHING, 0, 0);
    char *buf = calloc(1, outsz);
    SwkbdButton b = swkbdInputText(&kb, buf, outsz);
    bool ok = b == SWKBD_BUTTON_RIGHT;
    if (ok) snprintf(out, outsz, "%s", buf);
    free(buf);
    return ok;
}

bool kb_pin(const char *hint, char *out, int outsz) {
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NUMPAD, 2, 4);
    swkbdSetHintText(&kb, tr(hint));
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, tr("Cancelar"), false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, tr("Aceptar"), true);
    swkbdSetFeatures(&kb, SWKBD_DARKEN_TOP_SCREEN | SWKBD_FIXED_WIDTH);
    swkbdSetValidation(&kb, SWKBD_FIXEDLEN, 0, 0);
    char buf[8] = "";
    SwkbdButton b = swkbdInputText(&kb, buf, sizeof buf);
    if (b != SWKBD_BUTTON_RIGHT || strlen(buf) != 4 || strspn(buf, "0123456789") != 4) return false;
    snprintf(out, outsz, "%s", buf);
    return true;
}

// ------------------------------------------------------------ QR
static char s_qr_text[3000];
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(27)];
static bool s_qr_ok;

static void qr_prepare(const char *text) {
    if (!strcmp(text, s_qr_text) && s_qr_text[0]) return;
    snprintf(s_qr_text, sizeof(s_qr_text), "%s", text);
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(27)];
    // versión mínima 4: los enlaces cortos (GitHub, Discord, Ko-fi…) salen todos del mismo tamaño
    s_qr_ok = qrcodegen_encodeText(text, tmp, s_qr, qrcodegen_Ecc_LOW, 4, 27, qrcodegen_Mask_AUTO, true);
}

int qr_size_for(const char *text) {
    qr_prepare(text);
    return s_qr_ok ? qrcodegen_getSize(s_qr) : 0;
}

bool draw_qr(const char *text, float x, float y, float maxsize, float alpha) {
    qr_prepare(text);
    if (!s_qr_ok) return false;
    int n = qrcodegen_getSize(s_qr);
    int quiet = 2;
    float m = floorf(maxsize / (n + quiet * 2));
    if (m < 1) m = 1;
    float total = m * (n + quiet * 2);
    float ox = x + (maxsize - total) / 2, oy = y + (maxsize - total) / 2;
    draw_rrect(ox - 4, oy - 4, total + 8, total + 8, 10, col_alpha(0xFFFFFFFF, alpha));
    u32 black = col_alpha(C2D_Color32(0x1F, 0x1F, 0x1F, 0xFF), alpha);
    for (int yy = 0; yy < n; yy++) {
        int run = -1;
        for (int xx = 0; xx <= n; xx++) {
            bool on = xx < n && qrcodegen_getModule(s_qr, xx, yy);
            if (on && run < 0) run = xx;
            if (!on && run >= 0) {
                C2D_DrawRectSolid(ox + (quiet + run) * m, oy + (quiet + yy) * m, 0.5f, (xx - run) * m, m, black);
                run = -1;
            }
        }
    }
    return true;
}
