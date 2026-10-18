#pragma once

#include "gfx.h"

/* Entrada compartida por las escenas */
typedef struct {
	u32 down, held, up;
	bool touching, tap_released;
	float tx, ty;          /* posición actual */
	float sx, sy;          /* donde empezó el toque */
	float dx, dy;          /* movimiento desde el fotograma anterior */
	float moved;           /* distancia total del gesto */
	bool  used_buttons;    /* la última interacción fue con botones */
} Input;

extern Input IN;

void input_update(void);
bool ui_tap(float x, float y, float w, float h); /* toque soltado dentro, sin arrastrar */
bool ui_pressing(float x, float y, float w, float h);

/* Lista desplazable */
typedef struct {
	float off, target;
	int   sel;
	bool  dragging;
} Scroll;

void scroll_update(Scroll *s, int count, float row_h, float view_h, float dt);

/* Interruptor animado */
void ui_toggle(float x, float y, bool on, float anim);

/* Diálogo de confirmación */
typedef struct {
	bool open;
	float anim;
	char title[96];
	char text[256];
	char ok[32];
	bool danger;
	int  action;
	int  arg;
} Dialog;

void dialog_open(Dialog *d, const char *title, const char *text, const char *ok, bool danger, int action, int arg);
int  dialog_run(Dialog *d, float dt); /* -1 nada, 0 cancelado, 1 aceptado */
void dialog_draw(Dialog *d);

/* Avisos breves en la pantalla superior */
void toast_show(const char *msg, Icon ic, u32 color);
void toast_draw(float dt);
