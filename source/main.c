#include "app.h"
#include "fsx.h"
#include "gfx.h"
#include "i18n.h"
#include "server.h"
#include "state.h"
#include "ui.h"
#include "util.h"

#include "qrcodegen.h"
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { TAB_ACTIVITY, TAB_DEVICES, TAB_SETTINGS, TAB_COUNT };
enum { ACT_NONE, ACT_REVOKE, ACT_NEW_PIN, ACT_EXIT };
enum { SET_READONLY, SET_PROTECT, SET_PIN, SET_LANG, SET_THEME, SET_ABOUT, SET_EXIT, SET_COUNT };

#define TABBAR_H 44
#define VIEW_Y   34
#define VIEW_H   (SCR_H - TABBAR_H - VIEW_Y)

static struct {
	int tab;
	float tab_anim;
	Scroll sc[TAB_COUNT];
	Dialog dlg;
	float t_ro, t_prot; /* animación de los interruptores */
	float busy_anim;    /* 0 = QR, 1 = transferencias */
	u64 busy_until;     /* mantener la vista de transferencias un momento al acabar */
	bool about;         /* QR del Discord arriba */
	float about_anim;
	bool quit;

	/* QR */
	u8 qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	bool qr_ok;
	char qr_text[96];
	u8 qr2[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	bool qr2_ok;

	/* copia del estado para dibujar sin el cerrojo */
	AppState S;
	u32 last_rev;
	int last_hist_n, last_hist_head;
	bool events_seen; /* el historial cargado de la SD no genera avisos */
	bool lid_off;
	float time;
} U;

static void make_qr(u8 *out, bool *ok, const char *text)
{
	u8 tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
	*ok = qrcodegen_encodeText(text, tmp, out, qrcodegen_Ecc_MEDIUM, 4, 10, qrcodegen_Mask_AUTO, true);
}

static void draw_qr(const u8 *qr, float x, float y, float size, float a)
{
	int n = qrcodegen_getSize(qr);
	int quiet = 2;
	float m = floorf(size / (n + quiet * 2));
	float real = m * (n + quiet * 2);
	x += floorf((size - real) / 2);
	y += floorf((size - real) / 2);
	gfx_rrect(x, y, real, real, 8, gfx_alpha(0xFFFFFFFF, a));
	u32 dark = gfx_alpha(C2D_Color32(0x0D, 0x12, 0x1C, 0xFF), a);
	for (int r = 0; r < n; r++) {
		/* tramos horizontales seguidos: menos objetos que dibujar */
		int c = 0;
		while (c < n) {
			if (!qrcodegen_getModule(qr, c, r)) {
				c++;
				continue;
			}
			int s = c;
			while (c < n && qrcodegen_getModule(qr, c, r))
				c++;
			gfx_rect(x + (s + quiet) * m, y + (r + quiet) * m, (c - s) * m, m, dark);
		}
	}
}

static void url_text(char *out, size_t n, bool with_scheme)
{
	struct in_addr a = {U.S.ip};
	if (U.S.port == 80)
		snprintf(out, n, "%s%s", with_scheme ? "http://" : "", inet_ntoa(a));
	else
		snprintf(out, n, "%s%s:%d", with_scheme ? "http://" : "", inet_ntoa(a), U.S.port);
}

static void refresh_qr(void)
{
	char url[64], text[96];
	url_text(url, sizeof url, true);
	snprintf(text, sizeof text, "%s/#k=%s", url, U.S.qr_key);
	if (strcmp(text, U.qr_text) != 0) {
		util_strlcpy(U.qr_text, text, sizeof U.qr_text);
		make_qr(U.qr, &U.qr_ok, text);
	}
}

/* ---------- cabecera superior ---------- */

static void draw_header(void)
{
	gfx_icon(IC_CLOUD, 20, 14, 20, T.accent);
	gfx_title(36, 5, 15, T.text);

	bool chg;
	int pct = util_battery(&chg);
	char pt[8];
	snprintf(pt, sizeof pt, "%d%%", pct);
	float x = TOP_W - 12;
	x -= gfx_text(x, 6, 12, T.muted, AL_RIGHT, 0, pt) + 6;
	float bw = 22, bh = 11, by = 8;
	x -= bw + 3;
	u32 bc = chg ? T.ok : pct < 15 ? T.err : pct < 30 ? T.warn : T.ok;
	gfx_rrect(x, by, bw, bh, 3, T.muted);
	gfx_rect(x + 1.5f, by + 1.5f, bw - 3, bh - 3, T.bg);
	gfx_rect(x + 2.5f, by + 2.5f, (bw - 5) * pct / 100.0f, bh - 5, bc);
	gfx_rect(x + bw, by + 3, 2, bh - 6, T.muted);
	x -= 10;
	char clk[48];
	util_fmt_clock(clk, sizeof clk);
	x -= gfx_text(x, 6, 12, T.muted, AL_RIGHT, 0, clk) + 10;

	/* señal Wi-Fi */
	int bars = U.S.ip ? osGetWifiStrength() : 0;
	for (int i = 0; i < 3; i++) {
		float h = 4 + i * 3;
		gfx_rrect(x - 14 + i * 5, 17 - h, 3, h, 1, i < bars ? T.text : T.line);
	}
}

/* ---------- pantalla superior: conexión ---------- */

static void draw_connect(float a, float off)
{
	float cx = 12, cy = 32 + off, cw = TOP_W - 24, ch = 196;
	gfx_rrect(cx, cy, cw, ch, 16, gfx_alpha(T.card, a));
	if (U.qr_ok)
		draw_qr(U.qr, cx + 12, cy + 12, 172, a);

	float x = cx + 198, w = cw - 198 - 14;
	u32 tx = gfx_alpha(T.text, a), mu = gfx_alpha(T.muted, a);
	gfx_text(x, cy + 18, 16, tx, AL_LEFT, w, _("Escanea con tu móvil"));
	gfx_wrap(x, cy + 42, 11.5f, mu, AL_LEFT, w, _("La cámara abre tu nube al instante. Mismo Wi-Fi que la consola."), 3);

	gfx_rect(x, cy + 92, w, 1, gfx_alpha(T.line, a));
	gfx_text(x, cy + 102, 11.5f, mu, AL_LEFT, w, _("O escribe en el navegador:"));
	char url[48];
	url_text(url, sizeof url, false);
	float us = 19;
	while (us > 13 && gfx_text_w(us, url) > w)
		us -= 1;
	gfx_text(x, cy + 120, us, gfx_alpha(T.accent, a), AL_LEFT, w, url);

	gfx_text(x, cy + 150, 11.5f, mu, AL_LEFT, w, _("PIN"));
	for (int i = 0; i < 4; i++) {
		float bx = x + 34 + i * 30, by = cy + 144;
		gfx_rrect(bx, by, 26, 30, 7, gfx_alpha(T.card2, a));
		char d[2] = {U.S.pin[i], 0};
		gfx_text(bx + 13, by + 6, 17, tx, AL_CENTER, 0, d);
	}
}

static void draw_center_msg(Icon ic, u32 ic_color, const char *title, const char *text, float a, float off)
{
	float cx = 12, cy = 32 + off, cw = TOP_W - 24, ch = 196;
	gfx_rrect(cx, cy, cw, ch, 16, gfx_alpha(T.card, a));
	gfx_circle(TOP_W / 2, cy + 62, 34, gfx_alpha(T.card2, a));
	gfx_icon(ic, TOP_W / 2, cy + 62, 40, gfx_alpha(ic_color, a));
	gfx_text(TOP_W / 2, cy + 108, 17, gfx_alpha(T.text, a), AL_CENTER, cw - 40, title);
	gfx_wrap(cx + 40, cy + 134, 12, gfx_alpha(T.muted, a), AL_CENTER, cw - 80, text, 3);
}

static void draw_spinner(float cx, float cy, float r, float a)
{
	for (int i = 0; i < 8; i++) {
		float ang = U.time * 6 + i * M_PI / 4;
		float al = (i + 1) / 8.0f;
		gfx_circle(cx + cosf(ang) * r, cy + sinf(ang) * r, 2.6f, gfx_alpha(T.accent, a * al));
	}
}

/* ---------- pantalla superior: transferencias ---------- */

static int collect_xfers(const Xfer **out, int max)
{
	int n = 0;
	u64 now = util_ms();
	for (int i = 0; i < MAX_XFERS && n < max; i++) {
		const Xfer *x = &U.S.xfers[i];
		if (x->st == XS_RUNNING || (x->st != XS_FREE && now - x->t_end < 2500))
			out[n++] = x;
	}
	return n;
}

static void draw_xfer(const Xfer *x, float y, float h, float a)
{
	float cx = 12, cw = TOP_W - 24;
	gfx_rrect(cx, y, cw, h, 16, gfx_alpha(T.card, a));
	bool up = x->kind == XF_UPLOAD;
	u32 col = x->st == XS_FAILED || x->st == XS_CANCELED ? T.err : up ? T.ok : T.accent;
	float ix = cx + 30, iy = y + h / 2 - (h > 80 ? 10 : 0);
	gfx_circle(ix, iy, 18, gfx_alpha(col, a));
	Icon ic = x->st == XS_DONE ? IC_CHECK : x->st == XS_RUNNING ? (up ? IC_UP : IC_DOWN) : IC_CROSS;
	gfx_icon(ic, ix, iy, 20, gfx_alpha(T.on_accent, a));

	float tx = cx + 60, tw = cw - 72;
	float ty = y + (h - 64) / 2;
	gfx_text(tx, ty, 15, gfx_alpha(T.text, a), AL_LEFT, tw, x->name);
	char sub[96];
	if (x->st == XS_DONE)
		snprintf(sub, sizeof sub, up ? _("Recibido de %s") : _("Enviado a %s"), x->device);
	else if (x->st == XS_RUNNING)
		snprintf(sub, sizeof sub, up ? _("Recibiendo de %s") : _("Enviando a %s"), x->device);
	else
		snprintf(sub, sizeof sub, "%s", _("Cancelado o conexión perdida"));
	gfx_text(tx, ty + 19, 11.5f, gfx_alpha(T.muted, a), AL_LEFT, tw, sub);

	float frac = x->total ? (double)x->done / x->total : (x->st == XS_DONE ? 1 : 0);
	gfx_brand_bar(tx, ty + 38, tw, 8, frac);

	char d[24], t[24], line[80];
	util_fmt_size(d, sizeof d, x->done);
	util_fmt_size(t, sizeof t, x->total);
	snprintf(line, sizeof line, _("%s de %s"), d, t);
	gfx_text(tx, ty + 51, 11, gfx_alpha(T.muted, a), AL_LEFT, tw / 2, line);
	if (x->st == XS_RUNNING && x->speed > 0) {
		char sp[24], eta[24];
		util_fmt_speed(sp, sizeof sp, x->speed);
		util_fmt_eta(eta, sizeof eta, (x->total - x->done) / (x->speed ? x->speed : 1));
		snprintf(line, sizeof line, _("%s · quedan %s"), sp, eta);
		gfx_text(tx + tw, ty + 51, 11, gfx_alpha(T.muted, a), AL_RIGHT, tw / 2, line);
	} else if (x->st == XS_DONE) {
		gfx_text(tx + tw, ty + 51, 11, gfx_alpha(T.ok, a), AL_RIGHT, tw / 2, _("Completado"));
	}
}

static void draw_busy(float a, float off)
{
	const Xfer *xs[3];
	int n = collect_xfers(xs, 3);
	if (!n)
		return;
	float top = 32 + off, avail = 172;
	float h = n == 1 ? 110 : (avail - (n - 1) * 8) / n;
	float y = top + (n == 1 ? (avail - h) / 2 : 0);
	for (int i = 0; i < n; i++)
		draw_xfer(xs[i], y + i * (h + 8), h, a);
	/* para que otro dispositivo pueda conectarse mientras tanto */
	char url[48], foot[96];
	url_text(url, sizeof url, false);
	snprintf(foot, sizeof foot, _("%s · PIN %s"), url, U.S.pin);
	gfx_text(TOP_W / 2, top + avail + 8, 11.5f, gfx_alpha(T.muted, a), AL_CENTER, 360, foot);
}

static void draw_about_top(float a)
{
	float cx = 12, cy = 32, cw = TOP_W - 24, ch = 196;
	gfx_rrect(cx, cy, cw, ch, 16, gfx_alpha(T.card, a));
	if (U.qr2_ok)
		draw_qr(U.qr2, cx + 12, cy + 12, 172, a);
	float x = cx + 198, w = cw - 212;
	gfx_title(x, cy + 18, 20, gfx_alpha(T.text, a));
	gfx_text(x, cy + 46, 12, gfx_alpha(T.muted, a), AL_LEFT, w, _("Tu nube local"));
	gfx_text(x, cy + 64, 12, gfx_alpha(T.muted, a), AL_LEFT, w, "v" APP_VERSION);
	gfx_rect(x, cy + 90, w, 1, gfx_alpha(T.line, a));
	gfx_icon(IC_HEART, x + 8, cy + 110, 14, gfx_alpha(T.err, a));
	gfx_text(x + 20, cy + 102, 12.5f, gfx_alpha(T.text, a), AL_LEFT, w - 20, _("hecho por VENOMSHiNE"));
	gfx_wrap(x, cy + 128, 11.5f, gfx_alpha(T.muted, a), AL_LEFT, w, _("Escanea para unirte al Discord de la comunidad."), 3);
}

static void draw_top(float dt)
{
	gfx_target_top();
	draw_header();

	bool busy = collect_xfers((const Xfer *[3]){0}, 3) > 0;
	U.busy_anim = approach(U.busy_anim, busy ? 1 : 0, 9, dt);
	U.about_anim = approach(U.about_anim, U.about ? 1 : 0, 10, dt);
	float b = ease_in_out(U.busy_anim);

	if (U.about_anim > 0.01f) {
		draw_about_top(U.about_anim);
	} else if (!U.S.ip) {
		draw_center_msg(IC_WIFI_OFF, T.muted, _("Sin conexión Wi-Fi"),
		                _("Conecta la consola a tu Wi-Fi. Si no tienes router, usa el punto de acceso de tu móvil."),
		                1, 0);
	} else if (U.S.srv_error[0]) {
		draw_center_msg(IC_ALERT, T.warn, _("No se pudo abrir el servidor"),
		                _("Cierra otras apps de red (como FTPD) y vuelve a abrir Cloud3DS."), 1, 0);
	} else if (!U.S.running) {
		gfx_rrect(12, 32, TOP_W - 24, 196, 16, T.card);
		draw_spinner(TOP_W / 2, 110, 16, 1);
		gfx_text(TOP_W / 2, 148, 15, T.muted, AL_CENTER, 0, _("Preparando tu nube…"));
	} else {
		if (b < 0.99f)
			draw_connect(1 - b, -b * 14);
		if (b > 0.01f)
			draw_busy(b, (1 - b) * 14);
	}
	toast_draw(dt);
}

/* ---------- pantalla inferior ---------- */

static void tab_header(const char *title, const char *right)
{
	gfx_text(14, 9, 16, T.text, AL_LEFT, 180, title);
	if (right)
		gfx_text(BOT_W - 14, 12, 11.5f, T.muted, AL_RIGHT, 130, right);
}

/* «↑ 24 MB  ↓ 3 MB» con íconos: la fuente del sistema no tiene flechas */
static void totals_header(u64 up, u64 down)
{
	char a[24], b[24];
	util_fmt_size(a, sizeof a, up);
	util_fmt_size(b, sizeof b, down);
	float x = BOT_W - 14;
	x -= gfx_text(x, 12, 11.5f, T.muted, AL_RIGHT, 0, b) + 3;
	gfx_icon(IC_DOWN, x - 6, 19, 11, T.accent);
	x -= 22;
	x -= gfx_text(x, 12, 11.5f, T.muted, AL_RIGHT, 0, a) + 3;
	gfx_icon(IC_UP, x - 6, 19, 11, T.ok);
}

static void row_highlight(int i, float y, float h, Scroll *s)
{
	if (IN.used_buttons && i == s->sel && !U.dlg.open)
		gfx_rrect(6, y + 1, BOT_W - 12, h - 2, 10, T.card2);
}

static void empty_state(Icon ic, const char *title, const char *text)
{
	float cy = VIEW_Y + VIEW_H / 2 - 26;
	gfx_circle(BOT_W / 2, cy, 24, T.card);
	gfx_icon(ic, BOT_W / 2, cy, 26, T.muted);
	gfx_text(BOT_W / 2, cy + 34, 14, T.text, AL_CENTER, 280, title);
	gfx_wrap(30, cy + 54, 11.5f, T.muted, AL_CENTER, BOT_W - 60, text, 3);
}

static void ev_text(const Event *e, char *title, size_t tn, char *sub, size_t sn, Icon *ic, u32 *col)
{
	char sz[24], when[24];
	util_fmt_size(sz, sizeof sz, e->size);
	util_fmt_when(when, sizeof when, e->when);
	const char *verb = "";
	util_strlcpy(title, e->name, tn);
	*col = T.accent;
	switch (e->kind) {
	case EV_UPLOAD:      verb = _("Recibido"); *ic = IC_UP; *col = T.ok; break;
	case EV_DOWNLOAD:    verb = _("Enviado"); *ic = IC_DOWN; break;
	case EV_UPLOAD_FAIL: verb = _("No se completó"); *ic = IC_CROSS; *col = T.err; break;
	case EV_MKDIR:       verb = _("Carpeta creada"); *ic = IC_FOLDER; *col = T.warn; break;
	case EV_RENAME:      verb = _("Renombrado"); *ic = IC_PENCIL; *col = T.accent2; break;
	case EV_DELETE:      verb = _("Eliminado"); *ic = IC_TRASH; *col = T.err; break;
	case EV_PAIRED:
		util_strlcpy(title, _("Nuevo dispositivo vinculado"), tn);
		verb = "";
		*ic = IC_LINK;
		*col = T.accent2;
		break;
	case EV_REVOKED:
		util_strlcpy(title, _("Dispositivo expulsado"), tn);
		*ic = IC_CROSS;
		*col = T.muted;
		break;
	case EV_PIN_RESET:
		util_strlcpy(title, _("PIN cambiado por seguridad"), tn);
		util_strlcpy(sub, _("Demasiados intentos fallidos"), sn);
		*ic = IC_LOCK;
		*col = T.warn;
		return;
	}
	if (e->kind == EV_UPLOAD || e->kind == EV_DOWNLOAD || e->kind == EV_UPLOAD_FAIL)
		snprintf(sub, sn, "%s · %s · %s", verb, sz, e->device);
	else if (*verb)
		snprintf(sub, sn, "%s · %s", verb, e->device);
	else
		snprintf(sub, sn, "%s", e->device);
	(void)when;
}

static void tab_activity(float dt)
{
	Scroll *s = &U.sc[TAB_ACTIVITY];
	tab_header(_("Actividad"), NULL);
	if (U.S.stat_up || U.S.stat_down)
		totals_header(U.S.stat_up, U.S.stat_down);
	int n = U.S.hist_n;
	const float RH = 42;
	scroll_update(s, n, RH, VIEW_H, dt);
	if (!n) {
		empty_state(IC_CLOUD, _("Todo listo"), _("Sube algo desde tu móvil o tu PC y aparecerá aquí."));
		return;
	}
	gfx_scissor(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < n; i++) {
		float y = VIEW_Y + i * RH - s->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		/* el anillo de la copia está en U.S */
		const Event *e = &U.S.hist[(U.S.hist_head - i + MAX_HISTORY) % MAX_HISTORY];
		char title[128], sub[160] = "", when[24];
		Icon ic = IC_INFO;
		u32 col;
		ev_text(e, title, sizeof title, sub, sizeof sub, &ic, &col);
		util_fmt_when(when, sizeof when, e->when);
		row_highlight(i, y, RH, s);
		gfx_circle(28, y + RH / 2, 13, col);
		gfx_icon(ic, 28, y + RH / 2, 15, T.on_accent);
		float ww = gfx_text_w(10.5f, when);
		gfx_text(50, y + 6, 13.5f, T.text, AL_LEFT, BOT_W - 66 - ww - 8, title);
		gfx_text(50, y + 23, 10.5f, T.muted, AL_LEFT, BOT_W - 64, sub);
		gfx_text(BOT_W - 14, y + 8, 10.5f, T.muted, AL_RIGHT, 0, when);
	}
	gfx_scissor_off();
}

static int device_list(int *idx)
{
	int n = 0;
	/* los conectados primero, luego por última vez visto */
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < MAX_DEVICES; i++) {
			const Device *d = &U.S.dev[i];
			if (!d->used)
				continue;
			bool on = d->last_ms && util_ms() - d->last_ms < 30000;
			if ((pass == 0) == on)
				idx[n++] = i;
		}
	return n;
}

static void tab_devices(float dt)
{
	Scroll *s = &U.sc[TAB_DEVICES];
	int idx[MAX_DEVICES];
	int n = device_list(idx);
	int online = 0;
	for (int i = 0; i < n; i++)
		if (U.S.dev[idx[i]].last_ms && util_ms() - U.S.dev[idx[i]].last_ms < 30000)
			online++;
	char right[48];
	snprintf(right, sizeof right, online == 1 ? _("%d conectado") : _("%d conectados"), online);
	tab_header(_("Dispositivos"), n ? right : NULL);
	const float RH = 46;
	scroll_update(s, n, RH, VIEW_H, dt);
	if (!n) {
		empty_state(IC_DEVICES, _("Sin dispositivos"), _("Escanea el QR de arriba con tu móvil para vincularlo."));
		return;
	}
	if (!U.dlg.open && IN.used_buttons && (IN.down & (KEY_A | KEY_X)) && s->sel < n) {
		const Device *d = &U.S.dev[idx[s->sel]];
		char t[200];
		snprintf(t, sizeof t, _("%s tendrá que volver a escanear el QR o escribir el PIN."), d->name);
		dialog_open(&U.dlg, _("¿Expulsar dispositivo?"), t, _("Expulsar"), true, ACT_REVOKE, idx[s->sel]);
	}
	gfx_scissor(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < n; i++) {
		float y = VIEW_Y + i * RH - s->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		const Device *d = &U.S.dev[idx[i]];
		bool on = d->last_ms && util_ms() - d->last_ms < 30000;
		row_highlight(i, y, RH, s);
		Icon ic = d->kind == 0 ? IC_PHONE : d->kind == 2 ? IC_TABLET : IC_LAPTOP;
		gfx_circle(30, y + RH / 2, 15, T.card);
		gfx_icon(ic, 30, y + RH / 2, 20, on ? T.accent : T.muted);
		if (on) {
			gfx_circle(41, y + RH / 2 + 10, 5, T.bg);
			gfx_circle(41, y + RH / 2 + 10, 3.5f, T.ok);
		}
		gfx_text(54, y + 7, 13.5f, T.text, AL_LEFT, 170, d->name);
		char sub[64], when[24];
		if (on)
			snprintf(sub, sizeof sub, "%s · %s", _("Conectado ahora"), d->ip);
		else {
			util_fmt_when(when, sizeof when, d->last_seen);
			snprintf(sub, sizeof sub, _("Visto: %s"), when);
		}
		gfx_text(54, y + 25, 10.5f, on ? T.ok : T.muted, AL_LEFT, 170, sub);
		/* botón Expulsar */
		float bw = 76, bx = BOT_W - 12 - bw, by = y + 9;
		bool pr = ui_pressing(bx, by, bw, 28);
		gfx_rrect(bx, by, bw, 28, 14, pr ? gfx_alpha(T.err, 0.35f) : gfx_alpha(T.err, 0.16f));
		gfx_text(bx + bw / 2, by + 7, 11.5f, T.err, AL_CENTER, bw - 10, _("Expulsar"));
		if (!U.dlg.open && ui_tap(bx, by, bw, 28) && y >= VIEW_Y - 4 && y + RH <= VIEW_Y + VIEW_H + 4) {
			char t[200];
			snprintf(t, sizeof t, _("%s tendrá que volver a escanear el QR o escribir el PIN."), d->name);
			dialog_open(&U.dlg, _("¿Expulsar dispositivo?"), t, _("Expulsar"), true, ACT_REVOKE, idx[i]);
		}
	}
	gfx_scissor_off();
}

static const char *lang_name(int l)
{
	switch (l) {
	case 1: return "Español";
	case 2: return "English";
	case 3: return "日本語";
	default: return _("Sistema");
	}
}

static void apply_setting(int i)
{
	state_lock();
	switch (i) {
	case SET_READONLY:
		G.readonly = !G.readonly;
		G.dirty_cfg = true;
		break;
	case SET_PROTECT:
		G.protect_sys = !G.protect_sys;
		G.dirty_cfg = true;
		break;
	case SET_LANG:
		G.lang = (G.lang + 1) % 4;
		i18n_init(G.lang);
		G.dirty_cfg = true;
		break;
	case SET_THEME:
		G.light = !G.light;
		gfx_set_theme(G.light);
		G.dirty_cfg = true;
		break;
	}
	G.rev++;
	state_unlock();
	if (i == SET_PIN)
		dialog_open(&U.dlg, _("¿Crear un PIN nuevo?"),
		            _("El PIN y el QR actuales dejarán de servir. Los dispositivos ya vinculados siguen conectados."),
		            _("Cambiar"), false, ACT_NEW_PIN, 0);
	else if (i == SET_ABOUT)
		U.about = !U.about;
	else if (i == SET_EXIT)
		U.quit = true;
}

static void tab_settings(float dt)
{
	Scroll *s = &U.sc[TAB_SETTINGS];
	tab_header(_("Ajustes"), NULL);
	const float RH = 44;
	scroll_update(s, SET_COUNT, RH, VIEW_H, dt);
	U.t_ro = approach(U.t_ro, U.S.readonly ? 1 : 0, 14, dt);
	U.t_prot = approach(U.t_prot, U.S.protect_sys ? 1 : 0, 14, dt);

	if (!U.dlg.open && IN.used_buttons && (IN.down & KEY_A))
		apply_setting(s->sel);

	gfx_scissor(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < SET_COUNT; i++) {
		float y = VIEW_Y + i * RH - s->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		row_highlight(i, y, RH, s);
		if (ui_pressing(6, y, BOT_W - 12, RH) && y >= VIEW_Y)
			gfx_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.card);
		const char *title = "", *sub = NULL, *val = NULL;
		char vbuf[32];
		Icon ic = IC_GEAR;
		u32 col = T.accent;
		switch (i) {
		case SET_READONLY:
			title = _("Solo lectura");
			sub = _("Solo se podrá ver y descargar");
			ic = IC_LOCK;
			col = T.warn;
			break;
		case SET_PROTECT:
			title = _("Proteger carpetas del sistema");
			sub = _("Pide confirmación en Nintendo 3DS, luma…");
			ic = IC_ALERT;
			col = T.err;
			break;
		case SET_PIN:
			title = _("PIN nuevo");
			sub = _("Invalida el QR y el PIN actuales");
			snprintf(vbuf, sizeof vbuf, "%s", U.S.pin);
			val = vbuf;
			ic = IC_KEY;
			col = T.accent2;
			break;
		case SET_LANG:
			title = _("Idioma");
			val = lang_name(U.S.lang);
			ic = IC_GLOBE;
			break;
		case SET_THEME:
			title = _("Tema");
			val = U.S.light ? _("Claro") : _("Oscuro");
			ic = IC_MOON;
			col = T.accent2;
			break;
		case SET_ABOUT:
			title = _("Acerca de Cloud3DS");
			sub = U.about ? _("Mira la pantalla de arriba") : "v" APP_VERSION;
			ic = IC_INFO;
			col = T.muted;
			break;
		case SET_EXIT:
			title = _("Salir");
			sub = _("También con START");
			ic = IC_POWER;
			col = T.err;
			break;
		}
		gfx_circle(28, y + RH / 2, 13, col);
		gfx_icon(ic, 28, y + RH / 2, 15, T.on_accent);
		float right = BOT_W - 16;
		if (i == SET_READONLY || i == SET_PROTECT) {
			ui_toggle(right - 38, y + RH / 2 - 11, false, i == SET_READONLY ? U.t_ro : U.t_prot);
			right -= 48;
		} else if (val) {
			right -= gfx_text(right, y + RH / 2 - 8, 13, T.muted, AL_RIGHT, 110, val) + 10;
		}
		float tw = right - 50;
		if (sub) {
			gfx_text(50, y + 6, 13.5f, T.text, AL_LEFT, tw, title);
			gfx_text(50, y + 24, 10.5f, T.muted, AL_LEFT, tw, sub);
		} else {
			gfx_text(50, y + RH / 2 - 9, 13.5f, T.text, AL_LEFT, tw, title);
		}
		if (!U.dlg.open && ui_tap(6, y, BOT_W - 12, RH) && IN.sy >= VIEW_Y && IN.sy < VIEW_Y + VIEW_H) {
			s->sel = i;
			apply_setting(i);
		}
	}
	gfx_scissor_off();
}

static void draw_tabbar(float dt)
{
	float y = SCR_H - TABBAR_H;
	gfx_rect(0, y, BOT_W, TABBAR_H, T.card);
	gfx_rect(0, y, BOT_W, 1, T.line);
	float tw = BOT_W / (float)TAB_COUNT;
	U.tab_anim = approach(U.tab_anim, U.tab, 16, dt);
	gfx_rrect(U.tab_anim * tw + tw / 2 - 16, y + 2, 32, 3, 1.5f, T.accent);
	static const Icon ics[TAB_COUNT] = {IC_LIST, IC_DEVICES, IC_GEAR};
	const char *names[TAB_COUNT] = {_("Actividad"), _("Dispositivos"), _("Ajustes")};
	for (int i = 0; i < TAB_COUNT; i++) {
		float k = clampf(1 - fabsf(U.tab_anim - i), 0, 1);
		u32 c = gfx_mix(T.muted, T.accent, k);
		gfx_icon(ics[i], i * tw + tw / 2, y + 16, 17, c);
		gfx_text(i * tw + tw / 2, y + 27, 10.5f, c, AL_CENTER, tw - 8, names[i]);
		if (!U.dlg.open && ui_tap(i * tw, y, tw, TABBAR_H))
			U.tab = i;
	}
}

static void draw_bottom(float dt)
{
	gfx_target_bot();
	if (!U.dlg.open) {
		if (IN.down & (KEY_R | KEY_ZR))
			U.tab = (U.tab + 1) % TAB_COUNT;
		if (IN.down & (KEY_L | KEY_ZL))
			U.tab = (U.tab + TAB_COUNT - 1) % TAB_COUNT;
		Scroll *s = &U.sc[U.tab];
		if (IN.down & (KEY_DOWN | KEY_CPAD_DOWN))
			s->sel++;
		if (IN.down & (KEY_UP | KEY_CPAD_UP))
			s->sel--;
		if (IN.down & (KEY_UP | KEY_DOWN | KEY_CPAD_UP | KEY_CPAD_DOWN)) {
			/* que la selección quede a la vista */
			float rh = U.tab == TAB_ACTIVITY ? 42 : U.tab == TAB_DEVICES ? 46 : 44;
			if (s->sel < 0)
				s->sel = 0;
			float y = s->sel * rh;
			if (y < s->target)
				s->target = y;
			if (y + rh > s->target + VIEW_H)
				s->target = y + rh - VIEW_H;
		}
		if (IN.down & KEY_B && U.about)
			U.about = false;
	}

	switch (U.tab) {
	case TAB_ACTIVITY: tab_activity(dt); break;
	case TAB_DEVICES:  tab_devices(dt); break;
	default:           tab_settings(dt); break;
	}
	draw_tabbar(dt);
	dialog_draw(&U.dlg);
}

static void run_dialog(float dt)
{
	int r = dialog_run(&U.dlg, dt);
	if (r != 1)
		return;
	state_lock();
	switch (U.dlg.action) {
	case ACT_REVOKE: {
		Device *d = &G.dev[U.dlg.arg];
		if (d->used) {
			event_add(EV_REVOKED, "", d->name, 0);
			d->used = false;
			G.dirty_cfg = true;
		}
		break;
	}
	case ACT_NEW_PIN:
		pin_regen();
		break;
	case ACT_EXIT:
		U.quit = true;
		break;
	}
	G.rev++;
	state_unlock();
	if (U.dlg.action == ACT_NEW_PIN)
		toast_show(_("PIN y QR nuevos listos"), IC_KEY, T.accent2);
}

/* avisos al llegar eventos nuevos del servidor */
static void watch_events(void)
{
	int n = U.S.hist_n, head = U.S.hist_head;
	if (n == U.last_hist_n && head == U.last_hist_head)
		return;
	if (U.events_seen && n > 0) {
		const Event *e = &U.S.hist[head];
		char msg[128];
		if (e->kind == EV_PAIRED) {
			snprintf(msg, sizeof msg, _("Vinculado: %s"), e->device);
			toast_show(msg, IC_LINK, T.accent2);
		} else if (e->kind == EV_PIN_RESET) {
			toast_show(_("PIN cambiado por seguridad"), IC_LOCK, T.warn);
		} else if (e->kind == EV_DELETE) {
			snprintf(msg, sizeof msg, _("Eliminado: %s"), e->name);
			toast_show(msg, IC_TRASH, T.err);
		}
	}
	U.last_hist_n = n;
	U.last_hist_head = head;
}

static void lid_and_sleep(void)
{
	bool busy = xfer_active() > 0;
	aptSetSleepAllowed(!busy);
	/* tapa cerrada transfiriendo: apaga la luz y sigue */
	u8 open = 1;
	PTMU_GetShellState(&open);
	bool want_off = busy && !open;
	if (want_off != U.lid_off) {
		if (R_SUCCEEDED(gspLcdInit())) {
			if (want_off)
				GSPLCD_PowerOffAllBacklights();
			else
				GSPLCD_PowerOnAllBacklights();
			gspLcdExit();
		}
		U.lid_off = want_off;
	}
}

static aptHookCookie hook;

static void apt_hook(APT_HookType t, void *p)
{
	(void)p;
	if (t == APTHOOK_ONWAKEUP)
		server_restart();
}

static void load_console_name(void)
{
	u16 name[0x10] = {0};
	char n8[48] = "";
	if (R_SUCCEEDED(cfguInit())) {
		CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name);
		cfguExit();
	}
	util_utf16_to_utf8(n8, sizeof n8, name, 10);
	if (n8[0])
		snprintf(G.console, sizeof G.console, "3DS · %s", n8);
	else
		util_strlcpy(G.console, "Nintendo 3DS", sizeof G.console);
}

int main(void)
{
	osSetSpeedupEnable(true);
	romfsInit();
	ptmuInit();
	gfx_init();
	state_init();
	config_load();
	i18n_init(G.lang);
	gfx_set_theme(G.light);
	load_console_name();
	fsx_init();
	aptHook(&hook, apt_hook, NULL);
	make_qr(U.qr2, &U.qr2_ok, LINK_DISCORD);

	server_start();

	u64 last = util_ms(), last_save = last;
	while (aptMainLoop() && !U.quit) {
		u64 now = util_ms();
		float dt = (now - last) / 1000.0f;
		if (dt > 0.1f)
			dt = 0.1f;
		last = now;
		U.time += dt;

		input_update();

		state_lock();
		if (G.rev != U.last_rev || xfer_active()) {
			memcpy(&U.S, &G, sizeof G);
			U.last_rev = G.rev;
		}
		bool dirty = G.dirty_cfg;
		state_unlock();

		if (U.S.ip && U.S.running)
			refresh_qr();
		watch_events();
		U.events_seen = true;
		lid_and_sleep();

		if (IN.down & KEY_START && !U.dlg.open) {
			if (xfer_active())
				dialog_open(&U.dlg, _("¿Salir de Cloud3DS?"),
				            _("Hay transferencias en curso y se cancelarán."), _("Salir"), true, ACT_EXIT, 0);
			else
				break;
		}
		if (U.quit && xfer_active()) {
			U.quit = false;
			dialog_open(&U.dlg, _("¿Salir de Cloud3DS?"), _("Hay transferencias en curso y se cancelarán."),
			            _("Salir"), true, ACT_EXIT, 0);
		}

		gfx_frame_begin();
		draw_top(dt);
		draw_bottom(dt);
		gfx_frame_end();
		run_dialog(dt);

		if (dirty && now - last_save > 3000 && !xfer_active()) {
			state_lock();
			config_save();
			state_unlock();
			last_save = now;
		}
	}

	server_stop();
	state_lock();
	config_save();
	state_unlock();
	if (U.lid_off && R_SUCCEEDED(gspLcdInit())) {
		GSPLCD_PowerOnAllBacklights();
		gspLcdExit();
	}
	aptUnhook(&hook);
	fsx_exit();
	gfx_exit();
	ptmuExit();
	romfsExit();
	return 0;
}
