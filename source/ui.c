#include "ui.h"
#include "i18n.h"
#include "util.h"

#include <math.h>
#include <string.h>

Input IN;

void input_update(void)
{
	hidScanInput();
	IN.down = hidKeysDown();
	IN.held = hidKeysHeld();
	IN.up = hidKeysUp();
	IN.tap_released = false;
	IN.dx = IN.dy = 0;
	touchPosition tp;
	hidTouchRead(&tp);
	bool t = IN.held & KEY_TOUCH;
	if (t) {
		if (!IN.touching) {
			IN.sx = IN.tx = tp.px;
			IN.sy = IN.ty = tp.py;
			IN.moved = 0;
		} else {
			IN.dx = tp.px - IN.tx;
			IN.dy = tp.py - IN.ty;
			IN.tx = tp.px;
			IN.ty = tp.py;
			IN.moved += fabsf(IN.dx) + fabsf(IN.dy);
		}
		IN.used_buttons = false;
	} else if (IN.touching) {
		IN.tap_released = IN.moved < 10;
	}
	IN.touching = t;
	if (IN.down & ~(KEY_TOUCH))
		IN.used_buttons = true;
}

bool ui_tap(float x, float y, float w, float h)
{
	return IN.tap_released && IN.sx >= x && IN.sx < x + w && IN.sy >= y && IN.sy < y + h &&
	       IN.tx >= x && IN.tx < x + w && IN.ty >= y && IN.ty < y + h;
}

bool ui_pressing(float x, float y, float w, float h)
{
	return IN.touching && IN.moved < 10 && IN.tx >= x && IN.tx < x + w && IN.ty >= y && IN.ty < y + h;
}

void scroll_update(Scroll *s, int count, float row_h, float view_h, float dt)
{
	float max = count * row_h - view_h;
	if (max < 0)
		max = 0;
	if (IN.touching && IN.moved >= 10) {
		s->dragging = true;
		s->target -= IN.dy;
		s->off = s->target;
	} else {
		s->dragging = false;
	}
	if (s->target < 0)
		s->target = 0;
	if (s->target > max)
		s->target = max;
	s->off = approach(s->off, s->target, 18, dt);
	if (s->sel >= count)
		s->sel = count - 1;
	if (s->sel < 0)
		s->sel = 0;
}

void ui_toggle(float x, float y, bool on, float anim)
{
	float w = 38, h = 22;
	/* apagado con contraste suficiente también sobre la fila resaltada */
	u32 bg = gfx_mix(gfx_mix(T.card2, T.muted, 0.45f), T.accent, anim);
	gfx_rrect(x, y, w, h, h / 2, bg);
	float kx = x + 11 + (w - 22) * anim;
	gfx_circle(kx, y + h / 2, 8.5f, T.on_accent);
	(void)on;
}

void dialog_open(Dialog *d, const char *title, const char *text, const char *ok, bool danger, int action, int arg)
{
	d->open = true;
	d->anim = 0;
	util_strlcpy(d->title, title, sizeof d->title);
	util_strlcpy(d->text, text, sizeof d->text);
	util_strlcpy(d->ok, ok, sizeof d->ok);
	d->danger = danger;
	d->action = action;
	d->arg = arg;
}

#define DLG_X 24
#define DLG_W 272
#define DLG_Y 40
#define DLG_H 160

int dialog_run(Dialog *d, float dt)
{
	if (!d->open)
		return -1;
	d->anim = approach(d->anim, 1, 14, dt);
	float by = DLG_Y + DLG_H - 50;
	if (IN.down & KEY_B || ui_tap(DLG_X + 14, by, 116, 36)) {
		d->open = false;
		return 0;
	}
	if (IN.down & KEY_A || ui_tap(DLG_X + DLG_W - 130, by, 116, 36)) {
		d->open = false;
		return 1;
	}
	/* tocar fuera también cancela */
	if (IN.tap_released && !(IN.sx >= DLG_X && IN.sx < DLG_X + DLG_W && IN.sy >= DLG_Y && IN.sy < DLG_Y + DLG_H)) {
		d->open = false;
		return 0;
	}
	return -1;
}

void dialog_draw(Dialog *d)
{
	if (!d->open)
		return;
	float a = ease_out_cubic(d->anim);
	gfx_rect(0, 0, BOT_W, SCR_H, gfx_alpha(0xFF000000, 0.55f * a));
	float y = DLG_Y + (1 - a) * 18;
	gfx_rrect(DLG_X, y + 3, DLG_W, DLG_H, 16, T.shadow);
	gfx_rrect(DLG_X, y, DLG_W, DLG_H, 16, T.card);
	gfx_text(DLG_X + DLG_W / 2, y + 16, 16, T.text, AL_CENTER, DLG_W - 28, d->title);
	gfx_wrap(DLG_X + 18, y + 44, 12.5f, T.muted, AL_CENTER, DLG_W - 36, d->text, 4);
	float by = y + DLG_H - 50;
	bool pc = ui_pressing(DLG_X + 14, by, 116, 36), po = ui_pressing(DLG_X + DLG_W - 130, by, 116, 36);
	gfx_rrect(DLG_X + 14, by, 116, 36, 18, pc ? T.line : T.card2);
	gfx_text(DLG_X + 14 + 58, by + 10, 13.5f, T.text, AL_CENTER, 104, _("Cancelar"));
	u32 oc = d->danger ? T.err : T.accent;
	gfx_rrect(DLG_X + DLG_W - 130, by, 116, 36, 18, po ? gfx_mix(oc, 0xFF000000, 0.2f) : oc);
	gfx_text(DLG_X + DLG_W - 130 + 58, by + 10, 13.5f, T.on_accent, AL_CENTER, 104, d->ok);
	gfx_text(DLG_X + 14 + 58, by + 40, 10, gfx_alpha(T.muted, 0.8f), AL_CENTER, 0, "");
	gfx_text(DLG_X + DLG_W - 130 + 58, by + 40, 10, gfx_alpha(T.muted, 0.8f), AL_CENTER, 0, "");
}

static struct {
	char msg[128];
	Icon ic;
	u32 color;
	float t; /* segundos desde que apareció; <0 = oculto */
} toast = {.t = -1};

void toast_show(const char *msg, Icon ic, u32 color)
{
	util_strlcpy(toast.msg, msg, sizeof toast.msg);
	toast.ic = ic;
	toast.color = color;
	toast.t = 0;
}

void toast_draw(float dt)
{
	if (toast.t < 0)
		return;
	toast.t += dt;
	const float life = 3.2f;
	if (toast.t > life) {
		toast.t = -1;
		return;
	}
	float a = toast.t < 0.25f ? ease_out_cubic(toast.t / 0.25f) : clampf((life - toast.t) / 0.4f, 0, 1);
	float w = gfx_text_w(13, toast.msg) + 52;
	if (w > 360)
		w = 360;
	float x = (TOP_W - w) / 2, y = SCR_H - 44 + (1 - a) * 12;
	gfx_rrect(x, y + 2, w, 32, 16, gfx_alpha(T.shadow, a));
	gfx_rrect(x, y, w, 32, 16, gfx_alpha(T.card2, a));
	gfx_circle(x + 18, y + 16, 9, gfx_alpha(toast.color, a));
	gfx_icon(toast.ic, x + 18, y + 16, 11, gfx_alpha(T.on_accent, a));
	gfx_text(x + 34, y + 8, 13, gfx_alpha(T.text, a), AL_LEFT, w - 46, toast.msg);
}
