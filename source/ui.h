/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - componentes de interfaz
#pragma once
#include "gfx.h"
#include "icons.h"

// ------------------------------------------------------------ entrada
typedef struct {
    bool down, held, up;          // eventos táctiles
    int x, y, sx, sy, px, py;     // actual, inicio, anterior
    bool drag;                    // se movió lo suficiente para ser arrastre
    float hold;                   // tiempo pulsado
    bool long_fired;
} Touch;
extern Touch g_touch;
extern u32 g_kdown, g_kheld, g_krepeat;

void input_update(void);
bool touch_in(float x, float y, float w, float h);          // pulsando dentro (para resaltar)
bool tap(float x, float y, float w, float h);               // soltó dentro sin arrastrar
bool long_press(float x, float y, float w, float h);        // mantenido 0.6 s

// ------------------------------------------------------------ botones
bool ui_icon_button(IconId ic, float cx, float cy, float r, u32 fg, u32 bg);
bool ui_pill_button(const char *label, int ic, float x, float y, float w, float h, u32 fg, u32 bg);

// ------------------------------------------------------------ hojas (listas deslizables)
enum { SR_NONE, SR_CHECK, SR_CHEV, SR_ON, SR_OFF, SR_VALUE, SR_CLEAR };
typedef struct {
    int icon; u32 icon_col;
    char label[100]; char sub[140]; char value[40];
    int right; int id; bool header; bool disabled;
    bool swipe;              // se puede deslizar (chats: → fijar, ← renombrar / eliminar)
    bool pinned;             // para el texto del botón Fijar / No fijar
} SheetItem;

enum { SW_NONE, SW_PIN, SW_RENAME, SW_DELETE };

typedef struct {
    bool open, closing, from_left;
    float anim;
    char title[64];
    SheetItem items[320];
    int n, sel, kind, ctx;
    float scroll, scroll_target;
    int chosen, held_item;
    bool hit_right;          // el último toque fue en el accesorio derecho (p. ej. la X)
    // deslizar elementos (estilo WhatsApp)
    int swipe_item;          // elemento deslizado o abierto (-1 ninguno)
    float swipe_x, swipe_rest;   // desplazamiento actual y de reposo (>0 izquierda visible, <0 derecha)
    float swipe_start;
    bool swiping;            // el arrastre actual es horizontal sobre swipe_item
    bool vscroll;            // el arrastre actual es vertical (desplaza la lista)
    int swipe_action;        // SW_* elegido (sheet_update devuelve -4)
} Sheet;

void sheet_open(Sheet *s, int kind, const char *title, bool from_left);
void sheet_close(Sheet *s);
SheetItem *sheet_add(Sheet *s, int icon, const char *label, const char *sub, int right, int id);
void sheet_header(Sheet *s, const char *label);
int  sheet_update(Sheet *s);            // índice elegido o -1; -2 = cerrado; -3 mantener; -4 acción deslizando
void sheet_draw(Sheet *s);
bool sheet_visible(Sheet *s);
SheetItem *sheet_selected(Sheet *s);

// ------------------------------------------------------------ avisos y diálogos
void toast(const char *msg);
void toast_icon(const char *msg, int ic, u32 col);   // con icono y color propios
void toast_draw_top(void);
extern int g_toast_style;  // cómo se ven los avisos (ver toast_draw_top)
float toast_header_alpha(void);

typedef struct { bool open; float anim; char title[64]; char msg[300]; char ok[24]; int ctx; bool danger; } Dialog;
extern Dialog g_dialog;
void dialog_open(int ctx, const char *title, const char *msg, const char *ok, bool danger);
int  dialog_update(void);               // 1 aceptar, 0 cancelar, -1 nada
void dialog_draw(void);

// ------------------------------------------------------------ teclado del sistema
bool kb_input(const char *hint, const char *initial, char *out, int outsz, bool multiline, bool password);
bool kb_pin(const char *hint, char *out, int outsz);   // teclado numérico, exactamente 4 cifras

// ------------------------------------------------------------ QR
bool draw_qr(const char *text, float x, float y, float maxsize, float alpha);
int  qr_size_for(const char *text);

// ------------------------------------------------------------ texto de botones (glifos del sistema)
#define GLYPH_A "\xEE\x80\x80"
#define GLYPH_B "\xEE\x80\x81"
#define GLYPH_X "\xEE\x80\x82"
#define GLYPH_Y "\xEE\x80\x83"
#define GLYPH_L "\xEE\x80\x84"
#define GLYPH_R "\xEE\x80\x85"
#define GLYPH_DPAD "\xEE\x80\x86"
